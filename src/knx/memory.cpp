#include "memory.h"

#include <string.h>

#include "bits.h"

Memory::Memory(Platform& platform, DeviceObject& deviceObject)
    : _platform(platform), _deviceObject(deviceObject)
{}

Memory::~Memory()
{}

void Memory::readMemory()
{
    println("readMemory");

    uint8_t* flashStart = _platform.getNonVolatileMemoryStart();
    size_t flashSize = _platform.getNonVolatileMemorySize();
    if (flashStart == nullptr)
    {
        println("no user flash available;");
        return;
    }

    // Refuse a stream larger than the NVM: flashSize - metadataBlockSize is a size_t and would wrap.
    // An empty free list makes allocMemory() return nullptr, answered with E_MAX_TABLE_LENGTH_EXEEDED.
    size_t metadataBlockSize = alignToPageSize(_metadataSize);

    if (metadataBlockSize >= flashSize)
    {
        print("metadata block of ");
        print((uint32_t)metadataBlockSize);
        print(" bytes does not fit the NVM of ");
        print((uint32_t)flashSize);
        println(" bytes -- nothing is restored and no memory is managed");
        return;
    }

    printHex("RESTORED ", flashStart, _metadataSize);

    _freeList = new MemoryBlock(flashStart + metadataBlockSize, flashSize - metadataBlockSize);

    uint16_t apiVersion = 0;
    const uint8_t* buffer = popWord(apiVersion, flashStart);

    uint16_t layoutWord = 0;
    buffer = popWord(layoutWord, buffer);

    uint16_t manufacturerId = 0;
    buffer = popWord(manufacturerId, buffer);

    uint8_t hardwareType[LEN_HARDWARE_TYPE] = {0};
    buffer = popByteArray(hardwareType, LEN_HARDWARE_TYPE, buffer);

    uint16_t version = 0;
    buffer = popWord(version, buffer);

    VersionCheckResult versionCheck = FlashAllInvalid;

    // first check correct format of deviceObject-API
    if (_deviceObject.apiVersion == apiVersion) 
    {
        // The records are read back positionally, so a differing layout would be parsed field by field.
        // The product callback cannot see this: it compares the ETS application, not the build.
        if (layoutWord != layoutFingerprint())
        {
            println("stored layout belongs to a different firmware build");
            print("expected layout: ");
            print(layoutFingerprint(), HEX);
            print(", stored layout: ");
            println(layoutWord, HEX);
        }
        else if (_versionCheckCallback != 0) {
            versionCheck = _versionCheckCallback(manufacturerId, hardwareType, version);
            // callback should provide infomation about version check failure reasons
        }
        else if (_deviceObject.manufacturerId() == manufacturerId &&
                 memcmp(_deviceObject.hardwareType(), hardwareType, LEN_HARDWARE_TYPE) == 0) 
        {
            if (_deviceObject.version() == version) {
                versionCheck = FlashValid;
            } 
            else
            {
                versionCheck = FlashTablesInvalid;
            }
        } 
        else 
        {
            println("manufacturerId or hardwareType are different");
            print("expexted manufacturerId: ");
            print(_deviceObject.manufacturerId(), HEX);
            print(", stored manufacturerId: ");
            println(manufacturerId, HEX);
            print("expexted hardwareType: ");
            printHex("", _deviceObject.hardwareType(), LEN_HARDWARE_TYPE);
            print(", stored hardwareType: ");
            printHex("", hardwareType, LEN_HARDWARE_TYPE);
            println("");
        }
    } 
    else 
    {
        println("DataObject api changed, any data stored in flash is invalid.");
        print("expexted DataObject api version: ");
        print(_deviceObject.apiVersion, HEX);
        print(", stored api version: ");
        println(apiVersion, HEX);
    }

    if (versionCheck == FlashAllInvalid)
    {
        println("ETS has to reprogram PA and application!");
        return;
    }

    println("restoring data from flash...");
    print("Restore saveRestores: ");
    println(_saveCount);
    for (int i = 0; i < _saveCount; i++)
    {
        buffer = _saveRestores[i]->restore(buffer);
    }
    println("Restored saveRestores");

#if MASK_VERSION == 0x091A
    if(versionCheck == FlashTablesInvalid)
    {
        println("TableObjects are referring to an older firmware version and are restored, unloaded and filled with 0xff");
    }
#else
    if (versionCheck == FlashTablesInvalid) 
    {
        println("TableObjects are referring to an older firmware version and are not restored");
        return;
    }
#endif
    print("Restore TableObjs: ");
    println(_tableObjCount);
    for (int i = 0; i < _tableObjCount; i++)
    {
        buffer = _tableObjects[i]->restore(buffer);
        uint16_t memorySize = 0;
        buffer = popWord(memorySize, buffer);
        print("Size: ");
        println(memorySize);
        // A static table's extent is a build constant, so the persisted word may describe the previous
        // firmware's block; dynamic tables keep using the saved word.
        uint32_t blockSize = _tableObjects[i]->_staticTableAdr ? _tableObjects[i]->_size : memorySize;
        if (blockSize == 0 || _tableObjects[i]->_data == nullptr)
            continue;

        // this works because TableObject saves a relative addr and restores it itself
        addNewUsedBlock(_tableObjects[i]->_data, blockSize);

#if MASK_VERSION == 0x091A
    	// load the tables but delete the data
        if(versionCheck == FlashTablesInvalid)
        {
            println("unload and fill with 0xff");
            _tableObjects[i]->loadState(LS_UNLOADED);
            uint32_t start = toRelative(_tableObjects[i]->_data);
            uint8_t fillByte = 0xff;
            uint32_t end = start + _tableObjects[i]->_size;
            for (uint32_t addr = start; addr < end; addr++)
                writeMemory(addr, 1, &fillByte);
        }
#endif
    }
    println("restored TableObjects");
}

/**
 * @brief The word stored in the NVM header, identifying the persisted stream layout of this build.
 *
 * Every registered record contributes its kind, its length and the identity of the properties it writes,
 * in registration order, so the word changes exactly when the layout changes. It is computed on demand
 * rather than accumulated at registration: RouterObject fills its property table in initialize(), not in
 * its constructor, so a snapshot taken while registering would miss a coupler whose BAU registers first.
 */
uint16_t Memory::layoutFingerprint()
{
    uint32_t hash = 2166136261u; // FNV-1a offset basis

    // kind tag first, so a save-restore and a table object of equal shape cannot fold to the same word
    for (int i = 0; i < _saveCount; i++)
        hash = mixRecord(hash, 0x0001, _saveRestores[i]);

    for (int i = 0; i < _tableObjCount; i++)
        hash = mixRecord(hash, 0x0002, _tableObjects[i]);

    return (uint16_t)((hash >> 16) ^ (hash & 0xFFFF));
}

uint32_t Memory::mixRecord(uint32_t hash, uint16_t kind, SaveRestore* obj)
{
    const uint32_t tag = obj->layoutTag();

    hash = fnv1aWord(hash, kind);
    hash = fnv1aWord(hash, obj->saveSize());
    hash = fnv1aWord(hash, (uint16_t)(tag >> 16));
    hash = fnv1aWord(hash, (uint16_t)tag);
    return hash;
}

void Memory::writeMemory()
{
    // first get the necessary size of the writeBuffer
    uint16_t writeBufferSize = _metadataSize;
    for (int i = 0; i < _saveCount; i++)
        writeBufferSize = MAX(writeBufferSize, _saveRestores[i]->saveSize());

    for (int i = 0; i < _tableObjCount; i++)
        writeBufferSize = MAX(writeBufferSize, _tableObjects[i]->saveSize() + 2 /*for memory pos*/);
    
    // The stream is written from offset 0 with no bound below; refusing keeps the previous image.
    if (_metadataSize > memorySize())
    {
        println("metadata stream larger than the NVM -- not written");
        return;
    }

    uint8_t buffer[writeBufferSize];
    uint32_t flashPos = 0;
    uint8_t* bufferPos = buffer;

    bufferPos = pushWord(_deviceObject.apiVersion, bufferPos);
    bufferPos = pushWord(layoutFingerprint(), bufferPos);
    bufferPos = pushWord(_deviceObject.manufacturerId(), bufferPos);
    bufferPos = pushByteArray(_deviceObject.hardwareType(), LEN_HARDWARE_TYPE, bufferPos);
    bufferPos = pushWord(_deviceObject.version(), bufferPos);

    flashPos = _platform.writeNonVolatileMemory(flashPos, buffer, bufferPos - buffer);

    print("save saveRestores ");
    println(_saveCount);
    for (int i = 0; i < _saveCount; i++)
    {
        bufferPos = _saveRestores[i]->save(buffer);
        flashPos = _platform.writeNonVolatileMemory(flashPos, buffer, bufferPos - buffer);
    }

    print("save tableobjs ");
    println(_tableObjCount);
    for (int i = 0; i < _tableObjCount; i++)
    {
        bufferPos = _tableObjects[i]->save(buffer);

        //save to size of the memoryblock for tableobject too, so that we can rebuild the usedList and freeList
        if (_tableObjects[i]->_data != nullptr)
        {
            MemoryBlock* block = findBlockInList(_usedList, _tableObjects[i]->_data);
            if (block == nullptr)
            {
                println("_data of TableObject not in _usedList");
                _platform.fatalError();
            }
            bufferPos = pushWord(block->size, bufferPos);
        }
        else
            bufferPos = pushWord(0, bufferPos);

        flashPos = _platform.writeNonVolatileMemory(flashPos, buffer, bufferPos - buffer);
    }
    
    _platform.commitNonVolatileMemory();
}

void Memory::saveMemory()
{
    _platform.commitNonVolatileMemory();
}

void Memory::addSaveRestore(SaveRestore* obj)
{
    if (_saveCount >= MAXSAVE)
        return;

    _saveRestores[_saveCount] = obj;
    _saveCount += 1;
    _metadataSize += obj->saveSize();
}

void Memory::addSaveRestore(TableObject* obj)
{
    if (_tableObjCount >= MAXTABLEOBJ)
        return;

    _tableObjects[_tableObjCount] = obj;
    _tableObjCount += 1;
    _metadataSize += obj->saveSize();
    _metadataSize += 2; // for size
}

size_t Memory::memorySize()
{
    return _platform.getNonVolatileMemorySize();
}

void Memory::scheduleSave()
{
    _saveTimeout = millis();
    if (_saveTimeout == 0)
        _saveTimeout = 1; // 0 means disabled
}

uint8_t* Memory::allocMemory(size_t size)
{
    // always allocate aligned to pagesize
    size = alignToPageSize(size);

    MemoryBlock* freeBlock = _freeList;
    MemoryBlock* blockToUse = nullptr;
    
    // find the smallest possible block that is big enough
    while (freeBlock)
    {
        if (freeBlock->size >= size)
        {
            if (blockToUse != nullptr && (blockToUse->size - size) > (freeBlock->size - size))
                blockToUse = freeBlock;
            else if (blockToUse == nullptr)
                blockToUse = freeBlock;
        }
        freeBlock = freeBlock->next;
    }
    if (!blockToUse)
    {
        // allocTable() checks for null, so the load state machine can answer E_MAX_TABLE_LENGTH_EXEEDED.
        println("No available non volatile memory!");
        return nullptr;
    }

    // An allocation changes the used/free lists exactly as a free does, and both are rebuilt from the
    // persisted metadata on the next boot.
    scheduleSave();

    if (blockToUse->size == size)
    {
        // use whole block
        removeFromFreeList(blockToUse);
        addToUsedList(blockToUse);
        return blockToUse->address;
    }
    else
    {
        // split block
        MemoryBlock* newBlock = new MemoryBlock(blockToUse->address, size);
        addToUsedList(newBlock);

        blockToUse->address += size;
        blockToUse->size -= size;

        return newBlock->address;
    }
}


void Memory::freeMemory(uint8_t* ptr)
{
    MemoryBlock* block = _usedList;
    MemoryBlock* found = nullptr;
    while (block)
    {
        if (block->address == ptr)
        {
            found = block;
            break;
        }
        block = block->next;
    }
    if(!found)
    {
        println("freeMemory for not used pointer called");
        _platform.fatalError();
    }
    removeFromUsedList(block);
    addToFreeList(block);
    scheduleSave();
}

void Memory::writeMemory(uint32_t relativeAddress, size_t size, uint8_t* data)
{
    // Wrap-safe bound: a management write, reachable over the tunnel, must never leave the NVM.
    const size_t nvmSize = _platform.getNonVolatileMemorySize();
    if (size > nvmSize || relativeAddress > nvmSize - size)
        return;
    if(_saveTimeout != 0)
    {
        _saveTimeout = millis();
        if (_saveTimeout == 0)
            _saveTimeout = 1; // prevent 0=disabled; no impact by minimal increased timeout
    }
    _platform.writeNonVolatileMemory(relativeAddress, data, size);
}

void Memory::readMemory(uint32_t relativeAddress, size_t size, uint8_t* data)
{
    // Same wrap-safe bound on the read side.
    const size_t nvmSize = _platform.getNonVolatileMemorySize();
    if (size > nvmSize || relativeAddress > nvmSize - size)
        return;
    _platform.readNonVolatileMemory(relativeAddress, data, size);
}


uint8_t* Memory::toAbsolute(uint32_t relativeAddress)
{
    return _platform.getNonVolatileMemoryStart() + (ptrdiff_t)relativeAddress;
}

uint8_t* Memory::toAbsoluteChecked(uint32_t relativeAddress, size_t size)
{
    // Wrap-safe NVM bound (same as read/writeMemory): reject an out-of-range range so a management
    // memory-read cannot memcpy past the NVM buffer (OOB read / info-leak). Returns nullptr on reject.
    const size_t nvmSize = _platform.getNonVolatileMemorySize();
    if (size > nvmSize || relativeAddress > nvmSize - size)
        return nullptr;
    return toAbsolute(relativeAddress);
}


uint32_t Memory::toRelative(uint8_t* absoluteAddress)
{
    return absoluteAddress - _platform.getNonVolatileMemoryStart();
}

MemoryBlock* Memory::removeFromList(MemoryBlock* head, MemoryBlock* item)
{
    // Null guard first: with head and item both null the equality below is true and head->next would
    // dereference null.
    if (!head || !item)
    {
        println("invalid parameters of Memory::removeFromList");
        _platform.fatalError();
    }

    if (head == item)
    {
        MemoryBlock* newHead = head->next;
        head->next = nullptr;
        return newHead;
    }

    bool found = false;
    MemoryBlock* block = head;
    while (block)
    {
        if (block->next == item)
        {
            found = true;
            block->next = item->next;
            break;
        }
        block = block->next;
    }

    if (!found)
    {
        println("tried to remove block from list not in it");
        _platform.fatalError();
    }
    item->next = nullptr;
    return head;
}

void Memory::removeFromFreeList(MemoryBlock* block)
{
    _freeList = removeFromList(_freeList, block);
}


void Memory::removeFromUsedList(MemoryBlock* block)
{
    _usedList = removeFromList(_usedList, block);
}


void Memory::addToUsedList(MemoryBlock* block)
{
    block->next = _usedList;
    _usedList = block;
}


void Memory::addToFreeList(MemoryBlock* block)
{
    if (_freeList == nullptr)
    {
        _freeList = block;
        return;
    }

    // first insert free block in list
    MemoryBlock* current = _freeList;
    while (current)
    {
        if (current->address <= block->address && (current->next == nullptr || block->address < current->next->address))
        {
            //add after current
            block->next = current->next;
            current->next = block;
            break;
        }
        else if (current->address > block->address)
        {
            //add before current
            block->next = current;

            if (current == _freeList)
                _freeList = block;

            // swap current and block for merge
            MemoryBlock* tmp = current;
            current = block;
            block = tmp;

            break;
        }

        current = current->next;
    }
    // now check if we can merge the blocks
    // first current an block
    if ((current->address + current->size) == block->address)
    {
        current->size += block->size;
        current->next = block->next;
        delete block;
        // check further if now current can be merged with current->next
        block = current;
    }

    // if block is the last one, we are done 
    if (block->next == nullptr)
        return;

    // now check block and block->next
    if ((block->address + block->size) == block->next->address)
    {
        // Take the node before advancing, otherwise the absorbed node leaks and the one behind it is freed
        // while the list still points at it.
        MemoryBlock* absorbed = block->next;
        block->size += absorbed->size;
        block->next = absorbed->next;
        delete absorbed;
    }
}

size_t Memory::alignToPageSize(size_t size)
{
    size_t pageSize = 4; //_platform.flashPageSize(); // align to 32bit for now, as aligning to flash-page-size causes side effects in programming
    // pagesize should be a multiply of two
    return (size + pageSize - 1) & (-1*pageSize);
}

MemoryBlock* Memory::findBlockInList(MemoryBlock* head, uint8_t* address)
{
    while (head != nullptr)
    {
        if (head->address == address)
            return head;

        head = head->next;
    }
    return nullptr;
}

void Memory::addNewUsedBlock(uint8_t* address, size_t size)
{
    MemoryBlock* smallerFreeBlock = _freeList;
    // find block in freeList where the new used block is contained in
    while (smallerFreeBlock)
    {
        if (smallerFreeBlock->next == nullptr ||
            (smallerFreeBlock->next != nullptr && smallerFreeBlock->next->address > address))
            break;
        
        smallerFreeBlock = smallerFreeBlock->next;
    }

    if (smallerFreeBlock == nullptr)
    {
        println("addNewUsedBlock: no smallerBlock found");
        _platform.fatalError();
    }

    if ((smallerFreeBlock->address + smallerFreeBlock->size) < (address + size))
    {
        println("addNewUsedBlock: found block can't contain new block");
        _platform.fatalError();
    }

    if (smallerFreeBlock->address == address && smallerFreeBlock->size == size)
    {
        // we take thow whole block
        removeFromFreeList(smallerFreeBlock);
        addToUsedList(smallerFreeBlock);
        return;
    }

    if (smallerFreeBlock->address == address)
    {
        // we take a front part of the block
        smallerFreeBlock->address += size;
        smallerFreeBlock->size -= size;
    }
    else if (address < smallerFreeBlock->address)
    {
        // A block below the free list would underflow (address - block->address); carve the overlap off the
        // front of the free block instead.
        uint8_t* oldEndAddr = smallerFreeBlock->address + smallerFreeBlock->size;
        uint8_t* newStartAddr = address + size;

        if (newStartAddr > smallerFreeBlock->address)
        {
            if (newStartAddr < oldEndAddr)
            {
                smallerFreeBlock->address = newStartAddr;
                smallerFreeBlock->size = oldEndAddr - newStartAddr;
            }
            else
            {
                removeFromFreeList(smallerFreeBlock);
                delete smallerFreeBlock;
            }
        }
    }
    else
    {
        // we take a middle or end part of the block
        uint8_t* oldEndAddr = smallerFreeBlock->address + smallerFreeBlock->size;
        smallerFreeBlock->size = (address - smallerFreeBlock->address);

        if (address + size < oldEndAddr)
        {
            // we take the middle part of the block, so we need a new free block for the end part
            MemoryBlock* newFreeBlock = new MemoryBlock();
            newFreeBlock->next = smallerFreeBlock->next;
            newFreeBlock->address = address + size;
            newFreeBlock->size = oldEndAddr - newFreeBlock->address;
            smallerFreeBlock->next = newFreeBlock;
        }
    }

    MemoryBlock* newUsedBlock = new MemoryBlock(address, size);
    addToUsedList(newUsedBlock);
}

void Memory::versionCheckCallback(VersionCheckCallback func)
{
    _versionCheckCallback = func;
}

VersionCheckCallback Memory::versionCheckCallback()
{
    return _versionCheckCallback;
}

void Memory::loop()
{
    if(_saveTimeout != 0 && millis() - _saveTimeout > 5000)
    {
        println("saveMemory timeout");
        _saveTimeout = 0;
        writeMemory();
    }
}
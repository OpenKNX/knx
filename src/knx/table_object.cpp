#include <string.h>

#include "table_object.h"
#include "bits.h"
#include "memory.h"
#include "callback_property.h"
#include "data_property.h"

BeforeTablesUnloadCallback TableObject::_beforeTablesUnload = 0;
uint8_t TableObject::_tableUnloadCount = 0;

void TableObject::beforeTablesUnloadCallback(BeforeTablesUnloadCallback func)
{
    _beforeTablesUnload = func;
}

BeforeTablesUnloadCallback TableObject::beforeTablesUnloadCallback()
{
    return _beforeTablesUnload;
}

TableObject::TableObject(Memory& memory, uint32_t staticTableAdr , uint32_t staticTableSize)
    : _memory(memory)
{
    _staticTableAdr = staticTableAdr;
    _staticTableSize = staticTableSize;
}

TableObject::~TableObject()
{}

void TableObject::beforeStateChange(LoadState& newState)
{
    if (newState == LS_LOADED && _tableUnloadCount > 0)
        _tableUnloadCount--;
    if (_tableUnloadCount > 0)
        return;
    if (newState == LS_UNLOADED) {
        // Latch only when there is a callback to suppress repeats for: readMemory() unloads the 091A tables
        // long before registerCallbacks() runs.
        if (_beforeTablesUnload == 0)
            return;

        _tableUnloadCount++;
        _beforeTablesUnload();
    }
}

LoadState TableObject::loadState()
{
    return _state;
}

void TableObject::loadState(LoadState newState)
{
    if (newState == _state)
        return;

    // 03_05_01 4.2.28 p.40: leaving Error clears the error code. Here rather than at one event, so the
    // master reset -- which unloads directly through resetTable() -- clears it too.
    if (_state == LS_ERROR)
        errorCode(E_NO_FAULT);

    beforeStateChange(newState);
    _state = newState;
    // 03_05_01 4.23.2.1 p.293 wants the load state in non-volatile memory; saveMemory() only flushes the
    // buffered sector.
    _memory.scheduleSave();
}

void TableObject::masterReset(EraseCode eraseCode, uint8_t channel)
{
    (void)channel;
    // ConfirmedRestart and ResetIA never touch a table.
    if (eraseCode == EraseCode::ConfirmedRestart || eraseCode == EraseCode::ResetIA)
        return;

    // A factory reset erases every table; the targeted codes erase only their matching object type.
    bool doReset = (eraseCode == EraseCode::FactoryReset || eraseCode == EraseCode::FactoryResetWithoutIA);
    if (!doReset)
    {
        uint16_t ot = 0;
        Property* p = property(PID_OBJECT_TYPE);
        if (p != nullptr)
            p->read(ot);
        switch (eraseCode)
        {
            case EraseCode::ResetLinks: doReset = (ot == OT_ADDR_TABLE || ot == OT_ASSOC_TABLE); break;
            case EraseCode::ResetAP:    doReset = (ot == OT_APPLICATION_PROG || ot == OT_GRP_OBJ_TABLE); break;
            case EraseCode::ResetParam: doReset = (ot == OT_APPLICATION_PROG); break;
            default: break;
        }
    }

    if (doReset)
        resetTable();
}

void TableObject::resetTable()
{
    // Byte-identical to the LE_UNLOAD handler in loadEventLoaded(): mark unloaded and free the NV memory.
    loadState(LS_UNLOADED);
    if (_data && !_staticTableAdr)
    {
        _memory.freeMemory(_data);
        _data = 0;
        _size = 0; // the extent described a block that no longer exists
    }
}


uint8_t* TableObject::save(uint8_t* buffer)
{
    //println("TableObject::save");
    allocTableStatic();

    buffer = pushByte(_state, buffer);

    buffer = pushInt(_size, buffer);

    if (_data)
        buffer = pushInt(_memory.toRelative(_data), buffer);
    else
        buffer = pushInt(0, buffer);

    return InterfaceObject::save(buffer);
}


const uint8_t* TableObject::restore(const uint8_t* buffer)
{
    //println("TableObject::restore");

    uint8_t state = 0;
    buffer = popByte(state, buffer);
    // Clamp to the states loadEvent() dispatches on: LS_UNLOADING and LS_LOADCOMPLETING fall into its
    // default arm and would freeze the object. 03_05_01 Table 94 p.296 maps them to Unloaded on restart.
    _state = (state == LS_UNLOADED || state == LS_LOADED || state == LS_LOADING || state == LS_ERROR)
                 ? (LoadState)state : LS_UNLOADED;

    buffer = popInt(_size, buffer);

    uint32_t relativeAddress = 0;
    buffer = popInt(relativeAddress, buffer);
    //println(relativeAddress);

    if (_staticTableAdr)
    {
        // A static table's address and size are build constants, not persisted state.
        if (staticTableFitsNvm())
        {
            _size = _staticTableSize;
            _data = _memory.toAbsolute(_staticTableAdr);
        }
        else
        {
            // Memory::readMemory() hands a restored pointer to addNewUsedBlock(), which calls
            // fatalError() when the block does not fit. Leave both at zero so it skips this table.
            println("static table does not fit the NVM -- not restored");
            _size = 0;
            _data = 0;
        }
    }
    else if (relativeAddress != 0)
        _data = _memory.toAbsolute(relativeAddress);
    else
        _data = 0;
    //println((uint32_t)_data);
    return InterfaceObject::restore(buffer);
}

uint32_t TableObject::tableReference()
{
    // 03_05_01 4.2.7 p.31: zero when the allocation was not successful, and inside the 20-bit range.
    if (_data == nullptr)
        return 0;

    return (uint32_t)_memory.toRelative(_data);
}

bool TableObject::allocTable(uint32_t size, bool doFill, uint8_t fillByte)
{
    if(_staticTableAdr)
        return false;

    if (_data)
    {
        _memory.freeMemory(_data);
        _data = 0;
    }

    // A zero size would reach LOADED with a null data pointer; a size beyond the NVM would wrap in the
    // allocator.
    if (size == 0 || size > _memory.memorySize())
    {
        _size = 0;
        return false;
    }

    _data = _memory.allocMemory(size);
    if (!_data)
    {
        _size = 0;
        return false;
    }

    if (doFill)
    {
        uint32_t addr = _memory.toRelative(_data);
        for(uint32_t i = 0; i < size;i++)
            _memory.writeMemory(addr+i, 1, &fillByte);
    }

    _size = size;

    return true;
}


/**
 * @brief Whether a static table's build constants fit the NVM window.
 *
 * The address and the extent come from the product, not from flash, and were never compared against the
 * window. allocTable() refuses a static table in any case, so the load state machine ends in LS_ERROR;
 * what this prevents is the pointer past the end that addNewUsedBlock() turns into a fatalError().
 */
bool TableObject::staticTableFitsNvm()
{
    return (uint32_t)_staticTableAdr + _staticTableSize <= _memory.memorySize();
}

void TableObject::allocTableStatic()
{
    if(_staticTableAdr && !_data)
    {
        if (!staticTableFitsNvm())
            return;

        _data = _memory.toAbsolute(_staticTableAdr);
        _size = _staticTableSize;
        _memory.addNewUsedBlock(_data, _size);
    }
}

void TableObject::loadEvent(const uint8_t* data)
{
    //printHex("TableObject::loadEvent 0x", data, 10);
    switch (_state)
    {
        case LS_UNLOADED:
            loadEventUnloaded(data);
            break;
        case LS_LOADING:
            loadEventLoading(data);
            break;
        case LS_LOADED:
            loadEventLoaded(data);
            break;
        case LS_ERROR:
            loadEventError(data);
            break;
        default:
            /* do nothing */
            break;
    }
}

void TableObject::loadEventUnloaded(const uint8_t* data)
{
    uint8_t event = data[0];
    switch (event)
    {
        case LE_NOOP:
        case LE_LOAD_COMPLETED:
        case LE_ADDITIONAL_LOAD_CONTROLS:
        case LE_UNLOAD:
            break;
        case LE_START_LOADING:
            loadState(LS_LOADING);
            break;
        default:
            break; // 03_05_01 4.23.2.3.2 p.294: unknown events shall be ignored, without a change of state
    }
}

void TableObject::loadEventLoading(const uint8_t* data)
{
    uint8_t event = data[0];
    switch (event)
    {
        case LE_NOOP:
        case LE_START_LOADING:
            break;
        case LE_LOAD_COMPLETED:
            // No allocation was received: LOADED would hand consumers a null data(). A zero-size allocation
            // already ends in Error; completing without any must too. A static table gets its block here
            // when no save has placed it yet (fresh flash).
            allocTableStatic();
            if (_data == nullptr)
            {
                loadState(LS_ERROR);
                errorCode(E_GOT_MEM_ALLOC_ZERO);
                break;
            }
            _memory.saveMemory();
            loadState(LS_LOADED);
            break;
        case LE_UNLOAD:
            loadState(LS_UNLOADED);
            break;
        case LE_ADDITIONAL_LOAD_CONTROLS:
            additionalLoadControls(data);
            break;
        default:
            break; // 03_05_01 4.23.2.3.2 p.294: unknown events shall be ignored, without a change of state
    }
}

void TableObject::loadEventLoaded(const uint8_t* data)
{
    uint8_t event = data[0];
    switch (event)
    {
        case LE_NOOP:
        case LE_LOAD_COMPLETED:
            break;
        case LE_START_LOADING:
            loadState(LS_LOADING);
            break;
        case LE_UNLOAD:
            loadState(LS_UNLOADED);
            //free nv memory
            if (_data)
            {
                if(!_staticTableAdr)
                {
                    _memory.freeMemory(_data);
                    _data = 0;
                    _size = 0; // the extent described a block that no longer exists
                }
            }
            break;
        case LE_ADDITIONAL_LOAD_CONTROLS:
            loadState(LS_ERROR);
            errorCode(E_INVALID_OPCODE);
            break;
        default:
            break; // 03_05_01 4.23.2.3.2 p.294: unknown events shall be ignored, without a change of state
    }
}

void TableObject::loadEventError(const uint8_t* data)
{
    uint8_t event = data[0];
    switch (event)
    {
        case LE_NOOP:
        case LE_LOAD_COMPLETED:
        case LE_ADDITIONAL_LOAD_CONTROLS:
        case LE_START_LOADING:
            break;
        case LE_UNLOAD:
            loadState(LS_UNLOADED); // loadState() clears the error code on the way out of Error
            break;
        default:
            break; // 03_05_01 4.23.2.3.2 p.294: unknown events shall be ignored, without a change of state
    }
}

void TableObject::additionalLoadControls(const uint8_t* data)
{
    if (data[1] != 0x0B) // Data Relative Allocation
    {
        loadState(LS_ERROR);
        errorCode(E_INVALID_OPCODE);
        return;
    }

    size_t size = ((data[2] << 24) | (data[3] << 16) | (data[4] << 8) | data[5]);
    bool doFill = data[6] == 0x1;
    uint8_t fillByte = data[7];
    if (!allocTable(size, doFill, fillByte))
    {
        loadState(LS_ERROR);
        errorCode(E_MAX_TABLE_LENGTH_EXEEDED);
    }
}

uint8_t* TableObject::data()
{
    return _data;
}

void TableObject::errorCode(ErrorCode errorCode)
{
    uint8_t data = errorCode;
    Property* prop = property(PID_ERROR_CODE);
    // A static table gets its properties from InterfaceObject::initializeProperties, which creates no
    // PID_ERROR_CODE.
    if (prop == nullptr)
        return;
    prop->write(data);
}

uint16_t TableObject::saveSize()
{
    return 5 + InterfaceObject::saveSize() + sizeof(_size);
}

void TableObject::initializeProperties(size_t propertiesSize, Property** properties)
{
    Property* ownProperties[] =
    {
        new CallbackProperty<TableObject>(this, PID_LOAD_STATE_CONTROL, true, PDT_CONTROL, 1, ReadLv3 | WriteLv3,
            [](TableObject* obj, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t {
                if(start == 0)
                {
                    uint16_t currentNoOfElements = 1;
                    pushWord(currentNoOfElements, data);
                    return 1;
                }

                data[0] = obj->_state;
                return 1;
            },
            [](TableObject* obj, uint16_t start, uint8_t count, const uint8_t* data) -> uint8_t {
                obj->loadEvent(data);
                return 1;
            })
     };

    uint8_t ownPropertiesCount = sizeof(ownProperties) / sizeof(Property*);

    uint8_t propertyCount = propertiesSize / sizeof(Property*);
    uint8_t allPropertiesCount = propertyCount + ownPropertiesCount;

    Property* allProperties[allPropertiesCount];
    memcpy(allProperties, properties, propertiesSize);
    memcpy(allProperties + propertyCount, ownProperties, sizeof(ownProperties));

    if(_staticTableAdr)
        InterfaceObject::initializeProperties(sizeof(allProperties), allProperties);
    else
        initializeDynTableProperties(sizeof(allProperties), allProperties);
}

void TableObject::initializeDynTableProperties(size_t propertiesSize, Property** properties)
{
    Property* ownProperties[] =
    {
        new CallbackProperty<TableObject>(this, PID_TABLE_REFERENCE, false, PDT_UNSIGNED_LONG, 1, ReadLv3 | WriteLv0,
            [](TableObject* obj, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t {
                if(start == 0)
                {
                    uint16_t currentNoOfElements = 1;
                    pushWord(currentNoOfElements, data);
                    return 1;
                }

                if (obj->_state == LS_UNLOADED)
                    pushInt(0, data);
                else
                    pushInt(obj->tableReference(), data);
                return 1;
            }),
        new CallbackProperty<TableObject>(this, PID_MCB_TABLE, false, PDT_GENERIC_08, 1, ReadLv3 | WriteLv0,
            [](TableObject* obj, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t {
                // The element-count read, as both sibling callbacks do it: without it the lambda writes its 8 octets
                // into the 2-octet buffer the caller allocates for start index 0.
                if(start == 0)
                {
                    uint16_t currentNoOfElements = 1;
                    pushWord(currentNoOfElements, data);
                    return 1;
                }

                if (obj->_state != LS_LOADED)
                    return 0; // need to check return code for invalid
                
                uint32_t segmentSize = obj->_size;
                // crc16Ccitt() takes a uint16_t length: a segment above 65535 octets would be checksummed
                // over size & 0xFFFF. Unreachable while KNX_FLASH_SIZE stays below that -- widen the helper
                // before allocating more.
                uint16_t crc16 = crc16Ccitt(obj->data(), (uint16_t)segmentSize);

                pushInt(segmentSize, data);     // Segment size
                pushByte(0x00, data + 4);       // CRC control byte -> 0: always valid
                pushByte(0xFF, data + 5);       // Read access 4 bits + Write access 4 bits
                pushWord(crc16, data + 6);      // CRC-16 CCITT of data
    
                return 1;
            }),
        new DataProperty(PID_ERROR_CODE, false, PDT_ENUM8, 1, ReadLv3 | WriteLv0, (uint8_t)E_NO_FAULT)
     };

    uint8_t ownPropertiesCount = sizeof(ownProperties) / sizeof(Property*);

    uint8_t propertyCount = propertiesSize / sizeof(Property*);
    uint8_t allPropertiesCount = propertyCount + ownPropertiesCount;

    Property* allProperties[allPropertiesCount];
    memcpy(allProperties, properties, propertiesSize);
    memcpy(allProperties + propertyCount, ownProperties, sizeof(ownProperties));

    InterfaceObject::initializeProperties(sizeof(allProperties), allProperties);
}
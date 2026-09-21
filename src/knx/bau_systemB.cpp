#include "bau_systemB.h"
#include "bits.h"
#include <string.h>
#include <stdio.h>

enum NmReadSerialNumberType
{
    NM_Read_SerialNumber_By_ProgrammingMode = 0x01,
    NM_Read_SerialNumber_By_ExFactoryState = 0x02,
    NM_Read_SerialNumber_By_PowerReset = 0x03,
    NM_Read_SerialNumber_By_ManufacturerSpecific = 0xFE,
};

static constexpr auto kFunctionPropertyResultBufferMaxSize = 0xFF;
// What the response frames can actually carry. CemiFrame::buffer holds indices 0..263 and apdu.data()
// is buffer+10, so the plain response writes from buffer+13 (<= 251 octets) and the extended one from
// buffer+16 (<= 248). Telling a callee it may write 0xFF let it overrun the frame it is built into.
static constexpr uint8_t kFunctionPropertyResultMax = 251;

// Index 0 addresses the element count, not an array element: DataProperty::write() tests data[0] and
// data[1] there and clears the property on two zero octets. 03_03_07 3.4.4.1 p.64 defines index 0 for the
// READ only; the write is the de-facto counterpart this stack implements. Two octets are needed whatever
// ElementSize reports, and on TP the frame buffer ends right behind the declared payload.
// `length` is uint32_t: the cEMI feeder passes a uint16_t-derived size, which a uint8_t would truncate.
static inline bool propertyPayloadFits(const Property* prop, uint16_t startIndex, uint8_t count, uint32_t length)
{
    if (startIndex == 0)
        return length >= 2;

    return (uint32_t)count * prop->ElementSize() <= length;
}

static constexpr uint8_t kFunctionPropertyResultMaxExt = 248;
// 03_05_02 3.7.3 p.89: the client takes this as the TIME-OUT after which contact counts as failed, so
// too small declares a booting device dead. Measured after a master reset: TP 3.5 s, IP 8.0 to 9.8 s.
#ifdef USE_IP
static constexpr auto kRestartProcessTime = 20;
#else
static constexpr auto kRestartProcessTime = 10;
#endif
// Time the queued A_Restart_Response gets before the device resets: one TP1 frame with three repetitions and
// the partner's T_ACK take well under this, and it stays inside the announced process time.
static constexpr uint32_t kSelfResetDelayMs = 1000;

BauSystemB::BauSystemB(Platform& platform): _memory(platform, _deviceObj),
     _appProgram(_memory),
    _platform(platform)
{
    _memory.addSaveRestore(&_appProgram);
}

void BauSystemB::readMemory()
{
    _memory.readMemory();
}

void BauSystemB::writeMemory()
{
    _memory.writeMemory();
}


/**
 * @brief Write back a pending NVM sector before a management read.
 *
 * On flash-backed platforms a read goes through the flash mapping while a write sits in the driver's sector
 * buffer until commit, so a read right after a write returned the old content. An unchanged sector is not
 * rewritten; Eeprom-backed platforms read their RAM copy and need nothing.
 */
void BauSystemB::flushBeforeRead()
{
    if (_platform.NonVolatileMemoryType() != Eeprom)
        _platform.commitNonVolatileMemory();
}

Platform& BauSystemB::platform()
{
    return _platform;
}

ApplicationProgramObject& BauSystemB::parameters()
{
    return _appProgram;
}

DeviceObject& BauSystemB::deviceObject()
{
    return _deviceObj;
}

uint8_t BauSystemB::checkmasterResetValidity(EraseCode eraseCode, uint8_t channel)
{
    static constexpr uint8_t successCode = 0x00;
    static constexpr uint8_t invalidEraseCode = 0x02;      // unsupported erase code
    static constexpr uint8_t invalidChannelNumber = 0x03;  // 03_05_02 p.84

    // The erase code is validated FIRST: 03_05_02 Table 4 p.83 gives a reserved erase code the note
    // "Channel Number: not defined" and answers 02h, so the channel (03h) is only meaningful once the
    // code is known. The other order would answer 03h for a reserved code carrying a non-zero channel.
    switch (eraseCode)
    {
        // All standard erase codes are supported; the reset itself runs per object in doMasterReset().
        case EraseCode::ConfirmedRestart:
        case EraseCode::ResetAP:
        case EraseCode::ResetIA:
        case EraseCode::ResetLinks:
        case EraseCode::ResetParam:
        case EraseCode::FactoryReset:
        case EraseCode::FactoryResetWithoutIA:
            break;
        default:
        {
            print("Unhandled erase code: ");
            println(eraseCode, HEX);
            return invalidEraseCode;
        }
    }

    // 03_05_02 p.84: 03h when a channel other than 00h is requested but the server has none. Every
    // masterReset() implementation here discards the channel, so this stack has no channels.
    if (channel != 0)
        return invalidChannelNumber;

    return successCode;
}

void BauSystemB::deviceDescriptorReadIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t descriptorType)
{
    if (descriptorType != 0)
        descriptorType = 0x3f;
    
    uint8_t data[2];
    pushWord(_deviceObj.maskVersion(), data);
    applicationLayer().deviceDescriptorReadResponse(AckRequested, priority, hopType, asap, secCtrl, descriptorType, data);
}
void BauSystemB::memoryRouterWriteIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t number,
                                             uint16_t memoryAddress, uint8_t *data)
{
    print("Writing memory at: ");
    print(memoryAddress, HEX);
    print(" length: ");
    print(number);
    print(" data: ");
    printHex("=>", data, number);
    const bool stored = _memory.toAbsoluteChecked(memoryAddress, number) != nullptr;
    _memory.writeMemory(memoryAddress, number, data);
    if (_deviceObj.verifyMode())
    {
        print("Sending Read indication");
        memoryRouterReadIndication(priority, hopType, asap, secCtrl, stored ? number : 0, memoryAddress, stored ? data : nullptr);
    }
}

void BauSystemB::memoryRouterReadIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t number,
                                            uint16_t memoryAddress, uint8_t *data)
{
    applicationLayer().memoryRouterReadResponse(AckRequested, priority, hopType, asap, secCtrl, number, memoryAddress, data);
}

void BauSystemB::memoryRoutingTableReadIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t number, uint16_t memoryAddress, uint8_t *data)
{
    applicationLayer().memoryRoutingTableReadResponse(AckRequested, priority, hopType, asap, secCtrl, number, memoryAddress, data);
}
void BauSystemB::memoryRoutingTableReadIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t number, uint16_t memoryAddress)
{
    uint8_t* p = _memory.toAbsoluteChecked(memoryAddress, number);
    if (p == nullptr) number = 0; // OOB read guard: keep the response within NVM
    memoryRoutingTableReadIndication(priority, hopType, asap, secCtrl, number, memoryAddress, p);
}

void BauSystemB::memoryRoutingTableWriteIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t number, uint16_t memoryAddress, uint8_t *data)
{
    print("Writing memory at: ");
    print(memoryAddress, HEX);
    print(" length: ");
    print(number);
    print(" data: ");
    printHex("=>", data, number);
    const bool stored = _memory.toAbsoluteChecked(memoryAddress, number) != nullptr;
    _memory.writeMemory(memoryAddress, number, data);
    // Verify mode answers with the stored data, as memoryWriteIndication() does.
    if (_deviceObj.verifyMode())
        memoryRoutingTableReadIndication(priority, hopType, asap, secCtrl, stored ? number : 0, memoryAddress, stored ? data : nullptr);
}

void BauSystemB::memoryWriteIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t number,
    uint16_t memoryAddress, uint8_t * data)
{
    // Verify mode answers with the accepted data, not a read-back: on flash the block is still in the sector
    // buffer and the flash mapping holds the old content. writeMemory() drops a write outside the NVM: no data.
    const bool stored = _memory.toAbsoluteChecked(memoryAddress, number) != nullptr;
    _memory.writeMemory(memoryAddress, number, data);
    if (_deviceObj.verifyMode())
        memoryReadIndication(priority, hopType, asap, secCtrl, stored ? number : 0, memoryAddress, stored ? data : nullptr);
}

void BauSystemB::memoryReadIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t number,
    uint16_t memoryAddress, uint8_t * data)
{
    applicationLayer().memoryReadResponse(AckRequested, priority, hopType, asap, secCtrl, number, memoryAddress, data);
}

void BauSystemB::memoryReadIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t number,
    uint16_t memoryAddress)
{
    flushBeforeRead();
    uint8_t* p = _memory.toAbsoluteChecked(memoryAddress, number);
    if (p == nullptr) number = 0; // OOB read guard: keep the response within NVM
    applicationLayer().memoryReadResponse(AckRequested, priority, hopType, asap, secCtrl, number, memoryAddress, p);
}

void BauSystemB::memoryExtWriteIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t number, uint32_t memoryAddress, uint8_t * data)
{
    // Bound the range first; a rejected write is answered AddressVoid with zero elements.
    uint8_t* p = _memory.toAbsoluteChecked(memoryAddress, number);
    if (p == nullptr)
    {
        applicationLayer().memoryExtWriteResponse(AckRequested, priority, hopType, asap, secCtrl, ReturnCodes::AddressVoid, 0, memoryAddress, nullptr);
        return;
    }

    _memory.writeMemory(memoryAddress, number, data);
    applicationLayer().memoryExtWriteResponse(AckRequested, priority, hopType, asap, secCtrl, ReturnCodes::Success, number, memoryAddress, p);
}

void BauSystemB::memoryExtReadIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t number, uint32_t memoryAddress)
{
    // The response carries the data at APDU offset 5, so 5 + number has to fit MAX_APDU_OCTET_COUNT. The
    // builder clamps for memory safety, which answered a short read as Success; F4h says it does not fit.
    if ((uint16_t)number + 5 > MAX_APDU_OCTET_COUNT)
    {
        applicationLayer().memoryExtReadResponse(AckRequested, priority, hopType, asap, secCtrl,
                                                 ReturnCodes::ExceedsMaxApduLength, 0, memoryAddress, nullptr);
        return;
    }

    flushBeforeRead();
    uint8_t* p = _memory.toAbsoluteChecked(memoryAddress, number);
    ReturnCodes code = (p != nullptr) ? ReturnCodes::Success : ReturnCodes::AddressVoid; // OOB read -> AddressVoid, no data
    if (p == nullptr) number = 0;
    applicationLayer().memoryExtReadResponse(AckRequested, priority, hopType, asap, secCtrl, code, number, memoryAddress, p);
}

void BauSystemB::doMasterReset(EraseCode eraseCode, uint8_t channel)
{
    _deviceObj.masterReset(eraseCode, channel);
    _appProgram.masterReset(eraseCode, channel);
}

void BauSystemB::restartRequestIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, RestartType restartType, EraseCode eraseCode, uint8_t channel)
{
    if (restartType == RestartType::BasicRestart)
    {
        println("Basic restart requested");
        if (_beforeRestart != 0)
            _beforeRestart();
        // A master reset still waiting for its response to leave was already confirmed with 00h: carry it out.
        if (_selfResetPending)
        {
            _selfResetPending = false;
            doMasterReset(_selfResetErase, _selfResetChannel);
        }
    }
    else if (restartType == RestartType::MasterReset)
    {
        uint8_t errorCode = checkmasterResetValidity(eraseCode, channel);
        // We send the restart response now before actually applying the reset values
        applicationLayer().restartResponse(AckRequested, priority, hopType, secCtrl, errorCode, (errorCode == 0) ? kRestartProcessTime : 0, asap);

        // 03_05_02 Table 4 p.83: for an unsupported erase code the server shall neither execute a Basic
        // Restart nor any Master Reset.
        if (errorCode != 0)
            return;

        // Resetting and restarting here dropped the response still queued for the medium. The reset runs from
        // nextRestartState() once the response had time to go out; until then the device state is unchanged,
        // so the response also leaves with the address it was requested on.
        _selfResetErase = eraseCode;
        _selfResetChannel = channel;
        _selfResetAt = millis();
        _selfResetPending = true;
        return;
    }
    else
    {
        // Cannot happen as restartType is just one bit
        println("Unhandled restart type.");
        _platform.fatalError();
    }

    // Flush the EEPROM before resetting
    _memory.writeMemory();
    _platform.restart();
}

void BauSystemB::authorizeIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint32_t key)
{
    applicationLayer().authorizeResponse(AckRequested, priority, hopType, asap, secCtrl, 0);
}

void BauSystemB::userMemoryReadIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t number, uint32_t memoryAddress)
{
    flushBeforeRead();
    uint8_t* p = _memory.toAbsoluteChecked(memoryAddress, number);
    if (p == nullptr) number = 0; // OOB read guard: keep the response within NVM
    applicationLayer().userMemoryReadResponse(AckRequested, priority, hopType, asap, secCtrl, number, memoryAddress, p);
}

void BauSystemB::userMemoryWriteIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t number, uint32_t memoryAddress, uint8_t* data)
{
    // Verify mode answers with the stored data, as memoryWriteIndication() does.
    const bool stored = _memory.toAbsoluteChecked(memoryAddress, number) != nullptr;
    _memory.writeMemory(memoryAddress, number, data);

    if (_deviceObj.verifyMode())
        applicationLayer().userMemoryReadResponse(AckRequested, priority, hopType, asap, secCtrl, stored ? number : 0, memoryAddress, stored ? data : nullptr);
}

void BauSystemB::propertyDescriptionReadIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t objectIndex,
    uint8_t propertyId, uint8_t propertyIndex)
{
    uint8_t pid = propertyId;
    bool writeEnable = false;
    uint8_t type = 0;
    uint16_t numberOfElements = 0;
    uint8_t access = 0;
    InterfaceObject* obj = getInterfaceObject(objectIndex);
    if (obj)
        obj->readPropertyDescription(pid, propertyIndex, writeEnable, type, numberOfElements, access);

    applicationLayer().propertyDescriptionReadResponse(AckRequested, priority, hopType, asap, secCtrl, objectIndex, pid, propertyIndex,
        writeEnable, type, numberOfElements, access);
}

void BauSystemB::propertyExtDescriptionReadIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl,
    uint16_t objectType, uint16_t objectInstance, uint16_t propertyId, uint8_t descriptionType, uint16_t propertyIndex)
{
    uint8_t pid = propertyId;
    uint8_t pidx = propertyIndex;
    if(propertyId > 0xFF || propertyIndex > 0xFF)
    {
        println("BauSystemB::propertyExtDescriptionReadIndication: propertyId or Idx > 256 are not supported");
        return;
    }
    if(descriptionType != 0)
    {
        println("BauSystemB::propertyExtDescriptionReadIndication: only descriptionType 0 supported");
        return;
    }
    bool writeEnable = false;
    uint8_t type = 0;
    uint16_t numberOfElements = 0;
    uint8_t access = 0;
    InterfaceObject* obj = getInterfaceObject((ObjectType)objectType, objectInstance);
    if (obj)
        obj->readPropertyDescription(pid, pidx, writeEnable, type, numberOfElements, access);

    // Return the RESOLVED pid/index (readPropertyDescription resolves them in place, like the non-ext handler):
    // echoing the client's propertyId/propertyIndex made by-index reads report pid=0 -> ETS could not enumerate PIDs.
    applicationLayer().propertyExtDescriptionReadResponse(AckRequested, priority, hopType, asap, secCtrl, objectType, objectInstance, pid, pidx,
        descriptionType, writeEnable, type, numberOfElements, access);
}

void BauSystemB::propertyValueWriteIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t objectIndex,
    uint8_t propertyId, uint8_t numberOfElements, uint16_t startIndex, uint8_t* data, uint8_t length)
{
    bool written = false;
    InterfaceObject* obj = getInterfaceObject(objectIndex);
    if(obj)
    {
        // Memory-safety guard (see propertyValueExtWriteIndication): numberOfElements is attacker-controlled and
        // DataProperty::write() memcpy()s numberOfElements*ElementSize() from `data`; never read past the payload.
        Property* prop = obj->property((PropertyID)propertyId);
        // PID_LOAD_STATE_CONTROL (PDT_CONTROL, ElementSize reports 1) reaches additionalLoadControls, which reads
        // 8 octets for LE_ADDITIONAL_LOAD_CONTROLS; ElementSize does not bound that -> drop a short/corrupt one.
        bool loadCtrlShort = (propertyId == PID_LOAD_STATE_CONTROL && length >= 1
                              && data[0] == LE_ADDITIONAL_LOAD_CONTROLS && length < 8);
        // Enforce the write-enable flag reported in A_PropertyDescription_Response. Not inside
        // InterfaceObject::writeProperty: two BAUs initialise read-only PID_COMM_MODES_SUPPORTED through it.
        if (!loadCtrlShort && (prop == nullptr || (prop->WriteEnable() && propertyPayloadFits(prop, startIndex, numberOfElements, length))))
        {
            // `numberOfElements` is an in/out parameter: writeProperty() sets it to what the property
            // actually accepted (0 on refusal), so this reports a refusal the property itself made -- not
            // merely that the call was attempted.
            obj->writeProperty((PropertyID)propertyId, startIndex, data, numberOfElements);
            written = (numberOfElements != 0);
        }
    }
    // 03_03_07 3.4.4.2 p.66: on a problem, missing access rights included, nr_of_elem shall be zero with
    // no data. Count 0 makes the read below emit that.
    propertyValueReadIndication(priority, hopType, asap, secCtrl, objectIndex, propertyId, written ? numberOfElements : 0, startIndex);
}

void BauSystemB::propertyValueExtWriteIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, ObjectType objectType, uint8_t objectInstance,
    uint8_t propertyId, uint8_t numberOfElements, uint16_t startIndex, uint8_t* data, uint8_t length, bool confirmed)
{
    uint8_t returnCode = ReturnCodes::Success;

    InterfaceObject* obj = getInterfaceObject(objectType, objectInstance);
    if(obj)
    {
        // Memory-safety: numberOfElements is attacker-controlled; DataProperty::write() clamps it only against
        // _maxElements and then memcpy()s numberOfElements*ElementSize() from `data`, over-reading the
        // `length`-octet payload (and persisting the stolen bytes into the property). Reject a write that
        // claims more element data than the payload carries.
        Property* prop = obj->property((PropertyID)propertyId);
        // see propertyValueWriteIndication: the LE_ADDITIONAL_LOAD_CONTROLS callback reads 8 octets (PDT_CONTROL
        // ElementSize reports 1 and does not bound it) -> reject a short/corrupt load-control write.
        bool loadCtrlShort = (propertyId == PID_LOAD_STATE_CONTROL && length >= 1
                              && data[0] == LE_ADDITIONAL_LOAD_CONTROLS && length < 8);
        if (loadCtrlShort || (prop != nullptr && !propertyPayloadFits(prop, startIndex, numberOfElements, length)))
            returnCode = ReturnCodes::DataOverflow;
        else if (prop != nullptr && !prop->WriteEnable())  // see propertyValueWriteIndication
            returnCode = ReturnCodes::AccessReadOnly;
        else
            obj->writeProperty((PropertyID)propertyId, startIndex, data, numberOfElements);
    }
    else
        returnCode = ReturnCodes::AddressVoid;

    if (confirmed)
    {
        applicationLayer().propertyValueExtWriteConResponse(AckRequested, priority, hopType, asap, secCtrl, objectType, objectInstance, propertyId, numberOfElements, startIndex, returnCode);
    }
}

void BauSystemB::propertyValueReadIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t objectIndex,
    uint8_t propertyId, uint8_t numberOfElements, uint16_t startIndex)
{
    uint8_t size = 0;
    uint8_t elementCount = numberOfElements;
#ifdef LOG_KNX_PROP
    print("propertyValueReadIndication: ObjIdx ");
    print(objectIndex);
    print(" propId ");
    print(propertyId);
    print(" num ");
    print(numberOfElements);
    print(" start ");
    print(startIndex);
#endif

    InterfaceObject* obj = getInterfaceObject(objectIndex);
    if (obj)
    {
        uint8_t elementSize = obj->propertySize((PropertyID)propertyId);
        if (startIndex > 0)
        {
            // Clamp so elementSize*count fits the uint8 buffer: no truncation mismatch, no oversized stack VLA.
            uint16_t total = (uint16_t)elementSize * numberOfElements;
            if (total > 249)
            {
                // 03_03_07 3.4.4.1 p.63: if the data does not fit in a PDU, nr_of_elem shall be zero and the response
                // shall carry no data. The clamp still bounds the stack array below.
                elementCount = 0;
                total = 0;
            }
            size = (uint8_t)total;
        }
        else
            size = sizeof(uint16_t); // size of property array entry 0 which contains the current number of elements
    }
    else
        elementCount = 0;

    uint8_t data[size];
    if(obj)
        obj->readProperty((PropertyID)propertyId, startIndex, elementCount, data);

    if (elementCount == 0)
        size = 0;
    
    applicationLayer().propertyValueReadResponse(AckRequested, priority, hopType, asap, secCtrl, objectIndex, propertyId, elementCount,
                                        startIndex, data, size);
}

void BauSystemB::propertyValueExtReadIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, ObjectType objectType, uint8_t objectInstance,
    uint8_t propertyId, uint8_t numberOfElements, uint16_t startIndex)
{
    uint8_t size = 0;
    uint8_t elementCount = numberOfElements;
    InterfaceObject* obj = getInterfaceObject(objectType, objectInstance);
    if (obj)
    {
        uint8_t elementSize = obj->propertySize((PropertyID)propertyId);
        if (startIndex > 0)
        {
            // Clamp so elementSize*count fits the uint8 buffer: no truncation mismatch, no oversized stack VLA.
            uint16_t total = (uint16_t)elementSize * numberOfElements;
            if (total > 245)
            {
                elementCount = elementSize ? (uint8_t)(245 / elementSize) : 0;
                total = (uint16_t)elementSize * elementCount;
            }
            size = (uint8_t)total;
        }
        else
            size = sizeof(uint16_t); // size of propert array entry 0 which is the size
    }
    else
        elementCount = 0;

    uint8_t data[size];
    if(obj)
        obj->readProperty((PropertyID)propertyId, startIndex, elementCount, data);

    if (elementCount == 0)
        size = 0;

    applicationLayer().propertyValueExtReadResponse(AckRequested, priority, hopType, asap, secCtrl, objectType, objectInstance, propertyId, elementCount,
                                           startIndex, data, size);
}

void BauSystemB::functionPropertyCommandIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t objectIndex,
                                                   uint8_t propertyId, uint8_t* data, uint8_t length)
{
    uint8_t resultData[kFunctionPropertyResultBufferMaxSize];
    uint8_t resultLength = kFunctionPropertyResultMax; // tell the callee what the response can carry

    bool handled = false;

    InterfaceObject* obj = getInterfaceObject(objectIndex);
    if(obj)
    {
        Property* prop = obj->property((PropertyID)propertyId);
        if (prop != nullptr && prop->Type() == PDT_FUNCTION) // property() returns nullptr for an unknown PID -> null-deref
        {
            obj->command((PropertyID)propertyId, data, length, resultData, resultLength);
            handled = true;
        }
        else
        {
            if(_functionProperty != 0)
                if(_functionProperty(objectIndex, propertyId, length, data, resultData, resultLength))
                    handled = true;

            // 03_03_07 3.4.7.3 p.88: a property that exists but is not PDT_FUNCTION shall be answered with
            // a response carrying neither return code nor data. An unknown PID is not covered -> stay silent.
            if (!handled && prop != nullptr)
            {
                resultLength = 0;
                handled = true;
            }
        }
    } else {
        if(_functionProperty != 0)
            if(_functionProperty(objectIndex, propertyId, length, data, resultData, resultLength))
                handled = true;
    }

    //only return a value it was handled by a property or function
    if(handled)
        applicationLayer().functionPropertyStateResponse(AckRequested, priority, hopType, asap, secCtrl, objectIndex, propertyId, resultData, resultLength);
}

void BauSystemB::functionPropertyStateIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t objectIndex,
                                                 uint8_t propertyId, uint8_t* data, uint8_t length)
{
    uint8_t resultData[kFunctionPropertyResultBufferMaxSize];
    uint8_t resultLength = kFunctionPropertyResultMax; // tell the callee what the response can carry

    // Was initialised to true, so the "only answer if handled" check below never suppressed anything and
    // a response was built with the untouched resultLength. The command twin above initialises it false.
    bool handled = false;

    InterfaceObject* obj = getInterfaceObject(objectIndex);
    if(obj)
    {
        Property* prop = obj->property((PropertyID)propertyId);
        if (prop != nullptr && prop->Type() == PDT_FUNCTION) // property() returns nullptr for an unknown PID -> null-deref
        {
            obj->state((PropertyID)propertyId, data, length, resultData, resultLength);
            handled = true;
        }
        else
        {
            if(_functionPropertyState != 0)
                if(_functionPropertyState(objectIndex, propertyId, length, data, resultData, resultLength))
                    handled = true;

            // 03_03_07 3.4.7.3 p.88: a property that exists but is not PDT_FUNCTION shall be answered with
            // a response carrying neither return code nor data. An unknown PID is not covered -> stay silent.
            if (!handled && prop != nullptr)
            {
                resultLength = 0;
                handled = true;
            }
        }
    } else {
        if(_functionPropertyState != 0)
            if(_functionPropertyState(objectIndex, propertyId, length, data, resultData, resultLength))
                handled = true;
    }

    //only return a value it was handled by a property or function
    if(handled)
        applicationLayer().functionPropertyStateResponse(AckRequested, priority, hopType, asap, secCtrl, objectIndex, propertyId, resultData, resultLength);
}

void BauSystemB::functionPropertyExtCommandIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, ObjectType objectType, uint8_t objectInstance,
                                                      uint8_t propertyId, uint8_t* data, uint8_t length)
{
    if (length == 0) return; // the reserved input octet data[0] must be present; drop a truncated ext function-property command
    uint8_t resultData[kFunctionPropertyResultBufferMaxSize];
    uint8_t resultLength = 1; // we always have to include the return code at least

    InterfaceObject* obj = getInterfaceObject(objectType, objectInstance);
    if(obj)
    {
        Property* prop = obj->property((PropertyID)propertyId);
        PropertyDataType propType = prop != nullptr ? prop->Type() : (PropertyDataType)0; // null (unknown PID) -> non-FUNCTION sentinel, never deref null

        if (propType == PDT_FUNCTION)
        {
            // The first byte is reserved and 0 for PDT_FUNCTION
            uint8_t reservedByte = data[0];
            if (reservedByte != 0x00)
            {
                resultData[0] = ReturnCodes::DataVoid;
            }
            else
            {
                resultLength = kFunctionPropertyResultMaxExt; // tell the callee what the response can carry
                obj->command((PropertyID)propertyId, data, length, resultData, resultLength);
                // resultLength was modified by the callee
            }
        }
        else if (propType == PDT_CONTROL)
        {
            uint8_t count = 1;
            // guard: LE_ADDITIONAL_LOAD_CONTROLS reads 8 octets (see propertyValueWriteIndication); skip a short/corrupt one
            if (propertyId == PID_LOAD_STATE_CONTROL && length >= 1 && data[0] == LE_ADDITIONAL_LOAD_CONTROLS && length < 8)
                count = 0;
            else
                obj->writeProperty((PropertyID)propertyId, 1, data, count);
            if (count == 1)
            {
                // Read the current state (one byte only) for the response
                obj->readProperty((PropertyID)propertyId, 1, count, &resultData[1]);
                resultLength = count ? 2 : 1;
                resultData[0] = count ? ReturnCodes::Success : ReturnCodes::DataVoid;
            }
            else
            {
                resultData[0] = ReturnCodes::AddressVoid;
            }
        }
        else
        {
            resultData[0] = ReturnCodes::DataTypeConflict;
        }
    }
    else
    {
        resultData[0] = ReturnCodes::GenericError;
    }

    applicationLayer().functionPropertyExtStateResponse(AckRequested, priority, hopType, asap, secCtrl, objectType, objectInstance, propertyId, resultData, resultLength);
}

void BauSystemB::functionPropertyExtStateIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, ObjectType objectType, uint8_t objectInstance,
                                                    uint8_t propertyId, uint8_t* data, uint8_t length)
{
    if (length == 0) return; // the reserved input octet data[0] must be present; drop a truncated ext function-property state read
    uint8_t resultData[kFunctionPropertyResultBufferMaxSize];
    // Like the ExtCommand twin: start at the return code alone. The error paths below set only
    // resultData[0] and never touch resultLength, and 03_03_07 3.4.8.3 p.93 says such a response carries
    // no data field -- announcing the full buffer here would put uninitialised stack on the bus.
    // Only the length is fixed here: the codes those paths send still differ from the table on p.94,
    // which defines E_ADDRESS_VOID for a missing object or property. Changing them is a wire change.
    uint8_t resultLength = 1;

    InterfaceObject* obj = getInterfaceObject(objectType, objectInstance);
    if(obj)
    {
        Property* prop = obj->property((PropertyID)propertyId);
        PropertyDataType propType = prop != nullptr ? prop->Type() : (PropertyDataType)0; // null (unknown PID) -> non-FUNCTION sentinel, never deref null

        if (propType == PDT_FUNCTION)
        {
            // The first byte is reserved and 0 for PDT_FUNCTION
            uint8_t reservedByte = data[0];
            if (reservedByte != 0x00)
            {
                resultData[0] = ReturnCodes::DataVoid;
            }
            else
            {
                resultLength = kFunctionPropertyResultMaxExt; // tell the callee what the response can carry
                obj->state((PropertyID)propertyId, data, length, resultData, resultLength);
                // resultLength was modified by the callee
            }
        }
        else if (propType == PDT_CONTROL)
        {
            uint8_t count = 1;
            // Read the current state (one byte only) for the response
            obj->readProperty((PropertyID)propertyId, 1, count, &resultData[1]);
            resultLength = count ? 2 : 1;
            resultData[0] = count ? ReturnCodes::Success : ReturnCodes::DataVoid;
        }
        else
        {
            resultData[0] = ReturnCodes::DataTypeConflict;
        }
    }
    else
    {
        resultData[0] = ReturnCodes::GenericError;
    }

    applicationLayer().functionPropertyExtStateResponse(AckRequested, priority, hopType, asap, secCtrl, objectType, objectInstance, propertyId, resultData, resultLength);
}

void BauSystemB::individualAddressReadIndication(HopCountType hopType, const SecurityControl &secCtrl)
{
    if (_deviceObj.progMode())
        applicationLayer().individualAddressReadResponse(AckRequested, hopType, secCtrl);
}

void BauSystemB::individualAddressWriteIndication(HopCountType hopType, const SecurityControl &secCtrl, uint16_t newaddress)
{
    if (_deviceObj.progMode())
        _deviceObj.individualAddress(newaddress);
}

void BauSystemB::individualAddressSerialNumberWriteIndication(Priority priority, HopCountType hopType, const SecurityControl &secCtrl, uint16_t newIndividualAddress,
                                                          uint8_t* knxSerialNumber)
{
    // If the received serial number matches our serial number
    // then store the received new individual address in the device object
    if (!memcmp(knxSerialNumber, _deviceObj.propertyData(PID_SERIAL_NUMBER), 6))
        _deviceObj.individualAddress(newIndividualAddress);
}

void BauSystemB::individualAddressSerialNumberReadIndication(Priority priority, HopCountType hopType, const SecurityControl &secCtrl, uint8_t* knxSerialNumber)
{
    // If the received serial number matches our serial number
    // then send a response with the serial number. The domain address is set to 0 for closed media.
    // An open medium BAU has to override this method and provide a proper domain address.
    if (!memcmp(knxSerialNumber, _deviceObj.propertyData(PID_SERIAL_NUMBER), 6))
    {
        uint8_t emptyDomainAddress[2] = {0x00};
        applicationLayer().IndividualAddressSerialNumberReadResponse(priority, hopType, secCtrl, emptyDomainAddress, knxSerialNumber);
    }
}

void BauSystemB::addSaveRestore(SaveRestore* obj)
{
    _memory.addSaveRestore(obj);
}

#ifdef OPENKNX_FTC_CLIENT
// OPENKNX_FTC_CLIENT: client role for KnxFileTransfer (ObjectIndex 159) -- lets one device drive another's
// file transfer PA -> PA. The transfer state machine lives in the application, not here.
void BauSystemB::functionPropertyStateResponseIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl& secCtrl,
                                                         uint8_t objectIndex, uint8_t propertyId, uint8_t* data, uint8_t length)
{
    if (_ftcResponseCb != nullptr)
        _ftcResponseCb(asap, objectIndex, propertyId, data, length);
}

bool BauSystemB::ftcSendCommand(uint16_t asap, const SecurityControl secCtrl, uint8_t objectIndex, uint8_t propertyId, uint8_t* data, uint8_t length)
{
    // Stack-overflow guard: functionPropertyCommandRequest() memcpy's `length` (caller-controlled) bytes to
    // offset 13 of the CemiFrame stack buffer (0xFF + APDU_LPDU_DIFF = 264) -> payload fits 264-13 = 251 (exact
    // fill). 251 is the true max: octetCount = length+3 -> 254 (the last valid value; 255=0xFF is the reserved
    // escape and valid() drops it), and 3+251=254 does not wrap uint8. Needs NPDU::length() as uint16 (octetCount
    // 254 -> 256). Bound hard at 251.
    if (length > 251)
    {
        print("ftcSendCommand: length ");
        print(length);
        println(" > 251 -- rejected (would overflow the CemiFrame buffer)");
        return false;
    }

    // Connectionless by design (like the reference client) -- an isConnected() gate would reject every
    // frame. LowPriority, not System: a bulk upload runs ~34 min; System would win every arbitration and
    // starve time-critical group traffic. The server echoes the priority, so answers stay Low too.
    applicationLayer().functionPropertyCommandRequest(AckRequested, LowPriority, NetworkLayerParameter, asap, secCtrl,
                                                      objectIndex, propertyId, data, length);
    return true;
}

bool BauSystemB::ftcSendDeviceDescriptorRead(uint16_t asap, const SecurityControl secCtrl)
{
    // Scan probe: connectionless type-0 (mask version) read, LowPriority. AckDontCare, not AckRequested:
    // a scan hits many ABSENT addresses; AckRequested would retransmit each 3x -> a congestion storm that
    // drowns the real answers. Absent -> silence, present -> the DeviceDescriptor_Response is the proof.
    applicationLayer().deviceDescriptorReadRequest(AckDontCare, LowPriority, NetworkLayerParameter, asap, secCtrl, 0);
    return true;
}

void BauSystemB::deviceDescriptorReadAppLayerConfirm(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl& secCtrl,
                                                     uint8_t descriptortype, uint8_t* deviceDescriptor)
{
    // Park only -- the scan state machine in the client owns the follow-up.
    if (_ftcDdCb != nullptr)
        _ftcDdCb(asap, descriptortype, deviceDescriptor);
}

bool BauSystemB::ftcSendPropertyValueRead(uint16_t asap, const SecurityControl secCtrl, uint8_t objectIndex, uint8_t propertyId,
                                          uint8_t count, uint16_t startIndex)
{
    // AckRequested here (unlike the scan): one known-present target, so retry a lost read. LowPriority.
    applicationLayer().propertyValueReadRequest(AckRequested, LowPriority, NetworkLayerParameter, asap, secCtrl,
                                                objectIndex, propertyId, count, startIndex);
    return true;
}

void BauSystemB::propertyValueReadAppLayerConfirm(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl& secCtrl,
                                                  uint8_t objectIndex, uint8_t propertyId, uint8_t numberOfElements, uint16_t startIndex,
                                                  uint8_t* data, uint8_t length)
{
    // Park only -- the device-info state machine reads it back in loop().
    if (_ftcPropCb != nullptr)
        _ftcPropCb(asap, objectIndex, propertyId, data, length);
}

bool BauSystemB::ftcSendPropertyValueWrite(uint16_t asap, const SecurityControl secCtrl, uint8_t objectIndex, uint8_t propertyId,
                                           uint8_t count, uint16_t startIndex, uint8_t* data, uint8_t length)
{
    // Derived from propertyDataSend: it builds frame(5 + length) and its last payload byte lands on
    // buffer[14 + length], with buffer ending at 263. Callers pass a literal today and ignore the result.
    if (length > 249)
    {
        print("ftcSendPropertyValueWrite: length ");
        print(length);
        println(" > 249 -- rejected (would overflow the CemiFrame buffer)");
        return false;
    }

    // Fire-and-forget (`ftc <pa> led`): the target echoes a PropertyValue_Response onto _ftcPropCb; ignored.
    applicationLayer().propertyValueWriteRequest(AckRequested, LowPriority, NetworkLayerParameter, asap, secCtrl,
                                                 objectIndex, propertyId, count, startIndex, data, length);
    return true;
}

bool BauSystemB::ftcSendMemoryRead(uint16_t asap, const SecurityControl secCtrl, uint8_t number, uint16_t memoryAddress)
{
    // GA/assoc table walk: same connectionless, AckRequested, LowPriority path as ftcSendPropertyValueRead.
    applicationLayer().memoryReadRequest(AckRequested, LowPriority, NetworkLayerParameter, asap, secCtrl, number, memoryAddress);
    return true;
}

void BauSystemB::memoryReadAppLayerConfirm(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl& secCtrl,
                                           uint8_t number, uint16_t memoryAddress, uint8_t* data)
{
    // Park only -- the group-comm state machine reads it back in loop().
    if (_ftcMemCb != nullptr)
        _ftcMemCb(asap, memoryAddress, data, number);
}

bool BauSystemB::ftcSendAdcRead(uint16_t asap, const SecurityControl secCtrl, uint8_t channelNr, uint8_t readCount)
{
    // Same connectionless, AckRequested, LowPriority path as the other ftc reads.
    applicationLayer().adcReadRequest(AckRequested, LowPriority, NetworkLayerParameter, asap, secCtrl, channelNr, readCount);
    return true;
}

void BauSystemB::adcReadAppLayerConfirm(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl& secCtrl,
                                        uint8_t channelNr, uint8_t readCount, int16_t value)
{
    // Park only -- the device-info state machine reads it back in loop() (bus-voltage read).
    if (_ftcAdcCb != nullptr)
        _ftcAdcCb(asap, channelNr, readCount, value);
}

// --- CO scan shims (thin, because _connectedTsap is private to ApplicationLayer). Inherited by Bau091A. ---
bool BauSystemB::ftcScanConnect(uint16_t pa)
{
    // Self-guard: never step on an existing connection (an ETS session or a pending restart).
    if (applicationLayer().isConnected())
        return false;
    applicationLayer().connectRequest(pa, SystemPriority);
    return true;
}

bool BauSystemB::ftcScanConnected()
{
    return applicationLayer().isConnected();
}

void BauSystemB::ftcScanReadDescriptor(const SecurityControl& sec)
{
    applicationLayer().ftcDeviceDescriptorReadConnected(sec);
}

void BauSystemB::ftcScanDisconnect()
{
    // NOT gated on isConnected(): a failed TP connect leaves the transport stuck in Connecting, and only
    // disconnectRequest returns it to Closed -- so this must run on every scan exit path.
    applicationLayer().disconnectRequest(SystemPriority);
}
#endif

bool BauSystemB::restartRequest(uint16_t asap, const SecurityControl secCtrl)
{
    if (applicationLayer().isConnected())
        return false;
    _restartState = Connecting; // order important, has to be set BEFORE connectRequest
    _restartSecurity = secCtrl;
    applicationLayer().connectRequest(asap, SystemPriority);
    applicationLayer().deviceDescriptorReadRequest(AckRequested, SystemPriority, NetworkLayerParameter, asap, secCtrl, 0);
    return true;
}

void BauSystemB::connectConfirm(uint16_t tsap)
{
    if (_restartState == Connecting)
    {
        /* restart connection is confirmed, go to the next state */
        _restartState = Connected;
        _restartDelay = millis();
    }
    else
    {
        _restartState = Idle;
    }
}

void BauSystemB::nextRestartState()
{
    if (_selfResetPending && millis() - _selfResetAt >= kSelfResetDelayMs)
    {
        _selfResetPending = false;
        doMasterReset(_selfResetErase, _selfResetChannel);
        _memory.writeMemory();
        _platform.restart();
        return;
    }

    switch (_restartState)
    {
        case Idle:
            /* inactive state, do nothing */
            break;
        case Connecting:
            /* wait for connection, we do nothing here */
            break;
        case Connected:
            /* connection confirmed, we send restartRequest, but we wait a moment (sending ACK etc)... */
            if (millis() - _restartDelay > 30)
            {
                applicationLayer().restartRequest(AckRequested, SystemPriority, NetworkLayerParameter, _restartSecurity);
                _restartState = Restarted;
                _restartDelay = millis();
            }
            break;
        case Restarted:
            /* restart is finished, we send a disconnect */
            if (millis() - _restartDelay > 30)
            {
                applicationLayer().disconnectRequest(SystemPriority);
                _restartState = Idle;
            }
        default:
            break;
    }
}

void BauSystemB::systemNetworkParameterReadIndication(Priority priority, HopCountType hopType, const SecurityControl &secCtrl, uint16_t objectType,
                                                      uint16_t propertyId, uint8_t* testInfo, uint16_t testInfoLength)
{
    uint8_t operand;

    popByte(operand, testInfo + 1); // First byte (+ 0) contains only 4 reserved bits (0)

    // See KNX spec. 3.5.2 p.33 (Management Procedures: Procedures with A_SystemNetworkParameter_Read)
    switch((NmReadSerialNumberType)operand)
    {
        case NM_Read_SerialNumber_By_ProgrammingMode: // NM_Read_SerialNumber_By_ProgrammingMode
            // Only send a reply if programming mode is on
            if (_deviceObj.progMode() && (objectType == OT_DEVICE) && (propertyId == PID_SERIAL_NUMBER))
            {
                // Send reply. testResult data is KNX serial number
                applicationLayer().systemNetworkParameterReadResponse(priority, hopType, secCtrl, objectType, propertyId,
                                                             testInfo, testInfoLength, (uint8_t*)_deviceObj.propertyData(PID_SERIAL_NUMBER), 6);
            }
        break;

        case NM_Read_SerialNumber_By_ExFactoryState: // NM_Read_SerialNumber_By_ExFactoryState
        break;

        case NM_Read_SerialNumber_By_PowerReset: // NM_Read_SerialNumber_By_PowerReset
        break;

        case NM_Read_SerialNumber_By_ManufacturerSpecific: // Manufacturer specific use of A_SystemNetworkParameter_Read
        break;
    }
}

void BauSystemB::systemNetworkParameterReadLocalConfirm(Priority priority, HopCountType hopType, const SecurityControl &secCtrl, uint16_t objectType,
                                                         uint16_t propertyId, uint8_t* testInfo, uint16_t testInfoLength, bool status)
{
}

void BauSystemB::propertyValueRead(ObjectType objectType, uint8_t objectInstance, uint8_t propertyId,
                                   uint8_t &numberOfElements, uint16_t startIndex,
                                   uint8_t **data, uint32_t &length)
{
    uint32_t size = 0;
    uint8_t elementCount = numberOfElements;

    InterfaceObject* obj = getInterfaceObject(objectType, objectInstance);

    if (obj)
    {
        uint8_t elementSize = obj->propertySize((PropertyID)propertyId);
        if (startIndex > 0)
            size = elementSize * numberOfElements;
        else
            size = sizeof(uint16_t); // size of property array entry 0 which contains the current number of elements
        *data = new uint8_t [size];
        obj->readProperty((PropertyID)propertyId, startIndex, elementCount, *data);
    }
    else
    {
        elementCount = 0;
        *data = nullptr;
    }

    numberOfElements = elementCount;
    length = size;
}

void BauSystemB::propertyValueWrite(ObjectType objectType, uint8_t objectInstance, uint8_t propertyId,
                                    uint8_t &numberOfElements, uint16_t startIndex,
                                    uint8_t* data, uint32_t length)
{
    InterfaceObject* obj =  getInterfaceObject(objectType, objectInstance);
    if(obj)
    {
        // Memory-safety (same class as propertyValue(Ext)WriteIndication, but this is the cEMI-server feeder):
        // the length is passed but DataProperty::write() clamps count only against _maxElements and then
        // memcpy()s numberOfElements*ElementSize() from `data`. Reject a write claiming more element data than
        // the `length`-octet payload carries -> no over-read past the cEMI request buffer, no info-leak persisted.
        Property* prop = obj->property((PropertyID)propertyId);
        // see propertyValueWriteIndication: bound the LE_ADDITIONAL_LOAD_CONTROLS 8-octet read against the payload
        bool loadCtrlShort = (propertyId == PID_LOAD_STATE_CONTROL && length >= 1
                              && data[0] == LE_ADDITIONAL_LOAD_CONTROLS && length < 8);
        // The write-enable flag is deliberately not enforced here: this is the stack's own setter, and
        // OFM-Network writes the read-only PID_CURRENT_IP_ASSIGNMENT_METHOD through it. Both remote paths
        // check the flag at their own layer, where the origin is known. The length bound stays -- that is
        // memory-safety and applies to every caller.
        if (loadCtrlShort || (prop != nullptr && !propertyPayloadFits(prop, startIndex, numberOfElements, length)))
            numberOfElements = 0;
        else
            obj->writeProperty((PropertyID)propertyId, startIndex, data, numberOfElements);
    }
    else
        numberOfElements = 0;
}

Property* BauSystemB::property(ObjectType objectType, uint8_t objectInstance, uint8_t propertyId)
{
    InterfaceObject* obj = getInterfaceObject(objectType, objectInstance);
    return obj ? obj->property((PropertyID)propertyId) : nullptr;
}

Memory& BauSystemB::memory()
{
    return _memory;
}

void BauSystemB::versionCheckCallback(VersionCheckCallback func)
{
    _memory.versionCheckCallback(func);
}

VersionCheckCallback BauSystemB::versionCheckCallback()
{
    return _memory.versionCheckCallback();
}

void BauSystemB::beforeRestartCallback(BeforeRestartCallback func)
{
    _beforeRestart = func;
}

BeforeRestartCallback BauSystemB::beforeRestartCallback()
{
    return _beforeRestart;
}

void BauSystemB::functionPropertyCallback(FunctionPropertyCallback func)
{
    _functionProperty = func;
}

FunctionPropertyCallback BauSystemB::functionPropertyCallback()
{
    return _functionProperty;
}
void BauSystemB::functionPropertyStateCallback(FunctionPropertyCallback func)
{
    _functionPropertyState = func;
}

FunctionPropertyCallback BauSystemB::functionPropertyStateCallback()
{
    return _functionPropertyState;
}

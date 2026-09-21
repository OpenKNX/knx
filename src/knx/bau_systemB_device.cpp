#include "bau_systemB_device.h"
#include "bits.h"
#include <string.h>
#include <stdio.h>

BauSystemBDevice::BauSystemBDevice(Platform& platform) :
    BauSystemB(platform),
    _addrTable(_memory),
    _assocTable(_memory), _groupObjTable(_memory),
#ifdef USE_DATASECURE
    _appLayer(_deviceObj, _secIfObj, *this),
#else
    _appLayer(*this),
#endif
    _transLayer(_appLayer), _netLayer(_deviceObj, _transLayer)
{
    _appLayer.transportLayer(_transLayer);
    _appLayer.associationTableObject(_assocTable);
#ifdef USE_DATASECURE
    _appLayer.groupAddressTable(_addrTable);
#endif
    _transLayer.networkLayer(_netLayer);
    _transLayer.groupAddressTable(_addrTable);

    _memory.addSaveRestore(&_deviceObj);
    _memory.addSaveRestore(&_groupObjTable); // changed order for better memory management
    _memory.addSaveRestore(&_addrTable);
    _memory.addSaveRestore(&_assocTable);
#ifdef USE_DATASECURE
    _memory.addSaveRestore(&_secIfObj);
#endif
}

ApplicationLayer& BauSystemBDevice::applicationLayer()
{
    return _appLayer;
}

GroupObjectTableObject& BauSystemBDevice::groupObjectTable()
{
    return _groupObjTable;
}

void BauSystemBDevice::loop()
{
    _transLayer.loop();
    sendNextGroupTelegram();
    nextRestartState();
#ifdef USE_DATASECURE
    _appLayer.loop();
#endif
    _memory.loop();
}

void BauSystemBDevice::sendNextGroupTelegram()
{
    if(!configured())
        return;
    
    static uint16_t startIdx = 1;

    GroupObjectTableObject& table = _groupObjTable;
    uint16_t objCount = table.entryCount();

    for (uint16_t asap = startIdx; asap <= objCount; asap++)
    {
        GroupObject& go = table.get(asap);

        ComFlag flag = go.commFlag();
        if (flag != ReadRequest && flag != WriteRequest)
            continue;

        if (flag == WriteRequest) 
        {
#ifdef SMALL_GROUPOBJECT
            GroupObject::processClassCallback(go);
#else
            GroupObjectUpdatedHandler handler = go.callback();
            if (handler)
                handler(go);
#endif
        }
        if (!go.communicationEnable())
        {
            go.commFlag(Ok);
            continue;
        }

        SecurityControl goSecurity;
        goSecurity.toolAccess = false; // Secured group communication never uses the toolkey. ETS knows all keys, also the group keys.

#ifdef USE_DATASECURE
        // Get security flags from Security Interface Object for this group object
        goSecurity.dataSecurity = _secIfObj.getGroupObjectSecurity(asap);
#else
        goSecurity.dataSecurity = DataSecurity::None;
#endif

        if (flag == WriteRequest && go.transmitEnable())
        {
            uint8_t* data = go.valueRef();
            _appLayer.groupValueWriteRequest(AckRequested, asap, go.priority(), NetworkLayerParameter, goSecurity, data,
                go.sizeInTelegram());
        }
        else if (flag == ReadRequest)
        {
            _appLayer.groupValueReadRequest(AckRequested, asap, go.priority(), NetworkLayerParameter, goSecurity);
        }

        go.commFlag(Transmitting);

        startIdx = asap + 1;
        return;
    }

    startIdx = 1;
}

void BauSystemBDevice::updateGroupObject(GroupObject & go, uint8_t * data, uint8_t length)
{
    uint8_t* goData = go.valueRef();
    if (length != go.valueSize())
    {
        go.commFlag(Error);
        return;
    }

    memcpy(goData, data, length);

    if (go.commFlag() != WriteRequest)
    {
        go.commFlag(Updated);
#ifdef SMALL_GROUPOBJECT
        GroupObject::processClassCallback(go);
#else
        GroupObjectUpdatedHandler handler = go.callback();
        if (handler)
            handler(go);
#endif
    }
    else
    {
        go.commFlag(Updated);
    }
}

bool BauSystemBDevice::configured()
{
    // _configured is set to true initially, if the device was configured with ETS it will be set to true after restart
    
    if (!_configured)
        return false;
    
    _configured = _groupObjTable.loadState() == LS_LOADED
        && _addrTable.loadState() == LS_LOADED
        && _assocTable.loadState() == LS_LOADED
        && _appProgram.loadState() == LS_LOADED;

#ifdef USE_DATASECURE
    _configured &= _secIfObj.loadState() == LS_LOADED;
#endif

    return _configured;
}

void BauSystemBDevice::doMasterReset(EraseCode eraseCode, uint8_t channel)
{
    BauSystemB::doMasterReset(eraseCode, channel);

    _addrTable.masterReset(eraseCode, channel);
    _assocTable.masterReset(eraseCode, channel);
    _groupObjTable.masterReset(eraseCode, channel);
#ifdef USE_DATASECURE
    _secIfObj.masterReset(eraseCode, channel);
#endif
}

void BauSystemBDevice::groupValueWriteLocalConfirm(AckType ack, uint16_t asap, Priority priority, HopCountType hopType, const SecurityControl &secCtrl, uint8_t * data, uint8_t dataLength, bool status)
{
    // get() is unbounded (_groupObjects[asap-1]); a confirmation in flight across an unload/reload would
    // otherwise write past the newly allocated array.
    if (asap == 0 || asap > _groupObjTable.entryCount()) return;

    GroupObject& go = _groupObjTable.get(asap);
    if (status)
        go.commFlag(Ok);
    else
        go.commFlag(Error);
}

void BauSystemBDevice::groupValueReadLocalConfirm(AckType ack, uint16_t asap, Priority priority, HopCountType hopType, const SecurityControl &secCtrl, bool status)
{
    // get() is unbounded (_groupObjects[asap-1]); a confirmation in flight across an unload/reload would
    // otherwise write past the newly allocated array.
    if (asap == 0 || asap > _groupObjTable.entryCount()) return;

    GroupObject& go = _groupObjTable.get(asap);
    if (status)
        go.commFlag(Ok);
    else
        go.commFlag(Error);
}

void BauSystemBDevice::groupValueReadIndication(uint16_t asap, Priority priority, HopCountType hopType, const SecurityControl &secCtrl)
{

    if (asap == 0 || asap > _groupObjTable.entryCount()) return;

#ifdef USE_DATASECURE
    DataSecurity requiredGoSecurity;

    // Below the bounds check, on purpose: getGroupObjectSecurity() reads PID_GO_SECURITY_FLAGS into a VLA
    // sized by its element size, which is 1 for PDT_GENERIC_01. At index 0 a property read answers with
    // the element COUNT and pushWord() writes two octets, one past that buffer. asap 0 is the only input
    // that reaches it, and the bounds check above is what keeps it out.
    requiredGoSecurity = _secIfObj.getGroupObjectSecurity(asap);

    if (secCtrl.dataSecurity != requiredGoSecurity)
    {
        println("GroupValueRead: access denied due to wrong security flags");
        return;
    }
#endif
    GroupObject& go = _groupObjTable.get(asap);

    if (!go.communicationEnable() || !go.readEnable())
        return;
    
    uint8_t* data = go.valueRef();
    _appLayer.groupValueReadResponse(AckRequested, asap, priority, hopType, secCtrl, data, go.sizeInTelegram());
}

void BauSystemBDevice::groupValueReadAppLayerConfirm(uint16_t asap, Priority priority, HopCountType hopType, const SecurityControl &secCtrl, uint8_t* data,
    uint8_t dataLength)
{
    if (asap == 0 || asap > _groupObjTable.entryCount()) return;

#ifdef USE_DATASECURE
    // A received A_GroupValue_Response updates the object exactly as a write does, so it has to meet the
    // same requirement. Only the read and the write path checked it, so an UNSECURED response was taken
    // into an object configured for secured group communication - the one shape of this service that
    // carries a value into the device without being a write.
    if (secCtrl.dataSecurity != _secIfObj.getGroupObjectSecurity(asap))
    {
        println("GroupValueResponse: access denied due to wrong security flags");
        return;
    }
#endif

    GroupObject& go = _groupObjTable.get(asap);

    if (!go.communicationEnable() || !go.responseUpdateEnable())
        return;

    updateGroupObject(go, data, dataLength);
}

void BauSystemBDevice::groupValueWriteIndication(uint16_t asap, Priority priority, HopCountType hopType, const SecurityControl &secCtrl, uint8_t * data, uint8_t dataLength)
{
    if (asap == 0 || asap > _groupObjTable.entryCount()) return;

#ifdef USE_DATASECURE
    DataSecurity requiredGoSecurity;

    // Below the bounds check, on purpose: getGroupObjectSecurity() reads PID_GO_SECURITY_FLAGS into a VLA
    // sized by its element size, which is 1 for PDT_GENERIC_01. At index 0 a property read answers with
    // the element COUNT and pushWord() writes two octets, one past that buffer. asap 0 is the only input
    // that reaches it, and the bounds check above is what keeps it out.
    requiredGoSecurity = _secIfObj.getGroupObjectSecurity(asap);

    if (secCtrl.dataSecurity != requiredGoSecurity)
    {
        println("GroupValueWrite: access denied due to wrong security flags");
        return;
    }
#endif
    GroupObject& go = _groupObjTable.get(asap);

    if (!go.communicationEnable() || !go.writeEnable())
        return;

    updateGroupObject(go, data, dataLength);
}

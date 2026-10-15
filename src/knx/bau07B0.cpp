#include "config.h"
#if MASK_VERSION == 0x07B0 && !defined(KNX_TUNNELING)

#include "bau07B0.h"
#include "bits.h"
#include <string.h>
#include <stdio.h>

using namespace std;

Bau07B0::Bau07B0(Platform& platform)
    : BauSystemBDevice(platform),
      _dlLayer(_deviceObj, _netLayer.getInterface(), _platform, *this, (ITpUartCallBacks&) *this, (DataLinkLayerCallbacks*) this),
      DataLinkLayerCallbacks()
#ifdef USE_CEMI_SERVER
    , _cemiServer(*this)
#endif           
{
    _netLayer.getInterface().dataLinkLayer(_dlLayer);
#ifdef USE_CEMI_SERVER
    _cemiServerObject.setMediumTypeAsSupported(DptMedium::KNX_TP1);
    _cemiServer.dataLinkLayer(_dlLayer);
    _dlLayer.cemiServer(_cemiServer);
    _memory.addSaveRestore(&_cemiServerObject);
#endif
    // Set Mask Version in Device Object depending on the BAU
    _deviceObj.maskVersion(0x07B0);

    // The interface objects in index order: the position in this table is the object index (the number in
    // the comment). PID_IO_LIST and getInterfaceObject(idx) are both built from it, so they cannot differ.
    InterfaceObject* const objects[] = {
        &_deviceObj,         // 0  Device Object
        &_addrTable,         // 1  Group Address Table
        &_assocTable,        // 2  Group Association Table
        &_groupObjTable,     // 3  Group Object Table
        &_appProgram,        // 4  Application Program
        &_appProgram2,       // 5  Application Program 2 (Interfaceprogram Object)
#ifdef USE_DATASECURE
        &_secIfObj,          // next  Security Interface Object
#endif
#ifdef USE_CEMI_SERVER
        &_cemiServerObject,  // next  cEMI Server Object
#endif
    };
    static_assert(sizeof(objects) / sizeof(objects[0]) <= KNX_MAX_INTERFACE_OBJECTS, "PID_IO_LIST is too short");
    interfaceObjects(objects, sizeof(objects) / sizeof(objects[0]));
}

InterfaceObject* Bau07B0::getInterfaceObject(ObjectType objectType, uint16_t objectInstance)
{
    // We do not use it right now. 
    // Required for coupler mode as there are multiple router objects for example
    (void) objectInstance;

    switch (objectType)
    {
        case OT_DEVICE:
            return &_deviceObj;
        case OT_ADDR_TABLE:
            return &_addrTable;
        case OT_ASSOC_TABLE:
            return &_assocTable;
        case OT_GRP_OBJ_TABLE:
            return &_groupObjTable;
        case OT_APPLICATION_PROG:
            return &_appProgram;
        case OT_INTERFACE_PROG:
            return &_appProgram2;
#ifdef USE_DATASECURE
        case OT_SECURITY:
            return &_secIfObj;
#endif
#ifdef USE_CEMI_SERVER
        case OT_CEMI_SERVER:
            return &_cemiServerObject;
#endif            
        default:
            return nullptr;
    }
}

bool Bau07B0::enabled()
{
    return _dlLayer.enabled();
}

void Bau07B0::enabled(bool value)
{
    _dlLayer.enabled(value);
}

void Bau07B0::loop()
{
    _dlLayer.loop();
    BauSystemBDevice::loop();
#ifdef USE_CEMI_SERVER    
    _cemiServer.loop();
#endif    
}

TPAckType Bau07B0::isAckRequired(uint16_t address, bool isGrpAddr)
{
    if (isGrpAddr)
    {
        // ACK for broadcasts
        if (address == 0)
            return TPAckType::AckReqAck;
        // is group address in group address table? ACK if yes.
        if(_addrTable.contains(address))
            return TPAckType::AckReqAck;
        else
            return TPAckType::AckReqNone;
    }

    // Also ACK for our own individual address
    if (address  == _deviceObj.individualAddress())
        return TPAckType::AckReqAck;

    if (address == 0)
    {
        println("Invalid broadcast detected: destination address is 0, but address type is \"individual\"");
    }

    return TPAckType::AckReqNone;
}

TpUartDataLinkLayer* Bau07B0::getDataLinkLayer() {
    return (TpUartDataLinkLayer*)&_dlLayer;
}
#endif

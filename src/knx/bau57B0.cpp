#include "config.h"
#if MASK_VERSION == 0x57B0

#include "bau57B0.h"
#include "bits.h"
#include <string.h>
#include <stdio.h>

using namespace std;

Bau57B0::Bau57B0(Platform& platform)
    : BauSystemBDevice(platform),
      _ipParameters(_deviceObj, platform),
      _dlLayer(_deviceObj, _ipParameters, _netLayer.getInterface(), _platform, *this, (DataLinkLayerCallbacks*) this),
      DataLinkLayerCallbacks()
#ifdef USE_CEMI_SERVER
      ,
      _cemiServer(*this)
#endif
{
    _netLayer.getInterface().dataLinkLayer(_dlLayer);
#ifdef USE_CEMI_SERVER
    _dlLayer.cemiServerObject(_cemiServerObject);
    _cemiServerObject.setMediumTypeAsSupported(DptMedium::KNX_IP);
    _cemiServerObject.maxApduLength(_deviceObj.maxApduLength());
    _cemiServer.dataLinkLayer(_dlLayer);
    _cemiServerObject.maxApduLength(_deviceObj.maxApduLength());
    _dlLayer.cemiServer(_cemiServer);
    _memory.addSaveRestore(&_cemiServerObject);
#endif
    _memory.addSaveRestore(&_ipParameters);

    // Set Mask Version in Device Object depending on the BAU
    _deviceObj.maskVersion(0x57B0);

    // The interface objects in index order: the position in this table is the object index (the number in
    // the comment). PID_IO_LIST and getInterfaceObject(idx) are both built from it, so they cannot differ.
    InterfaceObject* const objects[] = {
        &_deviceObj,         // 0  Device Object
        &_addrTable,         // 1  Group Address Table
        &_assocTable,        // 2  Group Association Table
        &_groupObjTable,     // 3  Group Object Table
        &_appProgram,        // 4  Application Program
        &_appProgram2,       // 5  Application Program 2 (Interfaceprogram Object)
        &_ipParameters,      // 6  KNXnet/IP Parameter Object
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

InterfaceObject* Bau57B0::getInterfaceObject(ObjectType objectType, uint16_t objectInstance)
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
        case OT_IP_PARAMETER:
            return &_ipParameters;
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

void Bau57B0::doMasterReset(EraseCode eraseCode, uint8_t channel)
{
    // Device objects plus the address, association and group object tables
    BauSystemBDevice::doMasterReset(eraseCode, channel);

    _ipParameters.masterReset(eraseCode, channel);
}

bool Bau57B0::enabled()
{
    return _dlLayer.enabled();
}

void Bau57B0::enabled(bool value)
{
    _dlLayer.enabled(value);
}

// Rebuild the KNXnet/IP endpoint after the IP interface changed, so the device keeps answering
// SEARCH_REQUEST for its whole operational life.
bool Bau57B0::networkChanged(bool afterOutage)
{
    return _dlLayer.networkChanged(afterOutage);
}

void Bau57B0::loop()
{
    _dlLayer.loop();
    BauSystemBDevice::loop();
#ifdef USE_CEMI_SERVER
    _cemiServer.loop();
#endif
}

IpDataLinkLayer* Bau57B0::getDataLinkLayer() {
    return (IpDataLinkLayer*)&_dlLayer;
}
#endif

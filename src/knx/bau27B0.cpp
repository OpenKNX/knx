#include "config.h"
#if MASK_VERSION == 0x27B0

#include "bau27B0.h"
#include "bits.h"
#include <string.h>
#include <stdio.h>

using namespace std;

Bau27B0::Bau27B0(Platform& platform)
    : BauSystemBDevice(platform),
      _dlLayer(_deviceObj, _rfMediumObj, _netLayer.getInterface(), _platform, *this)
#ifdef USE_CEMI_SERVER
    , _cemiServer(*this)
#endif      
{
    _netLayer.getInterface().dataLinkLayer(_dlLayer);
    _memory.addSaveRestore(&_rfMediumObj);
#ifdef USE_CEMI_SERVER
    _cemiServerObject.setMediumTypeAsSupported(DptMedium::KNX_RF);
    _cemiServer.dataLinkLayer(_dlLayer);
    _dlLayer.cemiServer(_cemiServer);
    _memory.addSaveRestore(&_cemiServerObject);
#endif

    // Set Mask Version in Device Object depending on the BAU
    _deviceObj.maskVersion(0x27B0);

    // Set the maximum APDU length
    // ETS will consider this value while programming the device
    // For KNX-RF we use a smallest allowed value for now,
    // although long frame are also supported by the implementation.
    // Needs some experimentation.
    _deviceObj.maxApduLength(15);
#ifdef USE_CEMI_SERVER
    // Copies, does not track - must stay after the line above.
    _cemiServerObject.maxApduLength(_deviceObj.maxApduLength());
#endif

    // The interface objects in index order (KNX AN160 p.74 for mask 27B0): the position in this table is
    // the object index (the number in the comment). PID_IO_LIST and getInterfaceObject(idx) are both built
    // from it, so they cannot differ.
    InterfaceObject* const objects[] = {
        &_deviceObj,         // 0  Device Object
        &_addrTable,         // 1  Group Address Table
        &_assocTable,        // 2  Group Association Table
        &_groupObjTable,     // 3  Group Object Table
        &_appProgram,        // 4  Application Program
        &_appProgram2,       // 5  Application Program 2 (Interfaceprogram Object)
        &_rfMediumObj,       // 6  RF Medium Object
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

InterfaceObject* Bau27B0::getInterfaceObject(ObjectType objectType, uint16_t objectInstance)
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
        case OT_RF_MEDIUM:
            return &_rfMediumObj;
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

void Bau27B0::doMasterReset(EraseCode eraseCode, uint8_t channel)
{
    // Device objects plus the address, association and group object tables
    BauSystemBDevice::doMasterReset(eraseCode, channel);

    _rfMediumObj.masterReset(eraseCode, channel);
}

bool Bau27B0::enabled()
{
    return _dlLayer.enabled();
}

void Bau27B0::enabled(bool value)
{
    _dlLayer.enabled(value);
}

void Bau27B0::loop()
{
    _dlLayer.loop();
    BauSystemBDevice::loop();
#ifdef USE_CEMI_SERVER    
    _cemiServer.loop();
#endif    
}

void Bau27B0::domainAddressSerialNumberWriteIndication(Priority priority, HopCountType hopType, const SecurityControl &secCtrl, const uint8_t* rfDoA,
                                                       const uint8_t* knxSerialNumber)
{
    // If the received serial number matches our serial number
    // then store the received RF domain address in the RF medium object
    if (!memcmp(knxSerialNumber, _deviceObj.propertyData(PID_SERIAL_NUMBER), 6))
        _rfMediumObj.rfDomainAddress(rfDoA);
}

void Bau27B0::domainAddressSerialNumberReadIndication(Priority priority, HopCountType hopType, const SecurityControl &secCtrl, const uint8_t* knxSerialNumber)
{
    // If the received serial number matches our serial number
    // then send a response with the current RF domain address stored in the RF medium object
    if (!memcmp(knxSerialNumber, _deviceObj.propertyData(PID_SERIAL_NUMBER), 6))
        _appLayer.domainAddressSerialNumberReadResponse(priority, hopType, secCtrl, _rfMediumObj.rfDomainAddress(), knxSerialNumber);
}

void Bau27B0::individualAddressSerialNumberReadIndication(Priority priority, HopCountType hopType, const SecurityControl &secCtrl, uint8_t* knxSerialNumber)
{
    #pragma warning "individualAddressSerialNumberReadIndication is not available for rf"
}

void Bau27B0::domainAddressSerialNumberWriteLocalConfirm(Priority priority, HopCountType hopType, const SecurityControl &secCtrl, const uint8_t* rfDoA,
                                                         const uint8_t* knxSerialNumber, bool status)
{
}

void Bau27B0::domainAddressSerialNumberReadLocalConfirm(Priority priority, HopCountType hopType, const SecurityControl &secCtrl, const uint8_t* knxSerialNumber, bool status)
{
}

RfDataLinkLayer* Bau27B0::getDataLinkLayer() {
    return (RfDataLinkLayer*)&_dlLayer;
}
#endif // #ifdef USE_RF

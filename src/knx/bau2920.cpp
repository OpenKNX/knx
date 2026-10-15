#include "config.h"
#if MASK_VERSION == 0x2920

#include "bau2920.h"
#include "bits.h"
#include <string.h>
#include <stdio.h>

using namespace std;

// Mask 0x2920 uses coupler model 2.0
Bau2920::Bau2920(Platform& platform)
    : BauSystemBCoupler(platform),
      _rtObjPrimary(memory()),
      _rtObjSecondary(memory()),
      _rfMediumObject(),
      _dlLayerPrimary(_deviceObj, _netLayer.getPrimaryInterface(), _platform, *this, (ITpUartCallBacks&) *this),
      _dlLayerSecondary(_deviceObj, _rfMediumObject, _netLayer.getSecondaryInterface(), platform, *this)
#ifdef USE_CEMI_SERVER
      ,
      _cemiServer(*this)
#endif
{
    // Before accessing anything of the two router objects they have to be initialized according to the used media combination
    // Coupler model 2.0
    _rtObjPrimary.initialize20(1, DptMedium::KNX_TP1, RouterObjectType::Primary, 201);
    _rtObjSecondary.initialize20(2, DptMedium::KNX_RF, RouterObjectType::Secondary, 201);

    _netLayer.rtObjPrimary(_rtObjPrimary);
    _netLayer.rtObjSecondary(_rtObjSecondary);
    _netLayer.getPrimaryInterface().dataLinkLayer(_dlLayerPrimary);
    _netLayer.getSecondaryInterface().dataLinkLayer(_dlLayerSecondary);

#ifdef USE_CEMI_SERVER
    _cemiServerObject.setMediumTypeAsSupported(DptMedium::KNX_TP1);
    _cemiServerObject.setMediumTypeAsSupported(DptMedium::KNX_RF);
    _cemiServer.dataLinkLayer(_dlLayerSecondary); // Secondary I/F is the important one!
    _dlLayerSecondary.cemiServer(_cemiServer);
    _memory.addSaveRestore(&_cemiServerObject);
#endif

    _memory.addSaveRestore(&_rtObjPrimary);
    _memory.addSaveRestore(&_rtObjSecondary);

    _memory.addSaveRestore(&_rfMediumObject);

    // Set Mask Version in Device Object depending on the BAU
    _deviceObj.maskVersion(0x2920);

    // The interface objects in index order: the position in this table is the object index (the number in
    // the comment). PID_IO_LIST and getInterfaceObject(idx) are both built from it, so they cannot differ.
    InterfaceObject* const objects[] = {
        &_deviceObj,         // 0  Device Object
        &_rtObjPrimary,      // 1  Router Object, primary side
        &_rtObjSecondary,    // 2  Router Object, secondary side
        &_appProgram,        // 3  Application Program
        &_rfMediumObject,    // 4  RF Medium Object
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

InterfaceObject* Bau2920::getInterfaceObject(ObjectType objectType, uint16_t objectInstance)
{
    // We do not use it right now. 
    // Required for coupler mode as there are multiple router objects for example
    (void) objectInstance;

    switch (objectType)
    {
        case OT_DEVICE:
            return &_deviceObj;
        case OT_ROUTER:
            return objectInstance == 0 ? &_rtObjPrimary : &_rtObjSecondary;
        case OT_APPLICATION_PROG:
            return &_appProgram;
        case OT_RF_MEDIUM:
            return &_rfMediumObject;
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

void Bau2920::doMasterReset(EraseCode eraseCode, uint8_t channel)
{
    // Common SystemB objects
    BauSystemBCoupler::doMasterReset(eraseCode, channel);

    _rfMediumObject.masterReset(eraseCode, channel);
    _rtObjPrimary.masterReset(eraseCode, channel);
    _rtObjSecondary.masterReset(eraseCode, channel);
}

bool Bau2920::enabled()
{
    return _dlLayerPrimary.enabled() && _dlLayerSecondary.enabled();
}

void Bau2920::enabled(bool value)
{
    _dlLayerPrimary.enabled(value);
    _dlLayerSecondary.enabled(value);
}

void Bau2920::loop()
{
    _dlLayerPrimary.loop();
    _dlLayerSecondary.loop();
    BauSystemBCoupler::loop();
}

TpUartDataLinkLayer* Bau2920::getPrimaryDataLinkLayer() {
    return (TpUartDataLinkLayer*)&_dlLayerPrimary;
}

RfDataLinkLayer* Bau2920::getSecondaryDataLinkLayer() {
    return (RfDataLinkLayer*)&_dlLayerSecondary;
}
#endif

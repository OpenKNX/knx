#pragma once

#include "interface_object.h"

#define LEN_HARDWARE_TYPE 6

class DeviceObject: public InterfaceObject
{
public:
    // increase this version anytime DeviceObject-API changes 
    // the following value represents the serialized representation of DeviceObject.
    const uint16_t apiVersion = 2;
    
    DeviceObject();
    uint8_t* save(uint8_t* buffer) override;
    const uint8_t* restore(const uint8_t* buffer) override;
    uint16_t saveSize() override;

    uint16_t individualAddress();
    void individualAddress(uint16_t value);

    void individualAddressDuplication(bool value);
    bool verifyMode();
    void verifyMode(bool value);
    bool progMode();
    void progMode(bool value);
    uint16_t manufacturerId();
    void manufacturerId(uint16_t value);
    uint32_t bauNumber();
    void bauNumber(uint32_t value);
    const uint8_t* orderNumber();
    void orderNumber(const uint8_t* value);
    const uint8_t* hardwareType();
    void hardwareType(const uint8_t* value);
    uint16_t version();
    void version(uint16_t value);
    uint16_t maskVersion();
    void maskVersion(uint16_t value);
    uint16_t maxApduLength();
    void maxApduLength(uint16_t value);
    const uint8_t* rfDomainAddress();
    void rfDomainAddress(uint8_t* value);
    uint8_t defaultHopCount();

    // PID_DOWNLOAD_COUNTER (30): read-only change token, held in RAM here -- deliberately NOT in
    // save/restore, so the knx NVM layout and apiVersion stay put (a bump would wipe the PA + config).
    // Persisting it across reboot is OPTIONAL and left to the product: keep downloadCounter() in one of
    // its OpenKNX modules (flashSize/readFlash/writeFlash) -- see OAM-IP-Interface's IPInterfaceModule
    // for the pattern. A product that does not is fine: the counter simply resets on reboot and ETS then
    // does a full download. Nothing else may assume it is persisted.
    uint16_t downloadCounter();
    void downloadCounter(uint16_t value);
    void incrementDownloadCounter();
private:
    uint8_t _prgMode = 0;
    uint16_t _downloadCounter = 0;
    // Latch for the standard "+1 on first change since the last read" rule: armed by a PID read, spent
    // by the first downloadable change, so one download session = +1 (not +1 per table). RAM-only.
    bool _downloadCounterArmed = true;
#if MASK_VERSION == 0x091A || MASK_VERSION == 0x2920
    uint16_t _ownAddress = 0xFF00; // 15.15.0; couplers have 15.15.0 as default PA
#else
    uint16_t _ownAddress = 0xFFFF; // 15.15.255;
#endif
};

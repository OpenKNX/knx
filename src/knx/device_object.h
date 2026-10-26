#pragma once

#include "interface_object.h"

#define LEN_HARDWARE_TYPE 6
#define LEN_ORDER_INFO 10
#define LEN_PRODUCT_ID 10
// PID_MFR_STRUCT_INFO: "OpenKNX", layout version of the manufacturer specific Properties, bit per present PID
// (bit n = PID 202 + n). 9 octets is the most a single-element read fits into an L_Data_Standard Frame.
#define LEN_MFR_STRUCT_INFO 9
#define MFR_STRUCT_VERSION 1

// Element limits of the descriptive text Properties. They bound both the reported max_nr_of_elem
// (A_PropertyDescription_Read) and the number of characters a read can reach.
#define MAX_ELEM_OBJECT_NAME 80
#define MAX_ELEM_MANUFACTURER_DATA 40
#define MAX_ELEM_FIRMWARE_NAME 48
#define MAX_ELEM_BUILD_VERSION 16
#define MAX_ELEM_MCU 16

// The descriptive setters below exist: deviceName, hardwareId, firmwareName, buildVersion,
// buildTimestamp, firmwareRevision and productId. Clients gate on this so they also build against
// stacks that predate them. It lives here, next to the setters, and not in config.h - that file is
// wrapped in NO_KNX_CONFIG and would drop the flag while the feature is present.
#define KNX_HAS_DEVICE_INFORMATION 1
// The mcu setter exists. A flag of its own, because it came after the ones above.
#define KNX_HAS_DEVICE_MCU 1

class DeviceObject: public InterfaceObject
{
public:
    // increase this version anytime DeviceObject-API changes 
    // the following value represents the serialized representation of DeviceObject.
    const uint16_t apiVersion = 3; // 3: the NVM header carries the layout word
    
    DeviceObject();
    uint8_t* save(uint8_t* buffer) override;
    const uint8_t* restore(const uint8_t* buffer) override;
    uint16_t saveSize() override;
    // Master Reset (03_05_02 3.7.1.2): ResetIA and a factory reset WITH IA restore the individual address
    // to the medium default; factory-reset-without-IA and every other code leave it.
    void masterReset(EraseCode eraseCode, uint8_t channel) override;

    uint16_t individualAddress();
    void individualAddress(uint16_t value);

    void individualAddressDuplication(bool value);
    bool verifyMode();
    void verifyMode(bool value);
    /** @brief Safe State Control, PID 14 bit 3 (03_05_01 4.2.14.8). */
    void safeState(bool value);
    void writeProperty(PropertyID id, uint16_t start, uint8_t* data, uint8_t& count) override;
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
    const uint8_t* productId();
    void productId(const uint8_t* value);
    uint8_t firmwareRevision();
    void firmwareRevision(uint8_t value);
    uint8_t maxRetryCount();

    // The three text sources stay pointers into flash; the Properties convert to ISO 8859-1 on read
    // (03_05_01 4.2.2/4.2.21.1: one octet per element) and never copy the string. The caller must pass a
    // string with static storage duration - nothing copies it, so a temporary would leave a dangling read.
    void deviceName(const char* utf8Value);
    void hardwareId(const char* value);
    void firmwareName(const char* value);
    void buildVersion(const char* value);
    void mcu(const char* value);
    void buildTimestamp(uint32_t value);
    uint32_t buildTimestamp();
    uint16_t version();
    void version(uint16_t value);
    uint16_t maskVersion();
    void maskVersion(uint16_t value);
    uint16_t maxApduLength();
    void maxApduLength(uint16_t value);
    const uint8_t* rfDomainAddress();
    void rfDomainAddress(uint8_t* value);
    uint8_t defaultHopCount();

private:
    uint8_t _prgMode = 0;
    // 03_05_01 4.3.3: 3 busy repetitions and 3 Nack repetitions. Matches the TPUart driver default;
    // a write is accepted and reported back but is not yet pushed to the data link layer, and is not
    // persisted - giving it a flash slot would change saveSize() and require an apiVersion bump.
    uint8_t _maxRetryCount = 0x33;
    uint8_t _firmwareRevision = 0;
    uint8_t _productId[LEN_PRODUCT_ID] = {0};
    bool _hasProductId = false; // false -> PID_PRODUCT_ID answers 0 elements instead of zeroes
    const char* _deviceName = nullptr;
    // PID_OBJECT_NAME may be overwritten by a management client (03_05_01 4.2.2). The buffer is taken
    // from the heap once, on the first write, and never grows. There is no non-volatile store for it,
    // so a reset falls back to the manufacturer name in _deviceName.
    uint8_t* _objectName = nullptr;
    uint8_t _objectNameLength = 0;
    bool _objectNameWritten = false;
    const char* _hardwareId = nullptr;
    const char* _firmwareName = nullptr;
    const char* _buildVersion = nullptr;  // "X.Y.Z+githash" of the product build
    const char* _mcu = nullptr;           // chip the firmware was built for, e.g. "RP2040"
    uint32_t _buildTimestamp = 0;         // seconds since the epoch, 0 = not set
#if MASK_VERSION == 0x091A || MASK_VERSION == 0x2920
    uint16_t _ownAddress = 0xFF00; // 15.15.0; couplers have 15.15.0 as default PA
#else
    uint16_t _ownAddress = 0xFFFF; // 15.15.255;
#endif
};

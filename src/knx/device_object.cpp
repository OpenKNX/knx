#include <cstring>
#include "device_object.h"
#include "bits.h"
#include "data_property.h"
#include "callback_property.h"
#include "config.h"

#define LEN_KNX_SERIAL 6

/**
 * @brief Decode one UTF-8 sequence into a single ISO 8859-1 octet.
 * Code points above FFh and malformed sequences become '?'. Never reads past the terminating NUL.
 * @return number of source bytes consumed
 */
static uint8_t utf8ToLatin1(const char* text, uint8_t& value)
{
    const uint8_t lead = (uint8_t)text[0];

    if (lead < 0x80)
    {
        value = lead;
        return 1;
    }

    uint8_t follow = 0;
    uint32_t codePoint = 0;

    if ((lead & 0xE0) == 0xC0)
    {
        follow = 1;
        codePoint = lead & 0x1F;
    }
    else if ((lead & 0xF0) == 0xE0)
    {
        follow = 2;
        codePoint = lead & 0x0F;
    }
    else if ((lead & 0xF8) == 0xF0)
    {
        follow = 3;
        codePoint = lead & 0x07;
    }
    else
    {
        // stray continuation byte
        value = '?';
        return 1;
    }

    for (uint8_t i = 1; i <= follow; i++)
    {
        // a NUL fails this test too, so a truncated sequence stops before the string end
        if (((uint8_t)text[i] & 0xC0) != 0x80)
        {
            value = '?';
            return i;
        }
    }

    for (uint8_t i = 1; i <= follow; i++)
        codePoint = (codePoint << 6) | ((uint8_t)text[i] & 0x3F);

    value = (codePoint <= 0xFF) ? (uint8_t)codePoint : '?';
    return follow + 1;
}

/**
 * @brief Length of an UTF-8 string measured in ISO 8859-1 characters, capped at maxElements.
 */
static uint16_t latin1Length(const char* text, uint16_t maxElements)
{
    uint16_t length = 0;

    while (*text != 0 && length < maxElements)
    {
        uint8_t value = 0;
        text += utf8ToLatin1(text, value);
        length++;
    }

    return length;
}

/**
 * @brief Serve a read on a descriptive text Property backed by an UTF-8 string in flash.
 * Start index 0 answers the current number of elements; a read beyond it answers with 0 elements
 * (03_03_07 3.4.4.1 exception handling).
 */
static uint8_t readText(const char* text, uint16_t maxElements, uint16_t start, uint8_t count, uint8_t* data)
{
    const uint16_t length = (text == nullptr) ? 0 : latin1Length(text, maxElements);

    if (start == 0)
    {
        pushWord(length, data);
        return 1;
    }

    if (length == 0 || start > length || (uint16_t)(start + count) > (uint16_t)(length + 1))
        return 0;

    uint16_t index = 0;
    uint8_t written = 0;

    while (*text != 0 && written < count)
    {
        uint8_t value = 0;
        text += utf8ToLatin1(text, value);

        if (index++ < (uint16_t)(start - 1))
            continue;

        data[written++] = value;
    }

    return written;
}

DeviceObject::DeviceObject()
{
    // Default to KNXA (0xFA)
    // Note: ETS does not accept a SN 00FA00000000 in data secure mode.
    //       ETS says that 00FA00000000 looks "suspicious" in the log file.
    uint8_t serialNumber[] = {0x00, 0xFA, 0x01, 0x02, 0x03, 0x04};
    uint8_t hardwareType[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    Property* properties[] =
    {
        new DataProperty(PID_OBJECT_TYPE, false, PDT_UNSIGNED_INT, 1, ReadLv3 | WriteLv0, (uint16_t)OT_DEVICE),
        // 03_05_01 4.2.2: in the Device Object this Property is the name of the device.
        // KNX_DEVICE_NAME_WRITEABLE turns it into what Volume 6 A.2.3 asks for on the masks that list it,
        // (3/3) - optional, but writeable where present. It is off by default: a written name cannot be
        // persisted here, and the manufacturer name is always available read-only on PID_MFR_DEVICE_NAME.
#ifdef KNX_DEVICE_NAME_WRITEABLE
        new CallbackProperty<DeviceObject>(this, PID_OBJECT_NAME, true, PDT_UNSIGNED_CHAR, MAX_ELEM_OBJECT_NAME, ReadLv3 | WriteLv3,
            [](DeviceObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t
            {
                if (!io->_objectNameWritten)
                    return readText(io->_deviceName, MAX_ELEM_OBJECT_NAME, start, count, data);

                if (start == 0)
                {
                    pushWord(io->_objectNameLength, data);
                    return 1;
                }

                if (io->_objectNameLength == 0 || start > io->_objectNameLength ||
                    (uint16_t)(start + count) > (uint16_t)(io->_objectNameLength + 1))
                    return 0;

                memcpy(data, io->_objectName + (start - 1), count);
                return count;
            },
            [](DeviceObject* io, uint16_t start, uint8_t count, const uint8_t* data) -> uint8_t
            {
                if (start == 0)
                {
                    // 03_03_07 3.4.4.2: writing zero to element 0 empties the array. Only the first octet
                    // is evaluated - the write indication does not carry the payload length down here.
                    if (count != 1 || data[0] != 0) return 0;

                    io->_objectNameLength = 0;
                    io->_objectNameWritten = true;
                    return 1;
                }

                if ((uint16_t)(start + count) > (uint16_t)(MAX_ELEM_OBJECT_NAME + 1)) return 0;

                if (io->_objectName == nullptr)
                {
                    io->_objectName = new uint8_t[MAX_ELEM_OBJECT_NAME];
                    if (io->_objectName == nullptr) return 0;
                    memset(io->_objectName, 0, MAX_ELEM_OBJECT_NAME);
                }

                memcpy(io->_objectName + (start - 1), data, count);
                if ((uint16_t)(start - 1 + count) > io->_objectNameLength)
                    io->_objectNameLength = (uint8_t)(start - 1 + count);
                io->_objectNameWritten = true;
                return count;
            }),
#else
        new CallbackProperty<DeviceObject>(this, PID_OBJECT_NAME, false, PDT_UNSIGNED_CHAR, MAX_ELEM_OBJECT_NAME, ReadLv3 | WriteLv0,
            [](DeviceObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t
            {
                return readText(io->_deviceName, MAX_ELEM_OBJECT_NAME, start, count, data);
            }),
#endif
#if MASK_VERSION != 0x2920
        // 06_02_42 mask 2920h 2.5.2.1 note 10: PID_FIRMWARE_REVISION is not allowed there, PID_VERSION carries it.
        // A read-only Property must be a CallbackProperty without a write callback: WriteEnable() is only
        // evaluated on the cEMI path, so a DataProperty stays writeable over A_PropertyValue_Write.
        new CallbackProperty<DeviceObject>(this, PID_FIRMWARE_REVISION, false, PDT_UNSIGNED_CHAR, 1, ReadLv3 | WriteLv0,
            [](DeviceObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t
            {
                if (start == 0)
                {
                    pushWord(1, data);
                    return 1;
                }

                *data = io->_firmwareRevision;
                return 1;
            }),
#endif
        new DataProperty(PID_SERIAL_NUMBER, false, PDT_GENERIC_06, 1, ReadLv3 | WriteLv0, serialNumber), 
        new CallbackProperty<DeviceObject>(this, PID_MANUFACTURER_ID, false, PDT_UNSIGNED_INT, 1, ReadLv3 | WriteLv0,
            [](DeviceObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t 
            { 
                if(start == 0)
                {
                    uint16_t currentNoOfElements = 1;
                    pushWord(currentNoOfElements, data);
                    return 1;
                }
                
                pushByteArray(io->propertyData(PID_SERIAL_NUMBER), 2, data);
                return 1;
            }),
        new DataProperty(PID_DEVICE_CONTROL, true, PDT_BITSET8, 1, ReadLv3 | WriteLv3, (uint8_t)0),
        new DataProperty(PID_ORDER_INFO, false, PDT_GENERIC_10, 1, ReadLv3 | WriteLv0),
#if MASK_VERSION != 0x57B0
        // 03_05_01 4.2.16: devices without a PEI report zero. Profiles A.2.3 asks for (3/3) on mask
        // 57B0h - writeable where present - so it is left out there instead of shipping it read-only.
        new CallbackProperty<DeviceObject>(this, PID_PEI_TYPE, false, PDT_UNSIGNED_CHAR, 1, ReadLv3 | WriteLv0,
            [](DeviceObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t
            {
                if (start == 0)
                {
                    pushWord(1, data);
                    return 1;
                }

                *data = 0;
                return 1;
            }),
#endif
#if MASK_VERSION != 0x091A && MASK_VERSION != 0x2920
        // 03_05_01 4.2.19: manufacturer specific device information, here the hardware identifier.
        // Profiles A.3.2 asks for (3/0) on mask 091Ah and 3/1 on 2920h - both writeable - so it is left
        // out on the coupler masks instead of shipping it read-only.
        new CallbackProperty<DeviceObject>(this, PID_MANUFACTURER_DATA, false, PDT_UNSIGNED_CHAR, MAX_ELEM_MANUFACTURER_DATA, ReadLv3 | WriteLv0,
            [](DeviceObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t
            {
                return readText(io->_hardwareId, MAX_ELEM_MANUFACTURER_DATA, start, count, data);
            }),
#endif
        new DataProperty(PID_VERSION, false, PDT_VERSION, 1, ReadLv3 | WriteLv0, (uint16_t)3),
        new DataProperty(PID_ROUTING_COUNT, true, PDT_UNSIGNED_CHAR, 1, ReadLv3 | WriteLv3, (uint8_t)(6 << 4)),
#if MASK_VERSION != 0x57B0
        // 03_05_01 4.3.3: bit 6-4 busy_retry, bit 2-0 nak_retry. Not persisted, so a reset restores 33h.
        // Profiles A.2.3 marks this Property X for mask 57B0h, where it must not exist as a Data Property.
        new CallbackProperty<DeviceObject>(this, PID_MAX_RETRY_COUNT, true, PDT_GENERIC_01, 1, ReadLv3 | WriteLv3,
            [](DeviceObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t
            {
                if (start == 0)
                {
                    pushWord(1, data);
                    return 1;
                }

                *data = io->_maxRetryCount;
                return 1;
            },
            [](DeviceObject* io, uint16_t start, uint8_t count, const uint8_t* data) -> uint8_t
            {
                if (start == 0)
                    return 0;

                io->_maxRetryCount = *data & 0x77; // 03_05_01 4.3.3: bit 7 and bit 3 are reserved and shall be 0
                return 1;
            }),
#endif
        new CallbackProperty<DeviceObject>(this, PID_PROG_MODE, true, PDT_BITSET8, 1, ReadLv3 | WriteLv3, 
            [](DeviceObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t 
            { 
                if(start == 0)
                {
                    uint16_t currentNoOfElements = 1;
                    pushWord(currentNoOfElements, data);
                    return 1;
                }

                *data = io->_prgMode;
                return 1;
            },
            [](DeviceObject* io, uint16_t start, uint8_t count, const uint8_t* data) -> uint8_t 
            { 
                if(start == 0)
                    return 1;

                // PDT_BITSET8: only bit 0 is the programming mode, and progMode() tests the stored byte
                // for exactly 1 -- storing the reserved bits made a write of 03h read back as "not in
                // programming mode" while the device reported 03h.
                io->_prgMode = *data & 0x01;
                return 1;
            }),
        // 03_05_01 4.3.6: 2 octets application manufacturer + 8 octets manufacturer specific device type
        new CallbackProperty<DeviceObject>(this, PID_PRODUCT_ID, false, PDT_GENERIC_10, 1, ReadLv3 | WriteLv0,
            [](DeviceObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t
            {
                // A board without DEVICE_HW_ID has nothing to identify here. Report no elements rather
                // than ten zero octets, which a reader cannot tell apart from a real value.
                if (start == 0)
                {
                    pushWord(io->_hasProductId ? 1 : 0, data);
                    return 1;
                }

                if (!io->_hasProductId) return 0;

                pushByteArray(io->_productId, LEN_PRODUCT_ID, data);
                return 1;
            }),
        new DataProperty(PID_MAX_APDU_LENGTH, false, PDT_UNSIGNED_INT, 1, ReadLv3 | WriteLv0, (uint16_t)254),
        new CallbackProperty<DeviceObject>(this, PID_SUBNET_ADDR, false, PDT_UNSIGNED_CHAR, 1, ReadLv3 | WriteLv0,
            [](DeviceObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t 
            { 
                if(start == 0)
                {
                    uint16_t currentNoOfElements = 1;
                    pushWord(currentNoOfElements, data);
                    return 1;
                }

                *data = ((io->_ownAddress >> 8) & 0xff);

                return 1;
            }),
        new CallbackProperty<DeviceObject>(this, PID_DEVICE_ADDR, false, PDT_UNSIGNED_CHAR, 1, ReadLv3 | WriteLv0,
            [](DeviceObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t 
            { 
                if(start == 0)
                {
                    uint16_t currentNoOfElements = 1;
                    pushWord(currentNoOfElements, data);
                    return 1;
                }

                *data = (io->_ownAddress & 0xff);
                return 1;
            }),
        new DataProperty(PID_IO_LIST, false, PDT_UNSIGNED_INT, KNX_MAX_INTERFACE_OBJECTS, ReadLv3 | WriteLv0),
        new DataProperty(PID_HARDWARE_TYPE, true, PDT_GENERIC_06, 1, ReadLv3 | WriteLv3, hardwareType),
        new DataProperty(PID_DEVICE_DESCRIPTOR, false, PDT_GENERIC_02, 1, ReadLv3 | WriteLv0),
        // 03_05_01 4.3.1: Properties above 201 are manufacturer specific, and other manufacturers use them too.
        // The first one names the owner, so a reader checks "OpenKNX" here before it interprets the others.
        new CallbackProperty<DeviceObject>(this, PID_MFR_STRUCT_INFO, false, PDT_GENERIC_09, 1, ReadLv3 | WriteLv0,
            [](DeviceObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t
            {
                if (start == 0)
                {
                    pushWord(1, data);
                    return 1;
                }

                if (start > 1) return 0; // one element; a read beyond it answers no data (03_03_07 3.4.4.1)

                uint8_t present = 0;
                for (uint8_t bit = 0; bit < 8; bit++)
                    if (io->property((PropertyID)(PID_MFR_STRUCT_INFO + bit)) != nullptr)
                        present |= (uint8_t)(1 << bit);

                memcpy(data, "OpenKNX", 7);
                data[7] = MFR_STRUCT_VERSION;
                data[8] = present;
                return 1;
            }),
        // Manufacturer specific: PID_DESCRIPTION (21) is the standard descriptive slot, but the profile
        // requires it to be writeable and there is no non-volatile store for a written value here.
        new CallbackProperty<DeviceObject>(this, PID_MFR_FIRMWARE_NAME, false, PDT_UNSIGNED_CHAR, MAX_ELEM_FIRMWARE_NAME, ReadLv3 | WriteLv0,
            [](DeviceObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t
            {
                return readText(io->_firmwareName, MAX_ELEM_FIRMWARE_NAME, start, count, data);
            }),
        // The manufacturer's device name, which PID_OBJECT_NAME only carries until a management client
        // writes its own. Read-only, so a written name never takes the manufacturer's away.
        new CallbackProperty<DeviceObject>(this, PID_MFR_DEVICE_NAME, false, PDT_UNSIGNED_CHAR, MAX_ELEM_OBJECT_NAME, ReadLv3 | WriteLv0,
            [](DeviceObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t
            {
                return readText(io->_deviceName, MAX_ELEM_OBJECT_NAME, start, count, data);
            }),
        // Only the build timestamp, because nothing else here is new: major and minor live in
        // PID_VERSION, and the revision in PID_FIRMWARE_REVISION - at full precision, while
        // PID_VERSION keeps just 5 bits of it. Seconds since the epoch, 0 = not set.
        new CallbackProperty<DeviceObject>(this, PID_MFR_BUILD_TIMESTAMP, false, PDT_UNSIGNED_LONG, 1, ReadLv3 | WriteLv0,
            [](DeviceObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t
            {
                if (start == 0)
                {
                    pushWord(1, data);
                    return 1;
                }

                pushInt(io->_buildTimestamp, data);
                return 1;
            }),
        // The build identity as text - the short git hash, which no number can carry.
        new CallbackProperty<DeviceObject>(this, PID_MFR_FIRMWARE_BUILD, false, PDT_UNSIGNED_CHAR, MAX_ELEM_FIRMWARE_BUILD, ReadLv3 | WriteLv0,
            [](DeviceObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t
            {
                return readText(io->_firmwareBuild, MAX_ELEM_FIRMWARE_BUILD, start, count, data);
            }),
        // The chip the firmware was built for. One application exists for several chips, and nothing
        // else in the Device Object tells them apart; a tool compares this with a firmware file.
        new CallbackProperty<DeviceObject>(this, PID_MFR_MCU, false, PDT_UNSIGNED_CHAR, MAX_ELEM_MCU, ReadLv3 | WriteLv0,
            [](DeviceObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t
            {
                return readText(io->_mcu, MAX_ELEM_MCU, start, count, data);
            }),
#ifdef USE_RF
        new DataProperty(PID_RF_DOMAIN_ADDRESS_CEMI_SERVER, true, PDT_GENERIC_06, 1, ReadLv3 | WriteLv3),
#endif
    };
    initializeProperties(sizeof(properties), properties);
}

uint8_t* DeviceObject::save(uint8_t* buffer)
{
    buffer = pushWord(_ownAddress, buffer);
    return InterfaceObject::save(buffer);
}

const uint8_t* DeviceObject::restore(const uint8_t* buffer)
{
    buffer = popWord(_ownAddress, buffer);
    buffer = InterfaceObject::restore(buffer);
    // 03_05_01 4.2.14.4 p.35: PID 14 is 00h on start-up; it is persisted only because it is writable.
    property(PID_DEVICE_CONTROL)->write((uint8_t)0);
    return buffer;
}

uint16_t DeviceObject::saveSize()
{
    return 2 + InterfaceObject::saveSize();
}

void DeviceObject::masterReset(EraseCode eraseCode, uint8_t channel)
{
    (void)channel;
    if (eraseCode == EraseCode::ResetIA || eraseCode == EraseCode::FactoryReset)
    {
#if MASK_VERSION == 0x091A || MASK_VERSION == 0x2920
        _ownAddress = 0xFF00; // coupler default 15.15.0
#else
        _ownAddress = 0xFFFF; // device default 15.15.255
#endif
    }
}

uint16_t DeviceObject::individualAddress()
{
    return _ownAddress;
}

void DeviceObject::individualAddress(uint16_t value)
{
    _ownAddress = value;
}

#define USER_STOPPED  0x1
#define OWN_ADDR_DUPL 0x2
#define VERIFY_MODE   0x4
#define SAFE_STATE    0x8


void DeviceObject::individualAddressDuplication(bool value)
{
    Property* prop = property(PID_DEVICE_CONTROL);
    // Property::read(uint8_t&) leaves the argument untouched when the property holds no elements.
    uint8_t data = 0;
    prop->read(data);
    
    if (value)
        data |= OWN_ADDR_DUPL;
    else
        data &= ~OWN_ADDR_DUPL;
    prop->write(data);
}

bool DeviceObject::verifyMode()
{
    Property* prop = property(PID_DEVICE_CONTROL);
    // As individualAddressDuplication(): an unread property would leave this uninitialised.
    uint8_t data = 0;
    prop->read(data);
    return (data & VERIFY_MODE) > 0;
}

void DeviceObject::verifyMode(bool value)
{
    Property* prop = property(PID_DEVICE_CONTROL);
    uint8_t data;
    prop->read(data);

    if (value)
        data |= VERIFY_MODE;
    else
        data &= ~VERIFY_MODE;
    prop->write(data);
}

// 03_05_01 4.2.14.8.4: Safe State is not implemented, so bit 3 of PID 14 stays 0 after a write.
void DeviceObject::writeProperty(PropertyID id, uint16_t start, uint8_t* data, uint8_t& count)
{
    InterfaceObject::writeProperty(id, start, data, count);
    if (id == PID_DEVICE_CONTROL && count > 0)
        safeState(false);
}

void DeviceObject::safeState(bool value)
{
    Property* prop = property(PID_DEVICE_CONTROL);
    uint8_t data = 0;
    prop->read(data);

    if (value)
        data |= SAFE_STATE;
    else
        data &= ~SAFE_STATE;
    prop->write(data);
}

bool DeviceObject::progMode()
{
    return _prgMode == 1;
}

void DeviceObject::progMode(bool value)
{
    if (value)
        _prgMode = 1;
    else
        _prgMode = 0;
}

uint16_t DeviceObject::manufacturerId()
{
    uint16_t manufacturerId;
    popWord(manufacturerId, propertyData(PID_SERIAL_NUMBER));
    return manufacturerId;
}

void DeviceObject::manufacturerId(uint16_t value)
{
    uint8_t data[LEN_KNX_SERIAL];
    memcpy(data, propertyData(PID_SERIAL_NUMBER), LEN_KNX_SERIAL);
    pushWord(value, data);
    propertyValue(PID_SERIAL_NUMBER, data);
}

uint32_t DeviceObject::bauNumber()
{
    uint32_t bauNumber;
    popInt(bauNumber, propertyData(PID_SERIAL_NUMBER) + 2);
    return bauNumber;
}

void DeviceObject::bauNumber(uint32_t value)
{
    uint8_t data[LEN_KNX_SERIAL];
    memcpy(data, propertyData(PID_SERIAL_NUMBER), LEN_KNX_SERIAL);
    pushInt(value, data + 2);
    propertyValue(PID_SERIAL_NUMBER, data);
}

const uint8_t* DeviceObject::orderNumber()
{
    DataProperty* prop = (DataProperty*)property(PID_ORDER_INFO);
    return prop->data();
}

void DeviceObject::orderNumber(const uint8_t* value)
{
    Property* prop = property(PID_ORDER_INFO);
    // PDT_GENERIC_10: Property::write copies exactly 10 octets and callers pass shorter string literals.
    uint8_t padded[LEN_ORDER_INFO] = {0};
    size_t len = 0;
    while (len < sizeof(padded) && value[len] != 0)
        len++;
    memcpy(padded, value, len);
    prop->write(padded);
}

const uint8_t* DeviceObject::productId()
{
    return _productId;
}

void DeviceObject::productId(const uint8_t* value)
{
    memcpy(_productId, value, LEN_PRODUCT_ID);
    _hasProductId = true;
}

uint8_t DeviceObject::firmwareRevision()
{
    return _firmwareRevision;
}

void DeviceObject::firmwareRevision(uint8_t value)
{
    _firmwareRevision = value;
}

uint8_t DeviceObject::maxRetryCount()
{
    return _maxRetryCount;
}

void DeviceObject::deviceName(const char* utf8Value)
{
    _deviceName = utf8Value;
}

void DeviceObject::hardwareId(const char* value)
{
    _hardwareId = value;
}

void DeviceObject::firmwareName(const char* value)
{
    _firmwareName = value;
}

void DeviceObject::firmwareBuild(const char* value)
{
    _firmwareBuild = value;
}

void DeviceObject::mcu(const char* value)
{
    _mcu = value;
}

void DeviceObject::buildTimestamp(uint32_t value)
{
    _buildTimestamp = value;
}

uint32_t DeviceObject::buildTimestamp()
{
    return _buildTimestamp;
}

const uint8_t* DeviceObject::hardwareType()
{
    DataProperty* prop = (DataProperty*)property(PID_HARDWARE_TYPE);
    return prop->data();
}

void DeviceObject::hardwareType(const uint8_t* value)
{
    Property* prop = property(PID_HARDWARE_TYPE);
    prop->write(value);
}

uint16_t DeviceObject::version()
{
    Property* prop = property(PID_VERSION);
    uint16_t value;
    prop->read(value);
    return value;
}

void DeviceObject::version(uint16_t value)
{
    Property* prop = property(PID_VERSION);
    prop->write(value);
}

uint16_t DeviceObject::maskVersion()
{
    Property* prop = property(PID_DEVICE_DESCRIPTOR);
    uint16_t value;
    prop->read(value);
    return value;
}

void DeviceObject::maskVersion(uint16_t value)
{
    Property* prop = property(PID_DEVICE_DESCRIPTOR);
    prop->write(value);
}

uint16_t DeviceObject::maxApduLength()
{
    Property* prop = property(PID_MAX_APDU_LENGTH);
    uint16_t value;
    prop->read(value);
    return value;
}

void DeviceObject::maxApduLength(uint16_t value)
{
    Property* prop = property(PID_MAX_APDU_LENGTH);
    prop->write(value);
}

#ifdef USE_RF
// Defined only under USE_RF, as the property is registered: a caller without RF fails to link instead of reading null.
const uint8_t* DeviceObject::rfDomainAddress()
{
    DataProperty* prop = (DataProperty*)property(PID_RF_DOMAIN_ADDRESS_CEMI_SERVER);
    return prop->data();
}

void DeviceObject::rfDomainAddress(uint8_t* value)
{
    Property* prop = property(PID_RF_DOMAIN_ADDRESS_CEMI_SERVER);
    prop->write(value);
}
#endif

uint8_t DeviceObject::defaultHopCount()
{
    Property* prop = property(PID_ROUTING_COUNT);
    uint8_t value;
    prop->read(value);
    return (value >> 4) & 0x07;
}

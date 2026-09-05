#include "ip_parameter_object.h"
#ifdef USE_IP
#include "device_object.h"
#include "platform.h"
#include "bits.h"
#include "data_property.h"
#include "callback_property.h"

// 224.0.23.12
#define DEFAULT_MULTICAST_ADDR ((uint32_t)0xE000170C)

IpParameterObject::IpParameterObject(DeviceObject& deviceObject, Platform& platform, KnxIpCounters* counters): _deviceObject(deviceObject),
    _platform(platform), _counters(counters)
{
    Property* properties[] =
    {
        new DataProperty(PID_OBJECT_TYPE, false, PDT_UNSIGNED_INT, 1, ReadLv3 | WriteLv0, (uint16_t)OT_IP_PARAMETER),
        new DataProperty(PID_PROJECT_INSTALLATION_ID, true, PDT_UNSIGNED_INT, 1, ReadLv3 | WriteLv3),
        new CallbackProperty<IpParameterObject>(this, PID_KNX_INDIVIDUAL_ADDRESS, true, PDT_UNSIGNED_INT, 1, ReadLv3 | WriteLv3,
            [](IpParameterObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t 
            {
                if(start == 0)
                {
                    uint16_t currentNoOfElements = 1;
                    pushWord(currentNoOfElements, data);
                    return 1;
                }
                // TODO: get property of deviceobject and use it
                pushWord(io->_deviceObject.individualAddress(), data);
                return 1;
            },
            [](IpParameterObject* io, uint16_t start, uint8_t count, const uint8_t* data) -> uint8_t 
            { 
                io->_deviceObject.individualAddress(getWord(data));
                return 1; 
            }),
#ifdef KNX_TUNNELING
        new DataProperty(PID_ADDITIONAL_INDIVIDUAL_ADDRESSES, true, PDT_UNSIGNED_INT, KNX_TUNNELING, ReadLv3 | WriteLv3),
        new DataProperty(PID_CUSTOM_RESERVED_TUNNELS_CTRL, true, PDT_UNSIGNED_CHAR, KNX_TUNNELING, ReadLv3 | WriteLv3), // custom propertiy to control the stacks behaviour for reserverd tunnels, not in Spec (PID >= 200)
        new DataProperty(PID_CUSTOM_RESERVED_TUNNELS_IP, true, PDT_UNSIGNED_LONG, KNX_TUNNELING, ReadLv3 | WriteLv3), // custom propertiy to control the stacks behaviour for reserverd tunnels, not in Spec (PID >= 200)
#endif
        new DataProperty(PID_CURRENT_IP_ASSIGNMENT_METHOD, false, PDT_UNSIGNED_CHAR, 1, ReadLv3 | WriteLv3), // maxElements 1: must hold 1 octet so the resolved method (03_08_03 2.5.5) can be stored; 0 rejected every write
        new DataProperty(PID_IP_ASSIGNMENT_METHOD, true, PDT_UNSIGNED_CHAR, 1, ReadLv3 | WriteLv3),
        new DataProperty(PID_IP_CAPABILITIES, true, PDT_BITSET8, 1, ReadLv3 | WriteLv1),    // maxElements 1: must hold 1 octet so the application can store the IP capabilities (0 rejected every write)
        new CallbackProperty<IpParameterObject>(this, PID_CURRENT_IP_ADDRESS, false, PDT_UNSIGNED_LONG, 1, ReadLv3 | WriteLv0,
            [](IpParameterObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t 
            { 
                if(start == 0)
                {
                    uint16_t currentNoOfElements = 1;
                    pushWord(currentNoOfElements, data);
                    return 1;
                }

                pushInt(htonl(io->_platform.currentIpAddress()), data);
                return 1;
            }),
        new CallbackProperty<IpParameterObject>(this, PID_CURRENT_SUBNET_MASK, false, PDT_UNSIGNED_LONG, 1, ReadLv3 | WriteLv0,
            [](IpParameterObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t 
            { 
                if(start == 0)
                {
                    uint16_t currentNoOfElements = 1;
                    pushWord(currentNoOfElements, data);
                    return 1;
                }

                pushInt(htonl(io->_platform.currentSubnetMask()), data);
                return 1;
            }),
        new CallbackProperty<IpParameterObject>(this, PID_CURRENT_DEFAULT_GATEWAY, false, PDT_UNSIGNED_LONG, 1, ReadLv3 | WriteLv0,
            [](IpParameterObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t 
            { 
                if(start == 0)
                {
                    uint16_t currentNoOfElements = 1;
                    pushWord(currentNoOfElements, data);
                    return 1;
                }

                pushInt(htonl(io->_platform.currentDefaultGateway()), data);
                return 1;
            }),
        new DataProperty(PID_IP_ADDRESS, true, PDT_UNSIGNED_LONG, 1, ReadLv3 | WriteLv3),
        new DataProperty(PID_SUBNET_MASK, true, PDT_UNSIGNED_LONG, 1, ReadLv3 | WriteLv3),
        new DataProperty(PID_DEFAULT_GATEWAY, true, PDT_UNSIGNED_LONG, 1, ReadLv3 | WriteLv3),
        new CallbackProperty<IpParameterObject>(this, PID_MAC_ADDRESS, false, PDT_GENERIC_06, 1, ReadLv3 | WriteLv0,
            [](IpParameterObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t 
            { 
                if(start == 0)
                {
                    uint16_t currentNoOfElements = 1;
                    pushWord(currentNoOfElements, data);
                    return 1;
                }

                io->_platform.macAddress(data);
                return 1;
            }),
        new CallbackProperty<IpParameterObject>(this, PID_SYSTEM_SETUP_MULTICAST_ADDRESS, false, PDT_UNSIGNED_LONG, 1, ReadLv3 | WriteLv0,
            [](IpParameterObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t 
            { 
                if(start == 0)
                {
                    uint16_t currentNoOfElements = 1;
                    pushWord(currentNoOfElements, data);
                    return 1;
                }

                pushInt(DEFAULT_MULTICAST_ADDR, data);
                return 1;
            }),
#ifdef KNX_IS_ROUTER
        new DataProperty(PID_ROUTING_MULTICAST_ADDRESS, true, PDT_UNSIGNED_LONG, 1, ReadLv3 | WriteLv3, DEFAULT_MULTICAST_ADDR),
#else
        // Non-routing device: PID_ROUTING_MULTICAST_ADDRESS always reads 0.0.0.0 (03_08_02 §7.5.4.2).
        // Callback (read 0, write accepted+ignored) so a persisted value can't surface a non-zero routing
        // multicast. It must stay a CallbackProperty and must NOT gain save/restore: InterfaceObject walks
        // the WriteEnable properties into one packed stream with no per-object length, the table objects are
        // restored from the same cursor, and the version check compares only the ETS application -- so any
        // change to the number of bytes this property persists misaligns every device already in the field.
        new CallbackProperty<IpParameterObject>(this, PID_ROUTING_MULTICAST_ADDRESS, true, PDT_UNSIGNED_LONG, 1, ReadLv3 | WriteLv3,
            [](IpParameterObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t
            {
                if (start == 0)
                {
                    uint16_t currentNoOfElements = 1;
                    pushWord(currentNoOfElements, data);
                    return 1;
                }
                pushInt((uint32_t)0, data); // non-routing device: 0.0.0.0
                return 1;
            },
            [](IpParameterObject* io, uint16_t start, uint8_t count, const uint8_t* data) -> uint8_t
            {
                return 1; // accept a write (ETS/tools may set it) but ignore it -> the read stays 0.0.0.0
            }),
#endif
#ifdef KNX_IS_ROUTER
        // 03_08_03 2.5.23-2.5.26: telegram counters of a KNXnet/IP routing device. Read-only, they
        // never wrap, and they read 0 while no counter object is wired in.
        new CallbackProperty<IpParameterObject>(this, PID_QUEUE_OVERFLOW_TO_IP, false, PDT_UNSIGNED_INT, 1, ReadLv3 | WriteLv0,
            [](IpParameterObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t
            {
                if (start == 0) { pushWord(1, data); return 1; }
                pushWord(io->_counters ? io->_counters->overflowToIp() : 0, data);
                return 1;
            }),
        new CallbackProperty<IpParameterObject>(this, PID_QUEUE_OVERFLOW_TO_KNX, false, PDT_UNSIGNED_INT, 1, ReadLv3 | WriteLv0,
            [](IpParameterObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t
            {
                if (start == 0) { pushWord(1, data); return 1; }
                pushWord(io->_counters ? io->_counters->overflowToKnx() : 0, data);
                return 1;
            }),
        new CallbackProperty<IpParameterObject>(this, PID_MSG_TRANSMIT_TO_IP, false, PDT_UNSIGNED_LONG, 1, ReadLv3 | WriteLv0,
            [](IpParameterObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t
            {
                if (start == 0) { pushWord(1, data); return 1; }
                pushInt(io->_counters ? io->_counters->transmitToIp() : 0, data);
                return 1;
            }),
        new CallbackProperty<IpParameterObject>(this, PID_MSG_TRANSMIT_TO_KNX, false, PDT_UNSIGNED_LONG, 1, ReadLv3 | WriteLv0,
            [](IpParameterObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t
            {
                if (start == 0) { pushWord(1, data); return 1; }
                pushInt(io->_counters ? io->_counters->transmitToKnx() : 0, data);
                return 1;
            }),
#endif
        new DataProperty(PID_TTL, true, PDT_UNSIGNED_CHAR, 1, ReadLv3 | WriteLv3, (uint8_t)16),
        new CallbackProperty<IpParameterObject>(this, PID_KNXNETIP_DEVICE_CAPABILITIES, false, PDT_BITSET16, 1, ReadLv3 | WriteLv0,
            [](IpParameterObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t 
            { 
                if(start == 0)
                {
                    uint16_t currentNoOfElements = 1;
                    pushWord(currentNoOfElements, data);
                    return 1;
                }

                pushWord(0x1, data);
                return 1;
            }),
        new DataProperty(PID_FRIENDLY_NAME, true, PDT_UNSIGNED_CHAR, 30, ReadLv3 | WriteLv3)
    };
    initializeProperties(sizeof(properties), properties);
}

// Zero every element of a property via the tested write path (ETS uses the same write to set them).
static void clearProperty(Property* p)
{
    if (p == nullptr)
        return;
    const uint16_t max = p->MaxElements();
    const uint8_t elemSize = p->ElementSize();
    if (max == 0 || elemSize == 0 || elemSize > 4)
        return;
    uint8_t zero[4] = {0, 0, 0, 0};
    for (uint16_t i = 1; i <= max; i++)
        p->write(i, (uint8_t)1, zero);
}

void IpParameterObject::masterReset(EraseCode eraseCode, uint8_t channel)
{
    (void)channel;
    if (eraseCode != EraseCode::FactoryReset && eraseCode != EraseCode::FactoryResetWithoutIA)
        return;

    // Clear the tunnelling identities and the downloaded IP address config back to unassigned. Assignment
    // method, capabilities, multicast and TTL keep their defaults (some reject a 0 write), and the IP stack
    // re-resolves; ETS re-writes everything on the next download. Zeroing unused NV memory
    // (03_05_02 3.7.1.2.3.2.2) belongs to the Data Security track -- no keys are stored while it is off.
    clearProperty(property(PID_PROJECT_INSTALLATION_ID));
    clearProperty(property(PID_IP_ADDRESS));
    clearProperty(property(PID_SUBNET_MASK));
    clearProperty(property(PID_DEFAULT_GATEWAY));
    clearProperty(property(PID_FRIENDLY_NAME));
#ifdef KNX_TUNNELING
    clearProperty(property(PID_ADDITIONAL_INDIVIDUAL_ADDRESSES));
    clearProperty(property(PID_CUSTOM_RESERVED_TUNNELS_CTRL));
    clearProperty(property(PID_CUSTOM_RESERVED_TUNNELS_IP));
#endif
}

#endif

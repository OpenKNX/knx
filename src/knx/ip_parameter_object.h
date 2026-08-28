#pragma once

#include "config.h"
#ifdef USE_IP
#include "interface_object.h"
#include "device_object.h"
#include "platform.h"
#include "knx_ip_counters.h"

#define KNXIP_MULTICAST_PORT 3671

class IpParameterObject : public InterfaceObject
{
  public:
    IpParameterObject(DeviceObject& deviceObject, Platform& platform, KnxIpCounters* counters = nullptr);
    // Master Reset (03_05_02 3.7.1.2 / Table 6): a factory reset clears the tunnelling identities and the
    // downloaded IP configuration; assignment method / capabilities / multicast keep their defaults and
    // the IP stack re-resolves. ETS re-writes the object on the next download.
    void masterReset(EraseCode eraseCode, uint8_t channel) override;

  private:
    DeviceObject& _deviceObject;
    Platform& _platform;
    KnxIpCounters* _counters;
};
#endif
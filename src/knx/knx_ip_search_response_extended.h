#pragma once

#include "knx_ip_frame.h"
#include "ip_host_protocol_address_information.h"
#include "knx_ip_device_information_dib.h"
#include "knx_ip_extended_device_information_dib.h"
#include "knx_ip_supported_service_dib.h"
#include "ip_parameter_object.h"
#ifdef USE_IP

class KnxIpSearchResponseExtended : public KnxIpFrame
{
  public:
    KnxIpSearchResponseExtended(IpParameterObject& parameters, DeviceObject& deviceObj, int dibLength);
    IpHostProtocolAddressInformation& controlEndpoint();
    void setDeviceInfo(IpParameterObject& parameters, DeviceObject& deviceObject);
    void setSupportedServices();
    //setIpConfig
    //setIpCurrentConfig
    //setAddresses
    //setManuData
    //setTunnelingInfo
    void setExtendedDeviceInfo();
    uint8_t *DIBs();
  private:
    IpHostProtocolAddressInformation _controlEndpoint;
    int currentPos = 0;
};

#endif
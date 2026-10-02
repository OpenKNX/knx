#pragma once

#include "service_families.h"
#if KNX_SERVICE_FAMILY_CORE >= 2

#include "knx_ip_frame.h"
#include "cemi_frame.h"   // MAX_APDU_OCTET_COUNT, used as the default below
#include "ip_host_protocol_address_information.h"
#include "knx_ip_device_information_dib.h"
#include "knx_ip_extended_device_information_dib.h"
#include "knx_ip_supported_service_dib.h"
#include "knx_ip_config_dib.h"
#include "knx_ip_knx_addresses_dib.h"
#include "knx_ip_tunneling_info_dib.h"
#include "ip_parameter_object.h"
#include "knx_ip_tunnel_connection.h"
#ifdef USE_IP

class KnxIpSearchResponseExtended : public KnxIpFrame
{
  public:
    KnxIpSearchResponseExtended(IpParameterObject& parameters, DeviceObject& deviceObj, int dibLength);
    IpHostProtocolAddressInformation& controlEndpoint();
    void setDeviceInfo(IpParameterObject& parameters, DeviceObject& deviceObject);
    void setSupportedServices();
    void setIpConfig(IpParameterObject& parameters);
    void setIpCurrentConfig(IpParameterObject& parameters);
    void setKnxAddresses(IpParameterObject& parameters, DeviceObject& deviceObject);
    //setManuData
    void setTunnelingInfo(IpParameterObject& parameters, DeviceObject& deviceObject, KnxIpTunnelConnection tunnels[], uint16_t busApduLength = MAX_APDU_OCTET_COUNT);
    void setExtendedDeviceInfo(uint16_t localApduLength = MAX_APDU_OCTET_COUNT);
    uint8_t *DIBs();
  private:
    IpHostProtocolAddressInformation _controlEndpoint;
    int currentPos = 0;
};

#endif
#endif
#pragma once

#include "knx_ip_frame.h"
#include "ip_host_protocol_address_information.h"
#include "knx_ip_device_information_dib.h"
#include "knx_ip_supported_service_dib.h"
#include "knx_ip_config_dib.h"
#include "knx_ip_extended_device_information_dib.h"
#include "ip_parameter_object.h"
#ifdef USE_IP

class KnxIpDescriptionResponse : public KnxIpFrame
{
  public:
    KnxIpDescriptionResponse(IpParameterObject& parameters, DeviceObject& deviceObj);
    KnxIpDeviceInformationDIB& deviceInfo();
    KnxIpSupportedServiceDIB& supportedServices();
  private:
    KnxIpDeviceInformationDIB _deviceInfo;
    KnxIpSupportedServiceDIB _supportedServices;
    KnxIpConfigDIB _ipCurrentConfig;                       // IP_CUR_CONFIG (0x04): current IP/subnet/gateway
    // EXTENDED_DEVICE_INFO (0x08) DISABLED: not allowed in DESCRIPTION_RESPONSE (03_08_02 Core Table 5); only
    // permitted in SEARCH_RESPONSE_EXTENDED. Member kept commented so the intent/wiring is not lost.
    // KnxIpExtendedDeviceInformationDIB _extendedDeviceInfo;
};

#endif
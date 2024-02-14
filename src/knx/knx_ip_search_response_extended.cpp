#include "knx_ip_search_response_extended.h"
#ifdef USE_IP

#define LEN_SERVICE_FAMILIES 2
#if MASK_VERSION == 0x091A
#ifdef KNX_TUNNELING
#define LEN_SERVICE_DIB (2 + 4 * LEN_SERVICE_FAMILIES)
#else
#define LEN_SERVICE_DIB (2 + 3 * LEN_SERVICE_FAMILIES)
#endif
#else
#ifdef KNX_TUNNELING
#define LEN_SERVICE_DIB (2 + 3 * LEN_SERVICE_FAMILIES)
#else
#define LEN_SERVICE_DIB (2 + 2 * LEN_SERVICE_FAMILIES)
#endif
#endif

KnxIpSearchResponseExtended::KnxIpSearchResponseExtended(IpParameterObject& parameters, DeviceObject& deviceObject, int dibLength)
    : KnxIpFrame(LEN_KNXIP_HEADER + LEN_IPHPAI + dibLength),
      _controlEndpoint(_data + LEN_KNXIP_HEADER)
{
    serviceTypeIdentifier(SearchResponseExt);

    _controlEndpoint.length(LEN_IPHPAI);
    _controlEndpoint.code(IPV4_UDP);
    _controlEndpoint.ipAddress(parameters.propertyValue<uint32_t>(PID_CURRENT_IP_ADDRESS));
    _controlEndpoint.ipPortNumber(KNXIP_MULTICAST_PORT);

    currentPos = LEN_KNXIP_HEADER + LEN_IPHPAI;
}

void KnxIpSearchResponseExtended::setDeviceInfo(IpParameterObject& parameters, DeviceObject& deviceObject)
{
    KnxIpDeviceInformationDIB _deviceInfo(_data + currentPos);
    _deviceInfo.length(LEN_DEVICE_INFORMATION_DIB);
    _deviceInfo.code(DEVICE_INFO);
#if MASK_VERSION == 0x57B0
    _deviceInfo.medium(0x20); //MediumType is IP (for IP-Only Devices)
#else
    _deviceInfo.medium(0x02); //MediumType is TP
#endif
    _deviceInfo.status(deviceObject.progMode());
    _deviceInfo.individualAddress(parameters.propertyValue<uint16_t>(PID_KNX_INDIVIDUAL_ADDRESS));
    _deviceInfo.projectInstallationIdentifier(parameters.propertyValue<uint16_t>(PID_PROJECT_INSTALLATION_ID));
    _deviceInfo.serialNumber(deviceObject.propertyData(PID_SERIAL_NUMBER));
    _deviceInfo.routingMulticastAddress(parameters.propertyValue<uint32_t>(PID_ROUTING_MULTICAST_ADDRESS));
    //_deviceInfo.routingMulticastAddress(0);

    uint8_t mac_address[LEN_MAC_ADDRESS] = {0};
    Property* prop = parameters.property(PID_MAC_ADDRESS);
    prop->read(mac_address);
    _deviceInfo.macAddress(mac_address);
    
    uint8_t friendlyName[LEN_FRIENDLY_NAME] = {0};
    prop = parameters.property(PID_FRIENDLY_NAME);
    prop->read(1, LEN_FRIENDLY_NAME, friendlyName);
    _deviceInfo.friendlyName(friendlyName);

    currentPos += LEN_DEVICE_INFORMATION_DIB;
}

void KnxIpSearchResponseExtended::setSupportedServices()
{
    KnxIpSupportedServiceDIB _supportedServices(_data + currentPos);
    _supportedServices.length(LEN_SERVICE_DIB);
    _supportedServices.code(SUPP_SVC_FAMILIES);
    _supportedServices.serviceVersion(Core, 2);
    _supportedServices.serviceVersion(DeviceManagement, 1);
#ifdef KNX_TUNNELING
    _supportedServices.serviceVersion(Tunnelling, 1);
#endif
#if MASK_VERSION == 0x091A
    _supportedServices.serviceVersion(Routing, 1);
#endif
    currentPos += LEN_SERVICE_DIB;
}

void KnxIpSearchResponseExtended::setExtendedDeviceInfo()
{
    KnxIpExtendedDeviceInformationDIB _extended(_data + currentPos);
    _extended.length(LEN_EXTENDED_DEVICE_INFORMATION_DIB);
    _extended.code(EXTENDED_DEVICE_INFO);
    _extended.status(0x01); //FIXME dont know encoding PID_MEDIUM_STATUS=51 RouterObject
    _extended.localMaxApdu(254); //FIXME is this correct?
    _extended.deviceDescriptor(MASK_VERSION);

    currentPos += LEN_EXTENDED_DEVICE_INFORMATION_DIB;
}

IpHostProtocolAddressInformation& KnxIpSearchResponseExtended::controlEndpoint()
{
    return _controlEndpoint;
}


uint8_t *KnxIpSearchResponseExtended::DIBs()
{
    return _data + LEN_KNXIP_HEADER + LEN_IPHPAI;
}
#endif

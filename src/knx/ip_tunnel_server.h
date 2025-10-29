#pragma once

#include "config.h"
#ifdef KNX_TUNNELING

#include <stdint.h>
#include "knx_types.h"
#include "knx_ip_tunnel_connection.h"
#include "cemi_frame.h"
#include "ip_parameter_object.h"

class CemiServer;

class IpTunnelServer
{
  public:
    /**
     * The constructor.
     * @param bau methods are called here depending of the content of the APDU
     */
    IpTunnelServer(DeviceObject& devObj, IpParameterObject& ipParam, Platform& platform, CemiServer& cemiServer);

    void loop();
    void dataRequestToTunnel(CemiFrame& frame);
    void dataConfirmationToTunnel(CemiFrame& frame);
    void dataIndicationToTunnel(CemiFrame& frame);
    bool isTunnelAddress(uint16_t addr);
    bool isSentToTunnel(uint16_t address, bool isGrpAddr);
    bool HandleIpFrame(uint8_t* buffer, uint16_t length, uint32_t& src_addr, uint16_t& src_port);

  private:

    void sendFrameToTunnel(KnxIpTunnelConnection *tunnel, CemiFrame& frame);
    void HandleConnectRequest(uint8_t* buffer, uint16_t length, uint32_t& src_addr, uint16_t& src_port);
    void HandleConnectionStateRequest(uint8_t* buffer, uint16_t length);
    void HandleDisconnectRequest(uint8_t* buffer, uint16_t length);
    void HandleDescriptionRequest(uint8_t* buffer, uint16_t length);
    void HandleDeviceConfigurationRequest(uint8_t* buffer, uint16_t length);
    void HandleTunnelingRequest(uint8_t* buffer, uint16_t length);


    KnxIpTunnelConnection tunnels[KNX_TUNNELING];
    uint8_t _lastChannelId = 0;
    IpParameterObject& _ipParameters;
    DeviceObject& _deviceObject;
    Platform& _platform;
    CemiServer& _cemiServer;
};


#endif
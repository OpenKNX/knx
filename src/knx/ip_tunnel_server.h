#pragma once

#include "config.h"
#ifdef KNX_TUNNELING
#ifndef KNX_TUNNELING_DEVMGMT
#define KNX_TUNNELING_DEVMGMT 1
#endif

#include <stdint.h>
#include "knx_types.h"
#include "knx_ip_tunnel_connection.h"
#include "cemi_frame.h"
#include "ip_parameter_object.h"

class CemiServer;

#ifdef OPENKNX_HW_BUSMON
class KnxIpConnectRequest;

/**
 * @brief Bridge that lets the tunnel server drive the TP chip's HW busmonitor mode.
 * Keeps the IP layer decoupled from the concrete TP data link layer. Implemented by
 * TpUartDataLinkLayer; registered by the router BAU via setHwBusMonitorDll().
 */
class IHwBusMonitorDll
{
  public:
    virtual void hwBusMonEnter() = 0;     // U_BUSMON_REQ: chip goes passive, routing pauses
    virtual void hwBusMonExit() = 0;      // reset() -> BCU_CONNECTED (routing returns)
    virtual bool hwBusMonConnected() = 0; // chip back to normal operation?
};
#endif

class IpTunnelServer
{
  public:
    /**
     * The constructor.
     * @param bau methods are called here depending of the content of the APDU
     */
    IpTunnelServer(DeviceObject& devObj, IpParameterObject& ipParam, Platform& platform, CemiServer& cemiServer);

    void loop();
    void dataRequestToChannelId(CemiFrame& frame, uint8_t channelId);
    void dataRequestToTunnel(CemiFrame& frame);
    void dataConfirmationToTunnel(CemiFrame& frame);
    void dataIndicationToTunnel(CemiFrame& frame);
    bool isTunnelAddress(uint16_t addr);
    bool isSentToTunnel(uint16_t address, bool isGrpAddr);
    bool HandleIpFrame(uint8_t* buffer, uint16_t length, uint32_t& src_addr, uint16_t& src_port);

#ifdef OPENKNX_HW_BUSMON
    /** @brief Register the TP DLL bridge used to enter/leave HW busmonitor mode (router BAU only). */
    void setHwBusMonitorDll(IHwBusMonitorDll* dll) { _hwBusMon = dll; }
    /** @brief True while a KNX-Busmonitor tunnel is open (chip in HW monitor mode). */
    bool busMonitorActive() { return _busMonTunnel.ChannelId != 0; }
    /** @brief Forward one raw monitor-mode LPDU (incl. FCS) to the busmon tunnel as cEMI L_Busmon.ind. */
    void busMonitorFrame(uint8_t* lpdu, uint16_t len);
#endif

  private:

    void sendFrameToTunnel(KnxIpTunnelConnection *tunnel, CemiFrame& frame);
    void HandleConnectRequest(uint8_t* buffer, uint16_t length, uint32_t& src_addr, uint16_t& src_port);
    void HandleConnectionStateRequest(uint8_t* buffer, uint16_t length);
    void HandleDisconnectRequest(uint8_t* buffer, uint16_t length);
    void HandleDescriptionRequest(uint8_t* buffer, uint16_t length);
    void HandleDeviceConfigurationRequest(uint8_t* buffer, uint16_t length);
    void HandleTunnelingRequest(uint8_t* buffer, uint16_t length);


    KnxIpTunnelConnection tunnels[KNX_TUNNELING+KNX_TUNNELING_DEVMGMT];
    uint8_t _lastChannelId = 0;
    IpParameterObject& _ipParameters;
    DeviceObject& _deviceObject;
    Platform& _platform;
    CemiServer& _cemiServer;

#ifdef OPENKNX_HW_BUSMON
    void HandleBusMonitorConnect(KnxIpConnectRequest& connRequest, uint32_t src_addr, uint16_t src_port);
    void busMonitorTeardown();

    KnxIpTunnelConnection _busMonTunnel;      // dedicated busmon connection (kept out of the L_Data fan-out)
    IHwBusMonitorDll* _hwBusMon = nullptr;    // TP chip bridge (null on non-router BAUs)
    uint8_t _busMonSeq = 0;                   // rolling status/sequence nibble for L_Busmon.ind
    bool _busMonExitPending = false;          // exit-recovery poll running (non-blocking)
    uint32_t _busMonExitStart = 0;
#endif
};


#endif
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

    // Read-only tunnel introspection for diagnostics/UI (display widget, group objects).
    // "Data" tunnels only: the first KNX_TUNNELING slots; device-management connections are excluded.
    /** @brief Connectable data-tunnel count (spec/config maximum). */
    uint8_t tunnelMax() const { return KNX_TUNNELING; }
    /** @brief Currently open data tunnels (ChannelId != 0). */
    uint8_t tunnelCount() const;
    /** @brief Read-only i-th open data tunnel (0..tunnelCount()-1); nullptr if out of range. */
    const KnxIpTunnelConnection* tunnelAt(uint8_t index) const;

    // Tunnel connection type + how a session ended, for the active list / history.
    enum TunnelType : uint8_t { TUN_DATA = 0, TUN_CONFIG = 1, TUN_BUSMON = 2 };
    enum TunnelEndReason : uint8_t { END_ACTIVE = 0, END_CLOSED = 1, END_TIMEOUT = 2, END_BUSMON = 3 };
    // One tunnel session. Times are millis()-relative (uptime); the console converts start to an absolute
    // wall-clock time on the fly when the clock is valid, so it stays correct even if the clock arrives later.
    struct TunnelEvent
    {
        uint32_t ip = 0;
        uint16_t pa = 0;               // KNX individual address (0 for config/busmon)
        uint8_t type = TUN_DATA;       // TunnelType
        uint8_t reason = END_ACTIVE;   // TunnelEndReason (END_ACTIVE for the live list)
        unsigned long startMillis = 0; // millis() at connect
        unsigned long endMillis = 0;   // millis() at disconnect (0 while active)
    };
    /** @brief Snapshot of all currently open connections (data + config + busmon); returns count. */
    uint8_t activeTunnels(TunnelEvent* out, uint8_t maxOut) const;
    /** @brief Number of recorded finished sessions (up to the ring size). */
    uint8_t tunnelHistoryCount() const;
    /** @brief i-th finished session, index 0 = newest; nullptr if out of range. */
    const TunnelEvent* tunnelHistoryAt(uint8_t index) const;

#ifdef OPENKNX_HW_BUSMON
    /** @brief Register the TP DLL bridge used to enter/leave HW busmonitor mode (router BAU only). */
    void setHwBusMonitorDll(IHwBusMonitorDll* dll) { _hwBusMon = dll; }
    /** @brief True while a KNX-Busmonitor tunnel is open (chip in HW monitor mode). */
    bool busMonitorActive() { return _busMonTunnel.ChannelId != 0; }
    /** @brief Forward one raw monitor-mode LPDU (incl. FCS) to the busmon tunnel as cEMI L_Busmon.ind. */
    void busMonitorFrame(uint8_t* lpdu, uint16_t len, uint8_t status = 0);
#endif

  private:

    void sendFrameToTunnel(KnxIpTunnelConnection *tunnel, CemiFrame& frame);
#ifdef KNX_TUNNEL_RESEND
    void pumpTunnel(KnxIpTunnelConnection *t);                 // send the FIFO head if nothing is in flight
    void disconnectTunnel(KnxIpTunnelConnection *t, uint8_t reason); // server-initiated teardown + reap
    void handleTunnelAck(uint8_t *buffer, uint16_t length);    // pop the acked head + pump the next
#endif
    void HandleConnectRequest(uint8_t* buffer, uint16_t length, uint32_t& src_addr, uint16_t& src_port);
    void HandleConnectionStateRequest(uint8_t* buffer, uint16_t length);
    void HandleDisconnectRequest(uint8_t* buffer, uint16_t length);
    void HandleDescriptionRequest(uint8_t* buffer, uint16_t length);
    void HandleDeviceConfigurationRequest(uint8_t* buffer, uint16_t length);
    void HandleTunnelingRequest(uint8_t* buffer, uint16_t length);


    KnxIpTunnelConnection tunnels[KNX_TUNNELING+KNX_TUNNELING_DEVMGMT];
    uint8_t _lastChannelId = 0;

    // Rolling connect/disconnect history (newest overwrites oldest).
    // 32 = 16 tunnels each connecting + disconnecting once, so a full round is retained.
    static const uint8_t TUNNEL_HISTORY_SIZE = 32;
    TunnelEvent _history[TUNNEL_HISTORY_SIZE];
    uint8_t _historyHead = 0;  // next write slot
    uint8_t _historyCount = 0;
    void recordTunnelSession(uint32_t ip, uint16_t pa, uint8_t type, unsigned long startMillis, uint8_t reason);
    IpParameterObject& _ipParameters;
    DeviceObject& _deviceObject;
    Platform& _platform;
    CemiServer& _cemiServer;

#ifdef OPENKNX_HW_BUSMON
    void HandleBusMonitorConnect(KnxIpConnectRequest& connRequest, uint32_t src_addr, uint16_t src_port);
    void busMonitorTeardown(uint8_t reason = END_CLOSED);

    KnxIpTunnelConnection _busMonTunnel;      // dedicated busmon connection (kept out of the L_Data fan-out)
    IHwBusMonitorDll* _hwBusMon = nullptr;    // TP chip bridge (null on non-router BAUs)
    uint8_t _busMonSeq = 0;                   // rolling status/sequence nibble for L_Busmon.ind
    bool _busMonExitPending = false;          // exit-recovery poll running (non-blocking)
    uint32_t _busMonExitStart = 0;
#endif
};


#endif
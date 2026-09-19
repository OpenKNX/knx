#pragma once

#include "config.h"
#ifdef KNX_TUNNELING
// Defined in config.h. No fallback here on purpose: it sizes tunnels[], so a second definition that ever
// diverged would change sizeof(IpTunnelServer) between translation units.
#ifndef KNX_TUNNELING_DEVMGMT
    #error "KNX_TUNNELING_DEVMGMT must be defined (config.h, or by a NO_KNX_CONFIG build)"
#endif

#include <stdint.h>
#include <atomic>
#include "knx_types.h"
#include "knx_ip_tunnel_connection.h"
#include "knx_ip_counters.h"
#include "cemi_frame.h"
#include "ip_parameter_object.h"

class CemiServer;

#ifdef OPENKNX_HW_BUSMON
// KNX_BUSMON_CONNECTIONS is defined in config.h. Deliberately NO fallback here: the value sizes a member
// array, so a second definition that ever diverged would change sizeof(IpTunnelServer) between translation
// units. A build that bypasses config.h (NO_KNX_CONFIG) must supply it.
#ifndef KNX_BUSMON_CONNECTIONS
    #error "KNX_BUSMON_CONNECTIONS must be defined (config.h, or by a NO_KNX_CONFIG build)"
#endif

class KnxIpConnectRequest;

/**
 * @brief Bridge that lets the tunnel server drive the TP chip's HW busmonitor mode.
 * Keeps the IP layer decoupled from the concrete TP data link layer. Implemented by
 * TpUartDataLinkLayer; registered by the router BAU via setHwBusMonitorDll().
 */
class IHwBusMonitorDll
{
  public:
    virtual bool hwBusMonEnter() = 0;     // U_BUSMON_REQ: chip passive, routing paused; false = refused
    virtual bool hwBusMonExit() = 0;      // leave monitor mode -> BCU_CONNECTED; returns true if it actually reset (false if a local console busmon still owns the chip)
    virtual uint32_t hwBusMonResetIndCount() = 0; // U_Reset.ind counter: did the chip execute the reset?
    virtual bool hwBusMonRxDesynced() = 0;        // receiver lost sync -> a missing indication proves nothing
    virtual bool hwBusMonActive() = 0;    // chip currently in monitor mode (any owner: ETS tunnel or local `bcu mon`)
    virtual bool hwBusOperational() = 0;  // KNX bus actually usable (host<->chip link + bus voltage)?
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
    void dataConfirmationToTunnel(CemiFrame& frame);
    void dataIndicationToTunnel(CemiFrame& frame);
    bool isTunnelAddress(uint16_t addr);
    bool isSentToTunnel(uint16_t address, bool isGrpAddr);
    /**
     * @brief True for an individual address ETS configured as a tunnelling address, connected or not.
     * @details 03_08_04 2.2.2 p.7 asks the server to defend its additional individual addresses so an
     *          address-in-use check finds them occupied. Reads a cached copy of
     *          PID_ADDITIONAL_INDIVIDUAL_ADDRESSES: this runs on the TP acknowledge path, where a property
     *          lookup per received frame has no business being.
     */
    bool isConfiguredTunnelPa(uint16_t addr) const;
    /**
     * @brief True when a frame carrying this channel id really came from that channel's client.
     * @details A channel id is ONE octet and was the only thing matched, so any host on the LAN could
     *          sweep 255 values and act on a foreign session -- an injected M_Reset or DISCONNECT ends
     *          a running ETS download in about two seconds. 03_08_02 5.2 p.12 calls the channel "the
     *          data endpoint connection BETWEEN a client and a server", and the client announces that
     *          endpoint in its CONNECT_REQUEST, so the server has the address already. Only the IP is
     *          compared, never the port: a client may use different source ports for its control and
     *          data endpoints.
     */
    static bool fromTunnelPeer(const KnxIpTunnelConnection* tun, uint32_t src_addr);
    bool HandleIpFrame(uint8_t* buffer, uint16_t length, uint32_t& src_addr, uint16_t& src_port);

    // Read-only tunnel introspection for diagnostics/UI (display widget, group objects).
    // "Data" tunnels only: the first KNX_TUNNELING slots; device-management connections are excluded.
    /** @brief Uptime in seconds, wrap-free. Pair with TunnelEvent::startS to get "how long ago". */
    uint32_t uptimeS() const { return _uptimeS; }
    /**
     * @brief Seconds since a recorded startS, clamped. Use this instead of uptimeS() - startS, which
     * underflows whenever the stamp is read after the counter has moved on.
     */
    uint32_t secondsSince(uint32_t startS) const { const uint32_t u = _uptimeS; return u >= startS ? u - startS : 0; }
    /** @brief Connectable data-tunnel count (spec/config maximum). */
    uint8_t tunnelMax() const { return KNX_TUNNELING; }
    /** @brief Currently open data tunnels (ChannelId != 0). */
    uint8_t tunnelCount() const;
    /** @brief KNXnet/IP telegram counters (03_08_03); null on builds that do not keep them. */
    void setCounters(KnxIpCounters* counters) { _counters = counters; }

    /** @brief Read-only i-th open data tunnel (0..tunnelCount()-1); nullptr if out of range. */
    const KnxIpTunnelConnection* tunnelAt(uint8_t index) const;

    // Tunnel connection type + how a session ended, for the active list / history.
    enum TunnelType : uint8_t { TUN_DATA = 0, TUN_CONFIG = 1, TUN_BUSMON = 2, TUN_OTHER = 3 };
    // A refused CONNECT_REQUEST is recorded too (else a client hammering an unsupported type is invisible);
    // `detail` carries the offending CRI type / KNX layer octet.
    enum TunnelEndReason : uint8_t
    {
        END_ACTIVE = 0,
        END_CLOSED = 1,   // the client asked to disconnect -- the only ending it caused itself
        END_TIMEOUT = 2,  // no heartbeat within 120 s (03_08_02 Core 5.4)
        END_BUSMON = 3,   // closed so a busmonitor can own the bus alone (03_08_04 2.2.4)
        END_EVICTED = 4,  // the slot was handed to the client it is reserved for
        END_NOACK = 5,    // no ACK after the last repeat (03_08_04 2.6.1)
        END_OVERFLOW = 6, // send queue overflowed with a frame that must not be dropped
        END_LOCAL = 7,       // closed on the device's own request (web / console / before a restart)
        END_BUSMON_LOST = 8, // HW monitor mode ended underneath the connection -- nobody asked for it
        // Everything below is a REFUSED connect, not a session. The `reason >= END_REJ_TYPE` test in the
        // products relies on that split, so a new SESSION reason goes above this line, never appended.
        END_REJ_TYPE = 9,   // connection type not supported (03_08_02 Table 7) -> detail = CRI type
        END_REJ_LAYER = 10, // tunnelling layer not supported (03_08_04 Table 10) -> detail = layer
        END_REJ_BUSY = 11,     // no connection available right now (busmon owns the bus / all slots taken)
        END_REJ_TRANSPORT = 12 // Device Management refused: a FOREIGN transport connection holds the layer
                               // (03_08_03 2.6.1.2 / 08_TSSH 8.3.2). Own reason because it also answers
                               // 0x24, and reporting it as "no free tunnel" on a device with 16 free
                               // tunnels sends every diagnosis in the wrong direction.
    };
    // One tunnel session. Times are millis()-relative (uptime); the console converts start to an absolute
    // wall-clock time on the fly when the clock is valid, so it stays correct even if the clock arrives later.
    struct TunnelEvent
    {
        uint32_t ip = 0;
        uint16_t pa = 0;               // KNX individual address (0 for config/busmon)
        uint8_t type = TUN_DATA;       // TunnelType
        uint8_t reason = END_ACTIVE;   // TunnelEndReason (END_ACTIVE for the live list)
        uint8_t detail = 0;            // rejected attempts: the offending CRI type / KNX layer octet
        uint8_t slot = 0xFF;           // index in tunnels[], 0xFF when it does not belong to one
        uint8_t resSlot = 0xFF;        // slot reserved for this client at connect time, 0xFF for none
        uint8_t chId = 0;              // KNXnet/IP channel id; live list only, 0 in a history entry
        unsigned long startMillis = 0; // millis() at connect
        unsigned long endMillis = 0;   // millis() at disconnect (0 while active)
        unsigned long hbMillis = 0;    // millis() of the last accepted datagram; 0 for history entries
        uint32_t startS = 0;           // uptime seconds at connect -- "how long ago did this START"
        uint32_t ageS = 0;             // seconds this session has run (final duration for a history entry)
        // Copies of the connection's per-session counters (0 on a refused connect, which never had one).
        uint32_t toClient = 0;
        uint32_t fromClient = 0;
        uint16_t resend = 0;
        uint16_t seqGap = 0;
        uint16_t ackErr = 0;
        uint16_t txDrop = 0;
        uint16_t grpDrop = 0;
        uint8_t queuePeak = 0;
        uint8_t queueDepth = 0; // FIFO slots this build provides, so "1 / 3" needs no constant in the UI
    };
    /**
     * @brief Reserved-tunnel configuration as ETS wrote it, for diagnostics only.
     * One control byte per tunnel (bit 7 reserved, bits 6-5 behaviour when busy) and one IPv4 each.
     * Both return nullptr unless the property carries exactly KNX_TUNNELING entries.
     * The returned memory belongs to the property and is freed on the next ETS write to it, so this
     * must only be called from the KNX loop -- never from a web handler on another task.
     */
    const uint8_t* reservedTunnelsCtrl();
    const uint8_t* reservedTunnelsIp();

    /** @brief Snapshot of all currently open connections (data + config + busmon); returns count. */
    uint8_t activeTunnels(TunnelEvent* out, uint8_t maxOut) const;
    /** @brief Number of recorded finished sessions (up to the ring size). */
    uint8_t tunnelHistoryCount() const;
    /**
     * @brief Copy out the i-th finished session, index 0 = newest; false if out of range or torn.
     * A COPY, not a pointer: the web handler runs on another task on ESP32 while the KNX loop can be
     * overwriting the very slot it is reading, which used to splice two sessions into one plausible
     * but wrong row. A caller that gets false must SKIP the row, not abort the list; `out` is then
     * UNSPECIFIED -- an out-of-range index leaves it untouched, a torn copy leaves a half-written one.
     */
    bool tunnelHistoryCopy(uint8_t index, TunnelEvent& out) const;

    /** @brief True if the channel is a KNXnet/IP Device Management connection (not a tunnel). */
    bool isConfigChannel(uint8_t channelId) const;

    /**
     * @brief Close one open connection by its KNXnet/IP channel id; false if no such channel is open.
     * @param sent optional out: whether the client could be told (the slot is reaped either way).
     * Sends DISCONNECT_REQUEST to the client's control endpoint, which 03_08_02 §9.2 lists as mandatory
     * in the server->client direction. Must run in the KNX loop, not from a web task.
     */
    bool closeTunnel(uint8_t channelId, uint8_t reason = END_LOCAL, bool* sent = nullptr);
    /**
     * @brief Close every open connection and return how many were closed.
     * @param withBusMon also end a KNX-Busmonitor tunnel, which leaves HW monitor mode with it.
     * @param sent optional out: how many clients could actually be told (the slot is reaped either way).
     */
    uint8_t closeAllTunnels(uint8_t reason = END_LOCAL, bool withBusMon = true, uint8_t* sent = nullptr);

    /** @brief Send one cEMI frame to every open device management connection (evented M_PropInfo.ind). */
    void dataRequestToAllDevMgmt(CemiFrame& frame);

#ifdef OPENKNX_HW_BUSMON
    /** @brief Register the TP DLL bridge used to enter/leave HW busmonitor mode (router BAU only). */
    void setHwBusMonitorDll(IHwBusMonitorDll* dll) { _hwBusMon = dll; }
    /** @brief True while at least one KNX-Busmonitor tunnel is open (chip in HW monitor mode). */
    bool busMonitorActive()
    {
        for (uint8_t i = 0; i < KNX_BUSMON_CONNECTIONS; i++)
            if (_busMonTunnel[i].ChannelId != 0)
                return true;
        return false;
    }
    /** @brief Channel id of the FIRST open busmonitor tunnel, 0 if none. Saves snapshotting the whole list. */
    uint8_t busMonitorChannelId() const
    {
        for (uint8_t i = 0; i < KNX_BUSMON_CONNECTIONS; i++)
            if (_busMonTunnel[i].ChannelId != 0)
                return _busMonTunnel[i].ChannelId;
        return 0;
    }
    /** @brief Number of open busmonitor tunnels. */
    uint8_t busMonitorCount() const
    {
        uint8_t n = 0;
        for (uint8_t i = 0; i < KNX_BUSMON_CONNECTIONS; i++)
            if (_busMonTunnel[i].ChannelId != 0)
                n++;
        return n;
    }
    /** @brief Forward one raw monitor-mode LPDU (incl. FCS) to every open busmon tunnel as L_Busmon.ind. */
    void busMonitorFrame(uint8_t* lpdu, uint16_t len, uint8_t status = 0);
    /** @brief Close every open data/config tunnel so a busmonitor is the only connection (03_08_04 §2.2.4).
     *  Used by the ETS busmon connect AND the local console `bcu mon` toggle so both are equally exclusive. */
    void closeTunnelsForBusmon();
#endif

  private:

    void sendFrameToTunnel(KnxIpTunnelConnection *tunnel, CemiFrame& frame);
    bool sendDisconnectRequest(KnxIpTunnelConnection *t); // tell the client; false = datagram never left
#ifdef KNX_TUNNEL_RESEND
    void pumpTunnel(KnxIpTunnelConnection *t);                 // send the FIFO head if nothing is in flight
    bool evictOldestGroupFrame(KnxIpTunnelConnection *t);     // free one slot by dropping the oldest queued
                                                              // group telegram; false = nothing evictable
    void repeatOrDisconnect(KnxIpTunnelConnection *t);         // repeat the head once, then tear down
    void disconnectTunnel(KnxIpTunnelConnection *t, uint8_t reason); // server-initiated teardown + reap
    void handleTunnelAck(uint8_t *buffer, uint16_t length, uint32_t src_addr); // pop the acked head + pump the next
#endif
    void HandleConnectRequest(uint8_t* buffer, uint16_t length, uint32_t& src_addr, uint16_t& src_port);
    // src_addr/src_port: the UDP sender, needed to resolve a route-back control HPAI (03_08_02 8.6.2.2).
    void HandleConnectionStateRequest(uint8_t* buffer, uint16_t length, uint32_t src_addr, uint16_t src_port);
    void HandleDisconnectRequest(uint8_t* buffer, uint16_t length, uint32_t src_addr, uint16_t src_port);
    void HandleDescriptionRequest(uint8_t* buffer, uint16_t length, uint32_t src_addr, uint16_t src_port);
    void HandleDeviceConfigurationRequest(uint8_t* buffer, uint16_t length, uint32_t src_addr);
    void HandleTunnelingRequest(uint8_t* buffer, uint16_t length, uint32_t src_addr);


    KnxIpTunnelConnection tunnels[KNX_TUNNELING+KNX_TUNNELING_DEVMGMT];
    uint8_t _lastChannelId = 0;

    // Rolling connect/disconnect history (newest overwrites oldest).
    // 32 = 16 tunnels each connecting + disconnecting once, so a full round is retained.
    static const uint8_t TUNNEL_HISTORY_SIZE = 32;
    // Seqlock over _history/_historyHead/_historyCount: odd while an entry is being written. One
    // writer only (the KNX loop), so a plain load+store beats a read-modify-write.
    std::atomic<uint32_t> _histSeq{0};
    TunnelEvent _history[TUNNEL_HISTORY_SIZE];
    uint8_t _historyHead = 0;  // next write slot
    uint8_t _historyCount = 0;

    // Uptime in seconds, accumulated in loop() from millis() deltas so it is immune to the 49.7-day wrap.
    uint32_t _uptimeS = 0;
    uint32_t _lastMs = 0;
    uint32_t _msAcc = 0;
    bool _timeInit = false;

    // Cached copy of PID_ADDITIONAL_INDIVIDUAL_ADDRESSES for isConfiguredTunnelPa(). 32 bytes so the TP
    // acknowledge path never walks the property store. Refreshed from loop() once a second, which is far
    // faster than an ETS download can change the list and then run an address-in-use check.
    uint16_t _tunnelPaPool[KNX_TUNNELING] = {0};
    uint8_t _tunnelPaPoolCount = 0;
    uint32_t _tunnelPaPoolMs = 0;
    void refreshTunnelPaPool();
    // conn carries the per-session counters into the history entry; nullptr for a refused connect.
    void copyCounters(TunnelEvent& e, const KnxIpTunnelConnection& c) const;
    void recordTunnelSession(uint32_t ip, uint16_t pa, uint8_t type, unsigned long startMillis, uint8_t reason,
                             uint8_t detail = 0, const KnxIpTunnelConnection* conn = nullptr);
    // Refused CONNECT_REQUEST -> history; coalesces an identical repeat into the newest entry so a
    // retrying client cannot push the real sessions out of the 32-entry ring.
    void recordRejectedConnect(uint32_t ip, uint8_t type, uint8_t reason, uint8_t detail);
    IpParameterObject& _ipParameters;
    DeviceObject& _deviceObject;
    // Single funnel for every datagram this server emits, so PID_MSG_TRANSMIT_TO_IP counts them all
    // (Core, Tunnelling, Device Management, ACKs) as the spec requires.
    bool sendCounted(uint32_t addr, uint16_t port, uint8_t* buffer, uint16_t len);

    KnxIpCounters* _counters = nullptr;
    Platform& _platform;
    CemiServer& _cemiServer;

#ifdef OPENKNX_HW_BUSMON
    void HandleBusMonitorConnect(KnxIpConnectRequest& connRequest, uint32_t src_addr, uint16_t src_port);
    void busMonitorTeardown(uint8_t slot, uint8_t reason = END_CLOSED);
    int busMonSlotByChannel(uint8_t channelId);  // index of the busmon slot holding channelId, -1 if none
    int busMonFreeSlot();                        // index of a free busmon slot, -1 if all are taken

    // Busmon connections. 03_08_04 2.2.4 p.8 asks for one per subnetwork, which is the DEFAULT; the count
    // is configurable because the raw capture stream itself is fan-out friendly (pending KNXA alignment).
    KnxIpTunnelConnection _busMonTunnel[KNX_BUSMON_CONNECTIONS]; // kept out of the L_Data fan-out
    IHwBusMonitorDll* _hwBusMon = nullptr;    // TP chip bridge (null on non-router BAUs)
    uint8_t _busMonSeq = 0;                   // rolling status/sequence nibble for L_Busmon.ind
    bool _busMonExitPending = false;          // exit-recovery poll running (non-blocking)
    uint32_t _busMonExitStart = 0;
    uint32_t _busMonExitResetInd = 0;         // U_Reset.ind count when the poll was armed
#endif
};


#endif
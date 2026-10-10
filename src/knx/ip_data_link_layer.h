#pragma once

#include "config.h"
#ifdef USE_IP

#include <stdint.h>
#include "data_link_layer.h"
#include "ip_parameter_object.h"
#include "knx_ip_counters.h"
#include "service_families.h"
#include "ip_tunnel_server.h"
#ifdef USE_CEMI_SERVER
    #include "cemi_server_object.h"
#endif

class IpDataLinkLayer : public DataLinkLayer
{
    using DataLinkLayer::_deviceObject;

  public:
    /** @brief KNXnet/IP telegram counters (03_08_03); null on builds that do not keep them. */
    void setCounters(KnxIpCounters* counters) { _counters = counters; }

    /** @brief Source of the extended DIBs' APDU lengths (PID 68/69). Unset = encoder default. */
#ifdef USE_CEMI_SERVER
    void cemiServerObject(CemiServerObject& obj) { _cemiServerObject = &obj; }
#endif
    IpDataLinkLayer(DeviceObject& devObj, IpParameterObject& ipParam,
                    NetworkLayerEntity& netLayerEntity,
                    Platform& platform, BusAccessUnit& busAccessUnit,
#ifdef KNX_TUNNELING
                    IpTunnelServer& ipTunnelServer,
#endif
                    DataLinkLayerCallbacks* dllcb = nullptr);

    void loop();
    void enabled(bool value);
    bool enabled() const;

    /** @brief Link returned: rebuild the endpoint (03_08_02 Core 4.2). false only if the rebuild failed. */
    bool networkChanged(bool afterOutage);
    DptMedium mediumType() const override;

    // Interface mode: when false, inbound KNXnet/IP RoutingIndications are ignored. A Tunnelling-only
    // interface (e.g. Bau07B0IP) does not receive group communication over IP -- group comm is on TP.
    // Discovery (SearchRequest) and the tunnel receive-path stay active regardless.
    void enableRoutingIndications(bool value) { _rxRoutingIndications = value; }

    // System setup group socket (03_02_06 4.1.3): opened while the IP SBC mode is on, unless _setupOnRoutingSocket or not isolated.
    void enableSystemBroadcast(bool value);
    /** @brief Move the routing endpoint to another multicast group at the next loop pass (A_DomainAddressSerialNumber_Write). */
    void changeRoutingGroup(uint32_t group);
    bool indicatingIpSystemBroadcast() const override { return _indicatingSbc; }
    /** @brief M_Reset.req: applies PID 67 and the routing group (03_05_02 3.7.1.1.1); re-joins only when one of them requires it. */
    void reinitialise();

#ifdef KNX_IS_ROUTER
    /** @brief Report LAN->KNX losses with ROUTING_LOST_MESSAGE (03_08_05 2.3.4), about once per second (08_TSSH 6.2.2). */
    void loopLostMessage();
    /** @brief True while a received ROUTING_BUSY holds back ROUTING_INDICATIONs; the TP side then acknowledges routed frames with BUSY. */
    bool routingPaused() const { return _busyHold && (int32_t)(millis() - _busyUntil) < 0; }
#endif

  private:
    bool joinMultiCast();        // join _joinedGroup
    static uint32_t usableGroup(uint32_t group); // the group, or 224.0.23.12 outside 224.0.0.0/4 or in 224.0.0.0/24
    uint32_t multiCastAddress(); // property source; read once at enable, then latched
    uint32_t systemSetupMultiCastAddress();
    void loopSystemBroadcast();
    void receivedSystemBroadcast(uint8_t* buffer, int len);
    uint32_t _pendingGroup = 0;
    uint8_t readTtl();           // PID 67
    uint8_t _ttl = 16;           // PID 67 as last applied
    bool _indicatingSbc = false; // set around frameReceived() of a 0533h frame
    bool _sbcOpenFailed = false; // the secondary socket could not be opened; no retry until the mode goes off
    bool _setupOnRoutingSocket = false; // the routing socket receives the system setup group (same group, or joined extra)
    uint32_t _setupGroup = 0;           // PID 65 as read at the join
    bool _groupChangePending = false;
    bool sendSystemBroadcast(CemiFrame& frame);
    bool _sbcSocketOpen = false;
    uint32_t _joinedGroup = 0; // 03_08_03 2.5.17: PID 66 becomes active on reset, not at runtime
    KnxIpCounters* _counters = nullptr;
    bool sendUniCastCounted(uint32_t addr, uint16_t port, uint8_t* buffer, uint16_t len); // a KNX message: PID 74 or 72
    bool sendControlUniCast(uint32_t addr, uint16_t port, uint8_t* buffer, uint16_t len); // PID 74 only
    bool _enabled = false;
    bool _rxRoutingIndications = true;
    // 03_02_06 2.1 p.6: at most 50 datagrams within one second (routing indications and system broadcasts); last 50 send times.
    static constexpr uint8_t kSendLimit = 50;
    static constexpr uint32_t kSendWindowMs = 1005; // 1 s + 5 ms for millis() steps and send-path jitter
    uint32_t _sendTimes[kSendLimit] = {0};
    uint8_t _sendIdx = 0;   // oldest entry once the ring is full
    uint8_t _sendCount = 0;
#ifdef KNX_IS_ROUTER
    uint32_t _lostReported = 0; // KnxIpCounters::lostToKnx() covered by sent reports
    uint32_t _lostSentAt = 0;   // millis() of the last report attempt
    uint32_t _routingSentAt = 0; // millis() of the last ROUTING_INDICATION sent
    uint32_t _routingSentUs = 0; // micros() of the same, for the 20 ms pacing
    // ROUTING_BUSY flow control, 03_08_05 2.3.5 p.10-11
    static_assert(KNX_IP_TX_QUEUE_SIZE >= 1 && KNX_IP_TX_QUEUE_SIZE <= 255, "uint8_t queue indices");
    static constexpr uint8_t kTxQueueSize = KNX_IP_TX_QUEUE_SIZE;
    uint8_t* _txQueue[kTxQueueSize] = {nullptr}; // complete ROUTING_INDICATION datagrams, oldest at _txHead
    uint16_t _txLen[kTxQueueSize] = {0};
    uint8_t _txHead = 0;
    uint8_t _txCount = 0;
    volatile bool _busyHold = false;  // a ROUTING_BUSY wait is running; cleared once it expired
    volatile uint32_t _busyUntil = 0; // no ROUTING_INDICATION before this time (tw + trandom); read by the TP ACK decision
    uint32_t _busyWaitEnd = 0;        // end of tw alone, for the "higher remaining time" rule
    uint32_t _busyRxAt = 0;           // last ROUTING_BUSY received
    uint32_t _busyRxSrc = 0;          // its sender (network order)
    uint32_t _busyBurstAt = 0;        // first ROUTING_BUSY of the running pause
    uint16_t _busyBurstTw = 0;        // its tw
    uint8_t _sendRefusals = 0;        // routing indications the network driver refused in a row
    uint32_t _busyNDecAt = 0;         // last decrement of _busyN
    uint8_t _busyN = 0;               // ROUTING_BUSY frames in the moving period
    bool _busyDecaying = false;       // N decay running (from the resume on)
    void flushTxQueue();
    void confirmQueued(bool success); // dequeues the head and emits its L_Data.con
    uint32_t _busyPhase = 0;          // golden-ratio sequence for trandom, 0 = not seeded
    uint8_t _busyPauseSlot = 0;       // 0..5: slot 0 takes the low end of the range, slot 3 the high end
    static constexpr int32_t kBusyNoteGapMs = 10;           // N counts ROUTING_BUSY frames this far apart (03_08_05 2.3.5 note 3)
    static constexpr uint32_t kBusyRandomStepMs = 50;       // trandom range per N (03_08_05 2.3.5)
    static constexpr uint32_t kBusyDecayMs = 5;             // tbd: N decay step (03_08_05 2.3.5)
    static constexpr uint32_t kBusySeedMul = 2654435761u;   // Knuth multiplicative hash of the micros() seed
    static constexpr uint32_t kBusyPhaseStep = 0x9E3779B9u; // 2^32 / golden ratio
    static constexpr uint8_t kBusyPauseSlots = 6;
    static constexpr uint8_t kBusySlotLow = 0;              // pause slot that takes the low end of the range
    static constexpr uint8_t kBusySlotHigh = 3;             // pause slot that takes the high end of the range
    void handleRoutingBusy(const uint8_t* buffer, uint16_t len, uint32_t remoteAddr);
    bool canSendIndication(uint32_t now);
    void loopTxQueue();
    enum SendResult : uint8_t { RiSent, RiFailed, RiHeld };
    /** @brief RiHeld: the network driver withheld the datagram for a ROUTING_BUSY waiting in the chip; keep it queued. */
    SendResult sendIndicationNow(uint8_t* data, uint16_t len);
#endif
    bool sendFrame(CemiFrame& frame);
    static_assert(KNX_IP_RX_PER_LOOP >= 1 && KNX_IP_RX_PER_LOOP <= 255, "uint8_t counter");
    static constexpr uint8_t kRxPerLoop = KNX_IP_RX_PER_LOOP; // datagrams read per loop() at most
#ifdef KNX_IS_ROUTER
    static_assert(KNX_ROUTING_INDICATION_GAP_US >= 20001 && KNX_ROUTING_INDICATION_GAP_US <= 100000, "KNX_ROUTING_INDICATION_GAP_US out of 20001..100000");
    static_assert(KNX_ROUTING_SEND_RETRIES >= 1 && KNX_ROUTING_SEND_RETRIES <= 255, "KNX_ROUTING_SEND_RETRIES out of 1..255");
    static_assert(KNX_ROUTING_BUSY_SAME_SOURCE_GAP_MS >= 1 && KNX_ROUTING_BUSY_SAME_SOURCE_GAP_MS <= 10, "KNX_ROUTING_BUSY_SAME_SOURCE_GAP_MS out of 1..10");
    static_assert(KNX_ROUTING_BUSY_BURST_MARGIN_MS >= 0 && KNX_ROUTING_BUSY_BURST_MARGIN_MS <= 20, "KNX_ROUTING_BUSY_BURST_MARGIN_MS out of 0..20");
    static_assert(KNX_ROUTING_BUSY_RESUME_MARGIN_MS >= 0 && KNX_ROUTING_BUSY_RESUME_MARGIN_MS < 50, "KNX_ROUTING_BUSY_RESUME_MARGIN_MS out of 0..49");
#endif
    /** @brief Reads and dispatches one datagram of any service; false when the platform returned none (also after an oversize drop). */
    bool loopReceive();

#if KNX_SERVICE_FAMILY_CORE >= 2
    void loopHandleSearchRequestExtended(uint8_t* buffer, uint16_t length);
#endif
    bool sendBytes(uint8_t* buffer, uint16_t length, bool* held = nullptr); // a ROUTING_INDICATION: PID 74 or 72
#ifdef KNX_IS_ROUTER
    bool sendControlMultiCast(uint8_t* buffer, uint16_t length); // PID 74 only
#endif
    bool isSendLimitReached();
    bool sendLimitPeek(uint32_t now) const; // true while a send now would exceed 50 per second
    void sendLimitRecord(uint32_t now);

    IpParameterObject& _ipParameters;
    DataLinkLayerCallbacks* _dllcb;
#ifdef USE_CEMI_SERVER
    CemiServerObject* _cemiServerObject = nullptr;
#endif
};
#endif
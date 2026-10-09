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

    // IP system broadcast rides on the system setup group, which need not be the routing group
    // (03_02_06 4.1.3). Follows the router's IP System Broadcast Routing Mode: the second socket exists
    // only while that mode is on.
    void enableSystemBroadcast(bool value);
    bool indicatingIpSystemBroadcast() const override { return _indicatingSbc; }

#ifdef KNX_IS_ROUTER
    /** @brief Report LAN->KNX losses with ROUTING_LOST_MESSAGE (03_08_05 2.3.4), about once per second (08_TSSH 6.2.2). */
    void loopLostMessage();
#endif

  private:
    bool joinMultiCast();        // join _joinedGroup
    uint32_t multiCastAddress(); // property source; read once at enable, then latched
    uint32_t systemSetupMultiCastAddress();
    void loopSystemBroadcast();
    bool _indicatingSbc = false; // set around frameReceived() of a 0533h frame
    bool _sbcOpenFailed = false; // the secondary socket could not be opened; no retry until the mode goes off
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
#endif
    bool sendFrame(CemiFrame& frame);


#if KNX_SERVICE_FAMILY_CORE >= 2
    void loopHandleSearchRequestExtended(uint8_t* buffer, uint16_t length);
#endif
    bool sendBytes(uint8_t* buffer, uint16_t length);
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
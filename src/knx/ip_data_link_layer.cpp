#include "config.h"
#ifdef USE_IP

#include "ip_data_link_layer.h"

#include "bits.h"
#include "platform.h"
#include "device_object.h"
#include "knx_ip_routing_indication.h"
#include "knx_ip_routing_system_broadcast.h"
#include "knx_ip_search_request.h"
#include "knx_ip_search_response.h"
#include "knx_ip_search_request_extended.h"
#include "knx_ip_search_response_extended.h"
#include "knx_facade.h"

#include <new>
#include <stdio.h>
#include <string.h>

#define KNXIP_HEADER_LEN 0x6
#define KNXIP_PROTOCOL_VERSION 0x10

#define MIN_LEN_CEMI 10

IpDataLinkLayer::IpDataLinkLayer(DeviceObject& devObj, IpParameterObject& ipParam,
                                 NetworkLayerEntity &netLayerEntity,
                                 Platform& platform, BusAccessUnit& busAccessUnit,
#ifdef KNX_TUNNELING
                                IpTunnelServer& ipTunnelServer,
#endif
                                 DataLinkLayerCallbacks* dllcb) : DataLinkLayer(devObj, netLayerEntity, platform, busAccessUnit
#ifdef KNX_TUNNELING
                                                                                    , ipTunnelServer
#endif
                                ),
                                 _ipParameters(ipParam),
                                 _dllcb(dllcb)
{
}

bool IpDataLinkLayer::sendFrame(CemiFrame& frame)
{
    if (!_rxRoutingIndications)
    {
        dataConReceived(frame, false);
        return false;
    }
    // 03_02_06 4.1.3: a frame the coupler marks SysBroadcast (IP SBC routing mode on) or the answer to an IP system broadcast
    // (4.3.5.3.1) leaves as ROUTING_SYSTEM_BROADCAST, never as a ROUTING_INDICATION.
    if (frame.systemBroadcast() == SysBroadcast)
        return sendSystemBroadcast(frame);

    KnxIpRoutingIndication packet(frame);
#ifdef KNX_IS_ROUTER
    // Held back instead of dropped while ROUTING_BUSY, its slow start or the 50/s limit apply (03_08_05 2.3.5).
    const uint32_t now = millis();
    if (_txCount == 0 && canSendIndication(now))
    {
        const SendResult result = sendIndicationNow(packet.data(), packet.totalLength());
        if (result != RiHeld)
        {
            dataConReceived(frame, result == RiSent);
            return result == RiSent;
        }
    }
    // Refused at once without an endpoint (03_08_05 3.7.2 p.13), with a full queue or without memory.
    uint8_t* copy = (_enabled && _txCount < kTxQueueSize) ? new (std::nothrow) uint8_t[packet.totalLength()] : nullptr;
    if (copy == nullptr)
    {
        if (_counters != nullptr)
            _counters->incrementOverflowToIp();
        dataConReceived(frame, false);
        return false;
    }
    memcpy(copy, packet.data(), packet.totalLength());
    const uint8_t tail = (uint8_t)((_txHead + _txCount) % kTxQueueSize);
    _txQueue[tail] = copy;
    _txLen[tail] = packet.totalLength();
    _txCount++;
    return true; // the L_Data.con follows when the datagram leaves or is discarded (confirmQueued())
#else
    // only send 50 packet per second: see KNX 3.2.6 p.6
    if (isSendLimitReached())
    {
        if (_counters != nullptr)
            _counters->incrementOverflowToIp();
        // the send-limit drop was the only branch returning false without a dataCon -> the upper layer
        // waited for a confirmation that never arrived; emit the negative con to close that gap
        dataConReceived(frame, false);
        return false;
    }
    bool success = sendBytes(packet.data(), packet.totalLength());
#ifdef KNX_ACTIVITYCALLBACK
    if(_dllcb)
        _dllcb->activity((_netIndex << KNX_ACTIVITYCALLBACK_NET) | (KNX_ACTIVITYCALLBACK_DIR_SEND << KNX_ACTIVITYCALLBACK_DIR));
#endif
    dataConReceived(frame, success);
    return success;
#endif
}

#ifdef KNX_IS_ROUTER
IpDataLinkLayer::SendResult IpDataLinkLayer::sendIndicationNow(uint8_t* data, uint16_t len)
{
    const uint32_t now = millis(); // read after canSendIndication() polled the chip, which takes time
    bool held = false;
    const bool success = sendBytes(data, len, &held);
    if (held)
        return RiHeld;
    if (success)
    {
        sendLimitRecord(now);
        _routingSentAt = now; // tslow spacing and the 5 ms pause before a ROUTING_LOST_MESSAGE (03_02_06 2.1)
        _routingSentUs = micros();
    }
#ifdef KNX_ACTIVITYCALLBACK
    if(_dllcb)
        _dllcb->activity((_netIndex << KNX_ACTIVITYCALLBACK_NET) | (KNX_ACTIVITYCALLBACK_DIR_SEND << KNX_ACTIVITYCALLBACK_DIR));
#endif
    return success ? RiSent : RiFailed;
}

bool IpDataLinkLayer::canSendIndication(uint32_t now)
{
    if (_platform.routingBusyPending())
        return false; // a ROUTING_BUSY arrived and is not read yet
    if (_busyHold && (int32_t)(now - _busyUntil) < 0)
        return false; // tw + trandom of a received ROUTING_BUSY
    // 20 ms after every ROUTING_INDICATION (03_02_06 2.1 p.6, also covers tslow of 03_08_05 Figure 5); the 50/s window applies as well.
    if ((uint32_t)(micros() - _routingSentUs) < KNX_ROUTING_INDICATION_GAP_US)
        return false;
    if (sendLimitPeek(now))
        return false;
    _platform.pollReceive(); // a ROUTING_BUSY may still wait in the network chip
    return !_platform.routingBusyPending();
}

void IpDataLinkLayer::loopTxQueue()
{
    const uint32_t now = millis();
    if (_busyHold && (int32_t)(now - _busyUntil) >= 0)
        _busyHold = false;
    // N decays every 5 ms (tbd) from the resume; deviation: 03_08_05 2.3.5 p.11 starts the decay after tslowduration = N * 100 ms.
    // The 20 ms gap between ROUTING_INDICATIONs keeps the slow restart.
    if (_busyN > 0 && !_busyHold)
    {
        if (!_busyDecaying)
        {
            _busyDecaying = true;
            _busyNDecAt = now;
        }
        const uint32_t steps = (uint32_t)(now - _busyNDecAt) / kBusyDecayMs;
        if (steps > 0)
        {
            _busyN = (steps >= _busyN) ? 0 : (uint8_t)(_busyN - steps);
            _busyNDecAt += steps * kBusyDecayMs;
        }
    }
    if (_txCount == 0 || !canSendIndication(now))
        return;

    const SendResult result = sendIndicationNow(_txQueue[_txHead], _txLen[_txHead]);
    if (result == RiHeld)
        return; // stays at the head; the ROUTING_BUSY is read in the next pass
    confirmQueued(result == RiSent);
}

// Dequeues the head before its L_Data.con: a frame sent from within the con finds a consistent queue.
void IpDataLinkLayer::confirmQueued(bool success)
{
    uint8_t* const datagram = _txQueue[_txHead];
    const uint16_t len = _txLen[_txHead];
    _txQueue[_txHead] = nullptr;
    _txHead = (uint8_t)((_txHead + 1) % kTxQueueSize);
    _txCount--;
    CemiFrame frame(datagram + LEN_KNXIP_HEADER, len - LEN_KNXIP_HEADER);
    dataConReceived(frame, success);
    delete[] datagram;
}

// 03_08_05 3.7.2 p.13: queued telegrams are discarded when the connection to the medium is lost; the pause goes too.
// Each gets a negative L_Data.con and counts in PID 72. Callers clear _enabled first, so a con cannot queue a new frame.
void IpDataLinkLayer::flushTxQueue()
{
    while (_txCount > 0)
    {
        if (_counters != nullptr)
            _counters->incrementOverflowToIp();
        confirmQueued(false);
    }
    _busyHold = false;
    _sendRefusals = 0;
}

void IpDataLinkLayer::handleRoutingBusy(const uint8_t* buffer, uint16_t len, uint32_t remoteAddr)
{
    // 03_08_05 5.4 Figure 8 p.17: header + 6 (03_02_06 Figure 4 shows + 4, its own fields add to 6); a
    // shorter frame is ignored, our own one looped back as well.
    if (len < KNXIP_HEADER_LEN + 6 || remoteAddr == htonl(_platform.currentIpAddress()))
        return;
    if (_counters != nullptr)
        _counters->incrementBusyRx();

    // Every control field value stops sending for tw: we do not interpret it (03_08_05 p.11). Timed from reception.
    const uint32_t now = _platform.routingBusyReceivedAt(millis());
    uint16_t tw = getWord(buffer + KNXIP_HEADER_LEN + 2);
    if (tw > 100)
        tw = 100; // receiver-side deviation: the 100 ms cap of 03_08_05 2.3.5 p.11 binds the sender; a larger tw is cut to 100

    // N counts ROUTING_BUSY frames 10 ms or more apart (03_08_05 2.3.5); deviation: one sender's next one from KNX_ROUTING_BUSY_SAME_SOURCE_GAP_MS.
    // Signed gap, _busyRxAt kept at the latest: a later read may carry an earlier arrival.
    const bool first = _busyN == 0;
    const int32_t gap = (int32_t)(now - _busyRxAt);
    if (first || gap >= kBusyNoteGapMs || (remoteAddr == _busyRxSrc && gap >= KNX_ROUTING_BUSY_SAME_SOURCE_GAP_MS))
    {
        if (_busyN < 255)
            _busyN++;
    }
    if (first || gap > 0)
        _busyRxAt = now;
    _busyRxSrc = remoteAddr;

    if (!_busyHold || (int32_t)(now - _busyUntil) >= 0)
    {
        _busyBurstAt = now; // first ROUTING_BUSY of this pause
        _busyBurstTw = tw;
        // Slot scheme, deviation from the random real number of 03_08_05 2.3.5 p.11: 6 slots in turn from a random start; slot 0 waits 0, slot 3 the full range, the others a golden-ratio fraction.
        if (_busyPhase == 0)
        {
            _busyPhase = micros() * kBusySeedMul;
            _busyPauseSlot = (uint8_t)(_busyPhase % kBusyPauseSlots);
        }
        _busyPhase += kBusyPhaseStep;
        _busyPauseSlot = (uint8_t)((_busyPauseSlot + 1u) % kBusyPauseSlots);
    }

    // The higher of the remaining wait and the new tw applies (p.11).
    const uint32_t waitEnd = now + tw + 1; // +1: millis() granularity, the real wait is never below tw
    if (!_busyHold || (int32_t)(waitEnd - _busyWaitEnd) > 0)
        _busyWaitEnd = waitEnd;

    // trandom = slot fraction of a range of N * 50 ms less the resume margin and the tw growth of this pause, never below that of N = 1.
    // Deviation from 03_08_05 2.3.5 p.11: at N = 1 the range ends at 50 - margin (32 ms by default), so (32, 50] ms is never used.
    const int32_t moved = (int32_t)(_busyWaitEnd - (_busyBurstAt + _busyBurstTw + 1));
    // moved rests on read times, not on arrivals: a sequence gets KNX_ROUTING_BUSY_BURST_MARGIN_MS on top for that error.
    int32_t range = (int32_t)((uint32_t)_busyN * kBusyRandomStepMs - KNX_ROUTING_BUSY_RESUME_MARGIN_MS)
                  - (moved > 0 ? moved + KNX_ROUTING_BUSY_BURST_MARGIN_MS : 0);
    if (range < (int32_t)kBusyRandomStepMs - KNX_ROUTING_BUSY_RESUME_MARGIN_MS)
        range = (int32_t)kBusyRandomStepMs - KNX_ROUTING_BUSY_RESUME_MARGIN_MS;
    const uint32_t u = _busyPauseSlot == kBusySlotLow ? 0u : _busyPauseSlot == kBusySlotHigh ? 0xFFFFFFFFu : _busyPhase;
    const uint32_t trandom = (uint32_t)(((uint64_t)u * ((uint32_t)range + 1u)) >> 32);
    // The "higher value" rule applies to tw (_busyWaitEnd); the pause's phase is scaled to the range again after each
    // ROUTING_BUSY, so the resume follows the grown N and is never before the last tw.
    _busyUntil = _busyWaitEnd + trandom;
    _busyDecaying = false;
    _busyHold = true;
}

void IpDataLinkLayer::loopRoutingBusy(uint16_t txQueueFill)
{
    // ROUTING_BUSY from KNX_ROUTING_BUSY_TP_FILL queued TP frames or KNX_ROUTING_BUSY_RX_BACKLOG unread datagrams (03_08_05 2.3.5).
    // At most every tw/2; within one episode a repeat needs a ROUTING_INDICATION received since the last ROUTING_BUSY.
    if (!_enabled || (txQueueFill < KNX_ROUTING_BUSY_TP_FILL && _platform.multicastRxBacklog() < KNX_ROUTING_BUSY_RX_BACKLOG))
    {
        _busyEpisode = false;
        return;
    }
    const uint32_t now = millis();
    if ((uint32_t)(now - _routingSentAt) < KNX_ROUTING_SLOW_GAP_MS)
        return; // 5 ms pause after a ROUTING_INDICATION (03_02_06 2.1 p.6)

    uint16_t tw = 100;
    const Property* prop = _ipParameters.property(PID_ROUTING_BUSY_WAIT_TIME);
    if (prop != nullptr)
        prop->read(tw);
    if (tw < 20)
        tw = 20; // 03_08_05 p.11: 20 ms <= tw <= 100 ms
    else if (tw > 100)
        tw = 100;
    if (_busySent && (uint32_t)(now - _busySentAt) < (uint32_t)(tw / 2))
        return; // also across episodes: a queue hovering at the threshold must not send one per drained frame
    if (_busyEpisode && !_ipRxSinceBusy)
        return;

    uint8_t busy[] = {KNXIP_HEADER_LEN, KNXIP_PROTOCOL_VERSION, 0x05, 0x32, 0x00, 0x0C, 0x06, _ipParameters.deviceState(),
                      (uint8_t)(tw >> 8), (uint8_t)tw, 0x00, 0x00};
    _busySentAt = now;
    _busySent = true;
    _busyEpisode = true;
    _ipRxSinceBusy = false;
    sendControlMultiCast(busy, sizeof(busy));
}
#endif



void IpDataLinkLayer::loop()
{
    if (!_enabled)
        return;
    if (_groupChangePending)
    {
        _groupChangePending = false;
        enableSystemBroadcast(false);
        _platform.closeMultiCast();
        const uint32_t previous = _joinedGroup;
        _joinedGroup = _pendingGroup;
        _enabled = joinMultiCast();
        if (!_enabled)
        {
            // Not joinable here: keep the endpoint on the group that worked rather than losing it until reboot.
            _platform.closeMultiCast();
            _joinedGroup = previous;
            _enabled = joinMultiCast();
#ifdef KNX_IS_ROUTER
            if (!_enabled)
                flushTxQueue();
#endif
            if (!_enabled)
                return;
        }
    }
    loopSystemBroadcast();

    // Up to kRxPerLoop datagrams of every service (routing, tunnelling, discovery) per pass, router and interface alike.
    // Router: the queue is served between datagrams; a ROUTING_BUSY still unread holds the send (canSendIndication()).
#ifdef KNX_IS_ROUTER
    loopTxQueue();
#endif
    uint8_t received = 0;
    while (received < kRxPerLoop && loopReceive())
    {
        received++;
#ifdef KNX_IS_ROUTER
        if (_txCount > 0 || _busyHold) // only while something waits to go out or a pause runs
        {
            _platform.pollReceive(); // stamps a ROUTING_BUSY waiting in a main-loop-pumped chip now
            loopTxQueue();
        }
#endif
    }
}

bool IpDataLinkLayer::loopReceive()
{
    uint8_t buffer[KNXIP_RX_MAX_DATAGRAM];
    uint16_t remotePort = 0;
    uint32_t remoteAddr = 0;
    int len = _platform.readBytesMultiCast(buffer, sizeof(buffer), remoteAddr, remotePort);
    if (len <= 0)
        return false;

    if (len < KNXIP_HEADER_LEN)
        return true;
    
    if (buffer[0] != KNXIP_HEADER_LEN
        || buffer[1] != KNXIP_PROTOCOL_VERSION)
    {
        // Stays a silent discard -- 08_TSSH 3.2.2 (ID 10202, p.16) expects no answer at all, and a
        // negative confirmation would fail that case. What 03_08_02 6.2 p.15 asks for on top is the
        // teardown of a connection whose version changed mid-stream, which needs the channel and the
        // sender, so the tunnel server decides it.
#ifdef KNX_TUNNELING
        if (buffer[0] == KNXIP_HEADER_LEN)
            _ipTunnelServer.shutdownOnProtocolVersionChange(buffer, (uint16_t)len, remoteAddr);
#endif
        return true;
    }

    // KNXnet/IP total length (octets 4-5) must equal the received datagram; a mismatch is malformed ->
    // discard (TSSH 3.2.4/3.2.5, both refs do). Well-formed short frames (declared==len) still pass.
    uint16_t declaredLen;
    popWord(declaredLen, buffer + 4);
    if (declaredLen != (uint16_t)len)
    {
        return true;
    }

#ifdef KNX_ACTIVITYCALLBACK
    if(_dllcb)
        _dllcb->activity((_netIndex << KNX_ACTIVITYCALLBACK_NET) | (KNX_ACTIVITYCALLBACK_DIR_RECV << KNX_ACTIVITYCALLBACK_DIR));
#endif

    uint16_t code;
    popWord(code, buffer + 2);

    // A system setup group joined apart from the routing group (joinMultiCastExtra(), RP2040 and ESP32) delivers only
    // knxIpSetupGroupService(): the platform receive callback drops the rest before it is queued.
    switch ((KnxIpServiceType)code)
    {
        case RoutingIndication:
        {
            // Interface mode (no routing): drop inbound group traffic from IP -- it must not enter the
            // local stack. Group communication happens on the TP link only.
            if (!_rxRoutingIndications)
                break;
            KnxIpRoutingIndication routingIndication(buffer, len);
            // validate the inbound multicast cEMI before forwarding it to TP -- a malformed/0-length routing
            // frame from ETS would otherwise be pushed onto the bus as garbage. Order matters: totalLenght()!=0
            // first, so the short-circuit keeps valid() from OOB-reading a zero-length frame.
            if (routingIndication.frame().totalLenght() != 0 && routingIndication.frame().valid())
            {
#ifdef KNX_IS_ROUTER
                _ipRxSinceBusy = true; // a sender still talks: our ROUTING_BUSY may be repeated
#endif
                frameReceived(routingIndication.frame());
            }
            break;
        }

        case RoutingSystemBroadcast:
            // 03_02_06 4.1.3: valid only on the system setup group; unicast is ignored by destination where the platform reports it.
#ifdef KNX_IS_ROUTER
            {
                // Where the platform reports the destination, only the system setup group counts (unicast is ignored).
                const uint32_t dest = _platform.lastMulticastDestination();
                if (dest != 0 ? dest == _setupGroup : _joinedGroup == _setupGroup)
                    receivedSystemBroadcast(buffer, len);
            }
#endif
            break;


        case SearchRequest:
        {
            // The discovery endpoint HPAI sits at buffer[6..13] (03_08_02 7.6.1). Without this bound a
            // 6-octet SEARCH_REQUEST -- which passes the header and declared-length checks above -- made
            // hpai() a view over uninitialised stack, and the SEARCH_RESPONSE went to whatever address and
            // port that stale memory happened to hold.
            if (len < KNXIP_HEADER_LEN + LEN_IPHPAI)
                break;

            KnxIpSearchRequest searchRequest(buffer, len);
            KnxIpSearchResponse searchResponse(_ipParameters, _deviceObject);

            auto hpai = searchRequest.hpai();
            if (hpai.code() != IPV4_UDP) break; // 03_08_02 Core 7.6.1: discovery is UDP-only -> discard a TCP-HPAI SEARCH_REQUEST
#ifdef KNX_ACTIVITYCALLBACK
            if(_dllcb)
                _dllcb->activity((_netIndex << KNX_ACTIVITYCALLBACK_NET) | (KNX_ACTIVITYCALLBACK_DIR_SEND << KNX_ACTIVITYCALLBACK_DIR) | (KNX_ACTIVITYCALLBACK_IPUNICAST));
#endif
            sendControlUniCast(hpai.ipAddress(), hpai.ipPortNumber(), searchResponse.data(), searchResponse.totalLength());
            break;
        }
        case SearchRequestExt:
        {
            #if KNX_SERVICE_FAMILY_CORE >= 2
            loopHandleSearchRequestExtended(buffer, len);
            #endif
            break;
        }
        // Both consumed without a log line per datagram.
        case RoutingLostMessage:
        {
            // 03_08_05 5.3 p.16: struct length, DeviceState, NumberOfLostMessages (2 octets).
            // A truncated frame is ignored (03_08_02 6.2); our own report, if looped back, is not counted.
            const uint32_t ownAddr = _platform.currentIpAddress();
            if (_counters != nullptr && len >= KNXIP_HEADER_LEN + 4 && remoteAddr != htonl(ownAddr))
                _counters->incrementLostMessageRx(getWord(buffer + KNXIP_HEADER_LEN + 2));
            break;
        }
        case RoutingBusy:
#ifdef KNX_IS_ROUTER
            handleRoutingBusy(buffer, (uint16_t)len, remoteAddr);
#else
            // 03_08_05 5.4 Figure 8 p.17: header + 6 (03_02_06 Figure 4 shows + 4, its own fields add to 6); a
            // shorter frame is ignored. Holding back own ROUTING_INDICATIONs is implemented for the router only.
            if (_counters != nullptr && len >= KNXIP_HEADER_LEN + 6)
                _counters->incrementBusyRx();
#endif
            break;
        default:
        {
#ifdef KNX_TUNNELING
            if(!_ipTunnelServer.HandleIpFrame(buffer, len, remoteAddr, remotePort))
#endif
            {
                print("Unhandled KNX-IP service identifier: ");
                println(code, HEX);
            }
            break;
        }

    }
    return true;
}

#if KNX_SERVICE_FAMILY_CORE >= 2
void IpDataLinkLayer::loopHandleSearchRequestExtended(uint8_t* buffer, uint16_t length)
{
    KnxIpSearchRequestExtended searchRequest(buffer, length);
    if (searchRequest.hpai().code() != IPV4_UDP) return;

    if(searchRequest.srpByProgMode)
    {
        println("srpByProgMode");
        if(!knx.progMode()) return;
    }

    if(searchRequest.srpByMacAddr)
    {
        println("srpByMacAddr");
        // PID_MAC_ADDRESS is a CallbackProperty and keeps no data block, so propertyData() has nothing to
        // hand out - the old unchecked cast compared the requested MAC against six octets of code. Read it
        // through the property and answer nothing when it cannot be read.
        uint8_t mac[6] = {0};
        uint8_t macCount = 1;
        _ipParameters.readProperty(PID_MAC_ADDRESS, 1, macCount, mac);
        if (macCount == 0)
            return;
        for(int i = 0; i<6;i++)
            if(searchRequest.srpMacAddr[i] != mac[i])
                return;
    }

    //defaults: "Device Information DIB", "Extended Device Information DIB" and "Supported Services DIB".
    int dibLength = LEN_DEVICE_INFORMATION_DIB + LEN_SUPP_SVC_DIB_EXT + LEN_EXTENDED_DEVICE_INFORMATION_DIB;

    if(searchRequest.srpByService)
    {
        println("srpByService");
        uint8_t length = searchRequest.srpServiceFamilies[0];
        uint8_t *currentPos = searchRequest.srpServiceFamilies + 2;
        for(int i = 0; i < (length-2)/2; i++)
        {
            uint8_t serviceFamily = (currentPos + i*2)[0];
            uint8_t version = (currentPos + i*2)[1];
            switch(serviceFamily)
            {
                case Core:
                    if(version > KNX_SERVICE_FAMILY_CORE) return;
                    break;
                case DeviceManagement:
                    if(version > KNX_SERVICE_FAMILY_DEVICE_MANAGEMENT) return;
                    break;
                case Tunnelling:
                    if(version > KNX_SERVICE_FAMILY_TUNNELING) return;
                    break;
                case Routing:
                    if(version > KNX_SERVICE_FAMILY_ROUTING) return;
                    break;
            }
        }
    }

    if(searchRequest.srpRequestDIBs)
    {
        //println("srpRequestDIBs");
        if(searchRequest.requestedDIB(IP_CONFIG))
            dibLength += LEN_IP_CONFIG_DIB; //16

        if(searchRequest.requestedDIB(IP_CUR_CONFIG))
            dibLength += LEN_IP_CURRENT_CONFIG_DIB; //20

        if(searchRequest.requestedDIB(KNX_ADDRESSES))
        {uint16_t length = 0;
            _ipParameters.readPropertyLength(PID_ADDITIONAL_INDIVIDUAL_ADDRESSES, length);
            dibLength += 4 + length*2;
        }

        if(searchRequest.requestedDIB(MANUFACTURER_DATA))
            dibLength += 0; //4 + n

        if(searchRequest.requestedDIB(TUNNELING_INFO))
        {
            uint16_t length = 0;
            _ipParameters.readPropertyLength(PID_ADDITIONAL_INDIVIDUAL_ADDRESSES, length);
            dibLength += 4 + length*4;
        }
    }

    KnxIpSearchResponseExtended searchResponse(_ipParameters, _deviceObject, dibLength);

    searchResponse.setDeviceInfo(_ipParameters, _deviceObject); //DescriptionTypeCode::DeviceInfo 1
    searchResponse.setSupportedServices(); //DescriptionTypeCode::SUPP_SVC_FAMILIES 2
    // DIB APDU limits from PID 68/69; an unwired server keeps the encoder default.
    uint16_t busApduLength = MAX_APDU_OCTET_COUNT;
    uint16_t localApduLength = MAX_APDU_OCTET_COUNT;
#ifdef USE_CEMI_SERVER
    if (_cemiServerObject != nullptr)
    {
        // property() answers nullptr for a PID the object does not carry - fall back, do not crash.
        Property* busProp = _cemiServerObject->property(PID_MAX_INTERFACE_APDU_LENGTH);
        Property* localProp = _cemiServerObject->property(PID_MAX_LOCAL_APDU_LENGTH);
        if (busProp != nullptr) busProp->read(busApduLength);
        if (localProp != nullptr) localProp->read(localApduLength);
    }
#endif

    searchResponse.setExtendedDeviceInfo(localApduLength); //DescriptionTypeCode::EXTENDED_DEVICE_INFO 8

    if(searchRequest.srpRequestDIBs)
    {
        if(searchRequest.requestedDIB(IP_CONFIG))
            searchResponse.setIpConfig(_ipParameters);

        if(searchRequest.requestedDIB(IP_CUR_CONFIG))
            searchResponse.setIpCurrentConfig(_ipParameters);

        if(searchRequest.requestedDIB(KNX_ADDRESSES))
            searchResponse.setKnxAddresses(_ipParameters, _deviceObject);

        if(searchRequest.requestedDIB(MANUFACTURER_DATA))
        {
            //println("requested MANUFACTURER_DATA but not implemented");
        }

        if(searchRequest.requestedDIB(TUNNELING_INFO))
            searchResponse.setTunnelingInfo(_ipParameters, _deviceObject, tunnels, busApduLength);
    }

    if(searchResponse.totalLength() > 500)
    {
        printf("skipped response length > 500. Length: %d bytes\n", searchResponse.totalLength());
        return;
    }

    sendControlUniCast(searchRequest.hpai().ipAddress(), searchRequest.hpai().ipPortNumber(), searchResponse.data(), searchResponse.totalLength());
}
#endif



// The system setup group is where every IP system broadcast goes, whatever the routing group is set to
// (03_02_06 4.1.3). Sending to a multicast address needs no membership -- only receiving does -- so the
// unicast API, which is the only one carrying a destination, does the job here.
// The 50/s limit of 03_02_06 2.1 p.6 covers ROUTING_INDICATIONs only: a ROUTING_SYSTEM_BROADCAST is neither refused nor counted by it.
bool IpDataLinkLayer::sendSystemBroadcast(CemiFrame& frame)
{
    KnxIpRoutingSystemBroadcast packet(frame);
    const bool success = sendUniCastCounted(systemSetupMultiCastAddress(), KNXIP_MULTICAST_PORT,
                                            packet.data(), packet.totalLength());
    dataConReceived(frame, success);
    return success;
}

// Opened only while the router holds the mode enabled (it falls back after 20 s), and only where the platform keeps
// it apart from the routing socket. Where the routing socket takes the system setup group itself it is not needed.
void IpDataLinkLayer::enableSystemBroadcast(bool value)
{
    if (value && (_setupOnRoutingSocket || !_platform.multiCastSecondaryIsolated()))
        value = false;
    if (!value)
        _sbcOpenFailed = false;
    if (value == _sbcSocketOpen || (value && _sbcOpenFailed))
        return; // one attempt per mode activation: a refused open must not repeat every pass

    if (!value)
    {
        _platform.closeMultiCastSecondary();
        _sbcSocketOpen = false;
        return;
    }

    if (!_enabled)
        return;

    _sbcSocketOpen = _platform.setupMultiCastSecondary(systemSetupMultiCastAddress(), KNXIP_MULTICAST_PORT);

    if (!_sbcSocketOpen)
    {
        _sbcOpenFailed = true;
        println("IP system broadcast: the system setup group could not be joined");
    }
}

// Only where the platform isolates a second socket (multiCastSecondaryIsolated()); RP2040 and ESP32 use the routing socket.
void IpDataLinkLayer::loopSystemBroadcast()
{
    if (!_sbcSocketOpen)
        return;

    uint8_t buffer[KNXIP_RX_MAX_DATAGRAM];
    int len = _platform.readBytesMultiCastSecondary(buffer, sizeof(buffer));
    receivedSystemBroadcast(buffer, len);
}

void IpDataLinkLayer::changeRoutingGroup(uint32_t group)
{
    if (!_enabled)
    {
        _joinedGroup = group; // joined with the endpoint
        _groupChangePending = false;
        return;
    }
    _pendingGroup = group;
    _groupChangePending = true;
}

void IpDataLinkLayer::receivedSystemBroadcast(uint8_t* buffer, int len)
{
    if (len < KNXIP_HEADER_LEN)
        return;

    if (buffer[0] != KNXIP_HEADER_LEN || buffer[1] != KNXIP_PROTOCOL_VERSION)
        return;

    uint16_t declaredLen;
    popWord(declaredLen, buffer + 4);
    if (declaredLen != (uint16_t)len)
        return;

    uint16_t code;
    popWord(code, buffer + 2);
    if ((KnxIpServiceType)code != RoutingSystemBroadcast)
        return; // every other service is served on the routing socket

    KnxIpRoutingSystemBroadcast sbc(buffer, len);

    // Order as on the routing path: a zero-length frame must not reach valid(), which would read past it.
    if (sbc.frame().totalLenght() == 0 || !sbc.frame().valid())
        return;

    // 03_02_06 4.1.3: a frame whose cEMI does not meet the conditions shall be ignored.
    if (!sbc.cemiIsSystemBroadcast())
        return;

    _indicatingSbc = true;
    frameReceived(sbc.frame());
    _indicatingSbc = false;
}

uint32_t IpDataLinkLayer::systemSetupMultiCastAddress()
{
    return _ipParameters.propertyValue<uint32_t>(PID_SYSTEM_SETUP_MULTICAST_ADDRESS);
}

uint32_t IpDataLinkLayer::multiCastAddress()
{
#ifdef KNX_IS_ROUTER
    return _ipParameters.propertyValue<uint32_t>(PID_ROUTING_MULTICAST_ADDRESS);
#else
    // Non-routing device: the fixed system-setup multicast (PID 65) so SEARCH_REQUEST still reaches us.
    // Only one group is joined, so moving 57B0 to PID 66 would cut it off from SEARCH_REQUEST as soon as
    // ETS configures a non-default routing multicast.
    return _ipParameters.propertyValue<uint32_t>(PID_SYSTEM_SETUP_MULTICAST_ADDRESS);
#endif
}

uint32_t IpDataLinkLayer::usableGroup(uint32_t group)
{
    const uint8_t firstOctet = (uint8_t)(group >> 24);
    const bool linkLocal = (group >> 8) == 0xE00000; // 224.0.0.0/24: lwIP refuses these
    if (firstOctet < 224 || firstOctet > 239 || linkLocal)
        return 0xE000170C; // 224.0.23.12, 03_08_02 8.5.2.1
    return group;
}

void IpDataLinkLayer::reinitialise()
{
    // 03_05_02 3.7.1.1.1: PID 67 and PID 66 apply after the restart.
    // Re-joins for a changed group, or for a changed TTL where the platform sets it at the join only.
    const uint8_t ttl = readTtl();
    bool rejoin = false;
#ifdef KNX_IS_ROUTER
    rejoin = (ttl != _ttl) && _platform.multicastTtlAtJoinOnly(); // only a router sends multicast
#endif
    _ttl = ttl;
    _platform.multicastTtl(ttl);
    const uint32_t group = usableGroup(multiCastAddress());
    if (group != _joinedGroup || rejoin)
        changeRoutingGroup(group);
}

uint8_t IpDataLinkLayer::readTtl()
{
    uint8_t ttl = 16; // stays the default if the property holds no element
    const Property* ttlProp = _ipParameters.property(PID_TTL);
    if (ttlProp != nullptr)
        ttlProp->read(ttl);
    return ttl;
}

bool IpDataLinkLayer::joinMultiCast()
{
    if (!_platform.setupMultiCast(_joinedGroup, KNXIP_MULTICAST_PORT))
        return false;
#ifdef KNX_IS_ROUTER
    // 03_02_06 4.3.5.2: IP system broadcasts and discovery come on the system setup group whatever the routing
    // group is. Joined on the routing socket where the platform can tell the groups apart per datagram.
    const uint32_t setup = systemSetupMultiCastAddress();
    _setupGroup = setup;
    _setupOnRoutingSocket = (_joinedGroup == setup) || _platform.joinMultiCastExtra(setup);
    if (!_setupOnRoutingSocket)
        println("routing group differs from the system setup group, which is not joined: IP system broadcast and discovery requests on it are not received");
#endif
    return true;
}

/** @brief Rebuild the endpoint after the IP interface changed. Also re-opens unicast where both share a socket. */
// Returns false only when a rebuild was attempted and failed, so the caller can retry; "nothing to
// rebuild" and "address not joinable, socket kept" are both an intact endpoint.
bool IpDataLinkLayer::networkChanged(bool afterOutage)
{
    // No endpoint at all -- including a join that failed at boot. Always try, outage or not: without this
    // the device would keep link and address but answer no SEARCH_REQUEST for the rest of its runtime.
    if (!_enabled)
    {
        // On RP2040 setupMultiCast() joins before it binds and never rolls the join back, so a previous attempt may
        // still hold the membership. Leave first, else every retry only bumps lwIP's use counter silently.
        _platform.closeMultiCast();
        enabled(true);
        return _enabled;
    }

    if (!afterOutage) return true; // endpoint is fresh; a needless Leave prunes the group on a switch

    // Same for the system broadcast socket: it was bound to the interface that just went away. Dropping
    // it here lets the next loop pass rebuild it while the mode is still on.
    enableSystemBroadcast(false);
    _platform.closeMultiCast();
    _enabled = false;
#ifdef KNX_IS_ROUTER
    flushTxQueue();
#endif
    _enabled = joinMultiCast(); // the socket is gone if this failed; enabled() must not claim otherwise
    return _enabled;
}

void IpDataLinkLayer::enabled(bool value)
{
//    _print("own address: ");
//    _println(_deviceObject.individualAddress());
    if (value && !_enabled)
    {
        // 03_08_03 2.5.17: a runtime write to PID 66 becomes active on reset, so read the property once
        // per device lifetime. An unusable value falls back to the default instead of leaving no endpoint.
        if (_joinedGroup == 0)
            _joinedGroup = usableGroup(multiCastAddress());
        // 03_08_03 2.5.18: PID 67 is the TTL of every multicast send; read per enable, kept across re-joins.
        const uint8_t ttl = readTtl();
        if (ttl == 0)
            println("PID_TTL is 0: multicast datagrams are sent with TTL 0");
        _ttl = ttl;
        _platform.multicastTtl(ttl);
        _enabled = joinMultiCast();        // no endpoint, no enabled(): enabled() must not report a lie
        return;
    }

    if(!value && _enabled)
    {
        // The system broadcast socket goes with it: the mode may still be on, and the next loop pass
        // re-opens it once the endpoint is back. Without this it would survive as a stale handle.
        enableSystemBroadcast(false);
        _platform.closeMultiCast();
        _enabled = false;
#ifdef KNX_IS_ROUTER
        flushTxQueue();
#endif
        return;
    }
}

bool IpDataLinkLayer::enabled() const
{
    return _enabled;
}

DptMedium IpDataLinkLayer::mediumType() const
{
    return DptMedium::KNX_IP;
}

bool IpDataLinkLayer::sendUniCastCounted(uint32_t addr, uint16_t port, uint8_t* buffer, uint16_t len)
{
    const bool sent = _platform.sendBytesUniCast(addr, port, buffer, len);
    if (_counters != nullptr)
    {
        if (sent)
            _counters->incrementTransmitToIp();
        else
            _counters->incrementOverflowToIp();
    }
    return sent;
}

// A control datagram (SEARCH responses) is no KNX message: PID 74 when it leaves, never PID 72 (03_08_05 2.3.4).
bool IpDataLinkLayer::sendControlUniCast(uint32_t addr, uint16_t port, uint8_t* buffer, uint16_t len)
{
    const bool sent = _platform.sendBytesUniCast(addr, port, buffer, len);
    if (sent && _counters != nullptr)
        _counters->incrementTransmitToIp();
    return sent;
}

#ifdef KNX_IS_ROUTER
// ROUTING_BUSY, ROUTING_LOST_MESSAGE: counted as sendBytes() counts a sent datagram, never PID 72. Callers hold _enabled.
bool IpDataLinkLayer::sendControlMultiCast(uint8_t* bytes, uint16_t length)
{
    const bool sent = _platform.sendBytesMultiCast(bytes, length);
    if (sent && _counters != nullptr && !(_ipParameters.deviceState() & IpParameterObject::DeviceStateIpFault))
        _counters->incrementTransmitToIp();
    return sent;
}
#endif

bool IpDataLinkLayer::sendBytes(uint8_t* bytes, uint16_t length, bool* held)
{
    if (!_enabled)
    {
        if (_counters != nullptr)
            _counters->incrementOverflowToIp(); // no IP endpoint: 03_08_05 7.3.3 case 3.1
        return false;
    }

    const bool sent = _platform.sendBytesMultiCast(bytes, length);
#ifdef KNX_IS_ROUTER
    // A ROUTING_INDICATION the network driver refused stays queued: the W5500 driver refuses it for a ROUTING_BUSY in its
    // receive buffer. Other refusals are counted; the KNX_ROUTING_SEND_RETRIES-th refused attempt counts it as lost.
    if (held != nullptr)
    {
        if (!sent)
        {
            _platform.pollReceive();
            *held = true;
            if (_platform.routingBusyPending())
                return false;
            if (++_sendRefusals < KNX_ROUTING_SEND_RETRIES)
            {
                _routingSentUs = micros();
                return false;
            }
            *held = false;
        }
        _sendRefusals = 0;
    }
#else
    (void)held;
#endif
    if (_counters != nullptr)
    {
        // A send can succeed with the link up but the IP network unreachable; while the IP fault bit (03_08_03 3.5.3) is set, count a loss.
        if (sent && !(_ipParameters.deviceState() & IpParameterObject::DeviceStateIpFault))
            _counters->incrementTransmitToIp();
        else
            _counters->incrementOverflowToIp();
    }
    return sent;
}

#ifdef KNX_IS_ROUTER
// 03_08_05 5.3 Figure 7 p.16. The first loss after a quiet second is reported at once, later ones are summed
// (08_TSSH 6.2.2 p.112: about one second apart). Not subject to the 50/s ROUTING_INDICATION limit.
void IpDataLinkLayer::loopLostMessage()
{
    if (_counters == nullptr || !_enabled)
        return;

    const uint32_t pending = _counters->lostToKnx() - _lostReported; // modulo 2^32, exact across the wrap
    if (pending == 0)
        return;

    const uint32_t now = millis();
    if (now - _lostSentAt < 1000)
        return;
    if (now - _routingSentAt < KNX_ROUTING_SLOW_GAP_MS) // 03_02_06 2.1 p.6: >= 5 ms after a ROUTING_INDICATION
        return;

    const uint8_t deviceState = _ipParameters.deviceState();
    if (deviceState & IpParameterObject::DeviceStateIpFault)
        return; // keep the count until the LAN is back

    const uint16_t count = pending > 0xFFFF ? 0xFFFF : (uint16_t)pending;
    uint8_t frame[KNXIP_HEADER_LEN + 4] = {KNXIP_HEADER_LEN, KNXIP_PROTOCOL_VERSION,
                                           (uint8_t)(RoutingLostMessage >> 8), (uint8_t)RoutingLostMessage,
                                           0x00, KNXIP_HEADER_LEN + 4,
                                           0x04, deviceState, (uint8_t)(count >> 8), (uint8_t)count};
    _lostSentAt = now;
    if (sendControlMultiCast(frame, sizeof(frame)))
        _lostReported += count;
}
#endif

bool IpDataLinkLayer::isSendLimitReached()
{
    // The 51st datagram is refused while the oldest of the last 50 is under 1005 ms old; 5 ms for millis() steps and send-path jitter.
    const uint32_t now = millis();
    if (sendLimitPeek(now))
    {
        println("Dropping packet due to 50p/s limit");
        return true;   // drop packet
    }
    sendLimitRecord(now);
    return false;
}

bool IpDataLinkLayer::sendLimitPeek(uint32_t now) const
{
    return _sendCount >= kSendLimit && (uint32_t)(now - _sendTimes[_sendIdx]) < kSendWindowMs;
}

void IpDataLinkLayer::sendLimitRecord(uint32_t now)
{
    _sendTimes[_sendIdx] = now;
    _sendIdx = (_sendIdx + 1) % kSendLimit;
    if (_sendCount < kSendLimit)
        _sendCount++;
}
#endif

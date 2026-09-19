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
    // 03_02_06 4.1.3: a system broadcast leaves as ROUTING_SYSTEM_BROADCAST on the system setup group,
    // never as a routing indication. The coupler only marks a frame this way while the router has the IP
    // System Broadcast Routing Mode enabled, so this branch is unreachable in the delivery state.
    if (frame.systemBroadcast() == SysBroadcast)
        return sendSystemBroadcast(frame);

    KnxIpRoutingIndication packet(frame);
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
}



#ifdef OPENKNX_CON_DIAG
uint16_t g_bef3Drop = 0; // KNXnet/IP header total-length mismatch drops (diag); read by ip_tunnel_server
#endif

void IpDataLinkLayer::loop()
{
    if (!_enabled)
        return;


    loopSystemBroadcast();

    uint8_t buffer[512];
    uint16_t remotePort = 0;
    uint32_t remoteAddr = 0;
    int len = _platform.readBytesMultiCast(buffer, 512, remoteAddr, remotePort);
    if (len <= 0)
        return;

    if (len < KNXIP_HEADER_LEN)
        return;
    
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
        return;
    }

    // KNXnet/IP total length (octets 4-5) must equal the received datagram; a mismatch is malformed ->
    // discard (TSSH 3.2.4/3.2.5, both refs do). Well-formed short frames (declared==len) still pass.
    uint16_t declaredLen;
    popWord(declaredLen, buffer + 4);
    if (declaredLen != (uint16_t)len)
    {
#ifdef OPENKNX_CON_DIAG
        g_bef3Drop++;
#endif
        return;
    }

#ifdef KNX_ACTIVITYCALLBACK
    if(_dllcb)
        _dllcb->activity((_netIndex << KNX_ACTIVITYCALLBACK_NET) | (KNX_ACTIVITYCALLBACK_DIR_RECV << KNX_ACTIVITYCALLBACK_DIR));
#endif

    uint16_t code;
    popWord(code, buffer + 2);
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
                frameReceived(routingIndication.frame());
            break;
        }

        case RoutingSystemBroadcast:
            // 03_02_06 4.1.3: valid only when received on the system setup multicast address, which the
            // dedicated socket in loopSystemBroadcast() owns. Whatever arrives here came in on the routing
            // group or as unicast and shall be ignored.
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
            sendUniCastCounted(hpai.ipAddress(), hpai.ipPortNumber(), searchResponse.data(), searchResponse.totalLength());
            break;
        }
        case SearchRequestExt:
        {
            #if KNX_SERVICE_FAMILY_CORE >= 2
            loopHandleSearchRequestExtended(buffer, len);
            #endif
            break;
        }
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

    //defaults: "Device Information DIB", "Extended Device Information DIB" and "Supported Services DIB".
    int dibLength = LEN_DEVICE_INFORMATION_DIB + LEN_SERVICE_DIB + LEN_EXTENDED_DEVICE_INFORMATION_DIB;

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
    searchResponse.setExtendedDeviceInfo(); //DescriptionTypeCode::EXTENDED_DEVICE_INFO 8

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
            searchResponse.setTunnelingInfo(_ipParameters, _deviceObject, tunnels);
    }

    if(searchResponse.totalLength() > 500)
    {
        printf("skipped response length > 500. Length: %d bytes\n", searchResponse.totalLength());
        return;
    }

    sendUniCastCounted(searchRequest.hpai().ipAddress(), searchRequest.hpai().ipPortNumber(), searchResponse.data(), searchResponse.totalLength());
}
#endif



// The system setup group is where every IP system broadcast goes, whatever the routing group is set to
// (03_02_06 4.1.3). Sending to a multicast address needs no membership -- only receiving does -- so the
// unicast API, which is the only one carrying a destination, does the job here.
bool IpDataLinkLayer::sendSystemBroadcast(CemiFrame& frame)
{
    if (isSendLimitReached())
    {
        if (_counters != nullptr)
            _counters->incrementOverflowToIp();
        dataConReceived(frame, false);
        return false;
    }

    KnxIpRoutingSystemBroadcast packet(frame);
    const bool success = sendUniCastCounted(systemSetupMultiCastAddress(), KNXIP_MULTICAST_PORT,
                                            packet.data(), packet.totalLength());
    dataConReceived(frame, success);
    return success;
}

// Opened only while the router holds the mode enabled, and that falls back after 20 s: a device that
// never uses system broadcast keeps one socket, as before.
void IpDataLinkLayer::enableSystemBroadcast(bool value)
{
    if (value == _sbcSocketOpen)
        return;

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
        println("IP system broadcast: the system setup group could not be joined");
}

// A second socket instead of the arrival address of the datagram: the destination is not available on
// every platform, and a group of its own makes "received on the system setup multicast address"
// structural rather than a runtime comparison.
void IpDataLinkLayer::loopSystemBroadcast()
{
    if (!_sbcSocketOpen)
        return;

    uint8_t buffer[512];
    int len = _platform.readBytesMultiCastSecondary(buffer, 512);

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

    frameReceived(sbc.frame());
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

bool IpDataLinkLayer::joinMultiCast()
{
    return _platform.setupMultiCast(_joinedGroup, KNXIP_MULTICAST_PORT);
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
        // beginMulticast() joins before it binds and never rolls the join back, so a previous attempt may
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
        {
            _joinedGroup = multiCastAddress();
            const uint8_t firstOctet = (uint8_t)(_joinedGroup >> 24);
            const bool linkLocal = (_joinedGroup >> 8) == 0xE00000; // 224.0.0.0/24: lwIP refuses these
            if (firstOctet < 224 || firstOctet > 239 || linkLocal)
                _joinedGroup = 0xE000170C; // 224.0.23.12, 03_08_02 8.5.2.1
        }
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

bool IpDataLinkLayer::sendBytes(uint8_t* bytes, uint16_t length)
{
    if (!_enabled)
        return false;

    const bool sent = _platform.sendBytesMultiCast(bytes, length);
    if (_counters != nullptr)
    {
        if (sent)
            _counters->incrementTransmitToIp();
        else
            _counters->incrementOverflowToIp();
    }
    return sent;
}

bool IpDataLinkLayer::isSendLimitReached()
{
    uint32_t curTime = millis() / 100;

    // Forward tick versus millis overflow; the ring is ten 100 ms buckets bounding 03_02_06 2.1 (50/s).
    if(curTime > _frameCountTimeBase)
    {
        uint32_t timeBaseDiff = curTime - _frameCountTimeBase;
        if(timeBaseDiff > 10)
            timeBaseDiff = 10;
        for(uint32_t i = 0; i < timeBaseDiff ; i++)
        {
            _frameCountBase++;
            _frameCountBase = _frameCountBase % 10;
            _frameCount[_frameCountBase] = 0;
        }
        _frameCountTimeBase = curTime;
    }
    else if(curTime < _frameCountTimeBase) // millis overflow, reset
    {
        for(int i = 0; i < 10 ; i++)
            _frameCount[i] = 0;
        _frameCountBase = 0;
        _frameCountTimeBase = curTime;
    }

    //check if we are over the limit
    uint16_t sum = 0;
    for(int i = 0; i < 10 ; i++)
        sum += _frameCount[i];
    if(sum > 50)
    {
        println("Dropping packet due to 50p/s limit");
        return true;   // drop packet
    }
    else
    {
        _frameCount[_frameCountBase]++;
        //print("sent packages in last 1000ms: ");
        //print(sum);
        //print(" curTime: ");
        //println(curTime);
        return false;
    }
}
#endif

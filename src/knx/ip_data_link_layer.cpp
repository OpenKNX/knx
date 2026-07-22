#include "config.h"
#ifdef USE_IP

#include "ip_data_link_layer.h"

#include "bits.h"
#include "platform.h"
#include "device_object.h"
#include "knx_ip_routing_indication.h"
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
    KnxIpRoutingIndication packet(frame);
    // only send 50 packet per second: see KNX 3.2.6 p.6
    if(isSendLimitReached())
    {
#ifdef KNX_FIXES_EC
        // Every other sendFrame reports a dataConReceived (see the success path below).
        // The send-limit drop was the only branch returning false WITHOUT one, leaving the upper
        // layer waiting for a confirmation that never arrives. Emit the (negative) con to close that gap.
        dataConReceived(frame, false);
#endif
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



void IpDataLinkLayer::loop()
{
    if (!_enabled)
        return;


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
        return;

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
#ifdef KNX_FIXES_EC
            // Validate the inbound multicast cEMI BEFORE forwarding it to TP -- a malformed routing
            // frame (e.g. a 0-length cEMI from ETS) is otherwise pushed onto the bus as garbage / can crash.
            // The constructor does NOT validate (it even derives offsets from the untrusted data[1]), so THIS
            // is the check. Order is load-bearing: totalLenght()!=0 must be first -- valid()'s bounds-check
            // only self-bounds for _length!=0, so the short-circuit keeps valid() from ever OOB-reading a
            // zero-length frame.
            if (routingIndication.frame().totalLenght() != 0 && routingIndication.frame().valid())
                frameReceived(routingIndication.frame());
#else
            frameReceived(routingIndication.frame());
#endif
            break;
        }
        
        case SearchRequest:
        {
            KnxIpSearchRequest searchRequest(buffer, len);
            KnxIpSearchResponse searchResponse(_ipParameters, _deviceObject);

            auto hpai = searchRequest.hpai();
#ifdef KNX_ACTIVITYCALLBACK
            if(_dllcb)
                _dllcb->activity((_netIndex << KNX_ACTIVITYCALLBACK_NET) | (KNX_ACTIVITYCALLBACK_DIR_SEND << KNX_ACTIVITYCALLBACK_DIR) | (KNX_ACTIVITYCALLBACK_IPUNICAST));
#endif
            _platform.sendBytesUniCast(hpai.ipAddress(), hpai.ipPortNumber(), searchResponse.data(), searchResponse.totalLength());
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

    if(searchRequest.srpByProgMode)
    {
        println("srpByProgMode");
        if(!knx.progMode()) return;
    }

    if(searchRequest.srpByMacAddr)
    {
        println("srpByMacAddr");
        const uint8_t *x = _ipParameters.propertyData(PID_MAC_ADDRESS);
        for(int i = 0; i<6;i++)
            if(searchRequest.srpMacAddr[i] != x[i])
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

    _platform.sendBytesUniCast(searchRequest.hpai().ipAddress(), searchRequest.hpai().ipPortNumber(), searchResponse.data(), searchResponse.totalLength());
}
#endif



void IpDataLinkLayer::enabled(bool value)
{
//    _print("own address: ");
//    _println(_deviceObject.individualAddress());
    if (value && !_enabled)
    {
        _platform.setupMultiCast(_ipParameters.propertyValue<uint32_t>(PID_ROUTING_MULTICAST_ADDRESS), KNXIP_MULTICAST_PORT);
        _enabled = true;
        return;
    }

    if(!value && _enabled)
    {
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

bool IpDataLinkLayer::sendBytes(uint8_t* bytes, uint16_t length)
{
    if (!_enabled)
        return false;

    return _platform.sendBytesMultiCast(bytes, length);
}

bool IpDataLinkLayer::isSendLimitReached()
{
    uint32_t curTime = millis() / 100;

    // check if the countbuffer must be adjusted
    if(_frameCountTimeBase >= curTime)
    {
        uint32_t timeBaseDiff = _frameCountTimeBase - curTime;
        if(timeBaseDiff > 10)
            timeBaseDiff = 10;
        for(int i = 0; i < timeBaseDiff ; i++)
        {
            _frameCountBase++;
            _frameCountBase = _frameCountBase % 10;
            _frameCount[_frameCountBase] = 0;
        }
        _frameCountTimeBase = curTime;
    }
    else // _frameCountTimeBase < curTime => millis overflow, reset
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

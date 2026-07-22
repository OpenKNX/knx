#include "config.h"
#ifdef KNX_TUNNELING

#include "ip_tunnel_server.h"

#include "cemi_server.h"
#include "knx_ip_config_request.h"
#include "knx_ip_connect_request.h"
#include "knx_ip_connect_response.h"
#include "knx_ip_description_request.h"
#include "knx_ip_description_response.h"
#include "knx_ip_disconnect_request.h"
#include "knx_ip_disconnect_response.h"
#include "knx_ip_state_request.h"
#include "knx_ip_state_response.h"
#include "knx_ip_tunneling_ack.h"
#include "knx_ip_tunneling_request.h"

#ifdef OPENKNX_HW_BUSMON
#include <string.h>
#endif

IpTunnelServer::IpTunnelServer(DeviceObject& devObj, IpParameterObject& ipParam, Platform& platform, CemiServer& cemiServer) : _deviceObject(devObj),
                                                                                                                               _ipParameters(ipParam),
                                                                                                                               _platform(platform),
                                                                                                                               _cemiServer(cemiServer)
{
}

void IpTunnelServer::loop()
{
    for (int i = 0; i < KNX_TUNNELING + KNX_TUNNELING_DEVMGMT; i++)
    {
        if (tunnels[i].ChannelId != 0)
        {
            if (millis() - tunnels[i].lastHeartbeat > 120000)
            {
#ifdef KNX_LOG_TUNNELING
                print("Closed Tunnel 0x");
                print(tunnels[i].ChannelId, 16);
                println(" due to no heartbeat in 2 minutes");
#endif
                KnxIpDisconnectRequest discReq;
                discReq.channelId(tunnels[i].ChannelId);
                discReq.hpaiCtrl().length(LEN_IPHPAI);
                discReq.hpaiCtrl().code(IPV4_UDP);
                discReq.hpaiCtrl().ipAddress(tunnels[i].IpAddress);
                discReq.hpaiCtrl().ipPortNumber(tunnels[i].PortCtrl);
                _platform.sendBytesUniCast(tunnels[i].IpAddress, tunnels[i].PortCtrl, discReq.data(), discReq.totalLength());
                tunnels[i].Reset();
            }
#ifndef KNX_FIXES_EC
            break; // stock behaviour (stops the scan at the first OCCUPIED slot)
#endif
            // the slot-level break above is removed so loop() scans ALL slots and reaps every expired one.
            // the old break stopped at the first OCCUPIED slot, leaving dead tunnels in later slots unreaped -> slot exhaustion.
        }
    }

#ifdef OPENKNX_HW_BUSMON
    // Busmon self-heal safety-net: if ETS vanishes, the heartbeat times out -> leave monitor mode so
    // routing is never permanently stuck off (plan 5b.3).
    if (_busMonTunnel.ChannelId != 0 && millis() - _busMonTunnel.lastHeartbeat > 120000)
    {
        println("HW-Busmon: no heartbeat in 2 minutes -> leaving monitor mode");
        KnxIpDisconnectRequest discReq;
        discReq.channelId(_busMonTunnel.ChannelId);
        discReq.hpaiCtrl().length(LEN_IPHPAI);
        discReq.hpaiCtrl().code(IPV4_UDP);
        discReq.hpaiCtrl().ipAddress(_busMonTunnel.IpAddress);
        discReq.hpaiCtrl().ipPortNumber(_busMonTunnel.PortCtrl);
        _platform.sendBytesUniCast(_busMonTunnel.IpAddress, _busMonTunnel.PortCtrl, discReq.data(), discReq.totalLength());
        busMonitorTeardown();
    }

    // Bounded, non-blocking exit recovery: poll the chip back to CONNECTED after leaving monitor mode.
    if (_busMonExitPending)
    {
        if (_hwBusMon && _hwBusMon->hwBusMonConnected())
        {
            _busMonExitPending = false; // routing restored
        }
        else if (millis() - _busMonExitStart > 3000)
        {
            _busMonExitPending = false;
            if (_hwBusMon)
                _hwBusMon->hwBusMonExit(); // one more reset; NCN may need a power-cycle (plan 6)
            println("HW-Busmon: chip did not return to CONNECTED within 3s (possible NCN latch, see plan 6)");
        }
    }
#endif
}

void IpTunnelServer::dataRequestToChannelId(CemiFrame& frame, uint8_t channelId)
{
    KnxIpTunnelConnection* tun = nullptr;
    for (int i = 0; i < KNX_TUNNELING + KNX_TUNNELING_DEVMGMT; i++)
    {
#ifdef KNX_LOG_TUNNELING
        print("Tunnel ChannelId: ");
#endif
        if(tunnels[i].IsConfig)
        {
#ifdef KNX_LOG_TUNNELING
            print("Config ");
#endif
        }
#ifdef KNX_LOG_TUNNELING
        println(tunnels[i].ChannelId, 16);
#endif
        if (tunnels[i].ChannelId == channelId)
        {
            tun = &tunnels[i];
            break;
        }
    }

    if (tun == nullptr)
    {
#ifdef KNX_LOG_TUNNELING
        print("Found no Tunnel for ChannelId: ");
        println(channelId, 16);
#endif
        return;
    }

    sendFrameToTunnel(tun, frame);
}

void IpTunnelServer::dataRequestToTunnel(CemiFrame& frame)
{
    if (frame.addressType() == AddressType::GroupAddress)
    {
        for (int i = 0; i < KNX_TUNNELING; i++)
            if (tunnels[i].ChannelId != 0 && tunnels[i].IndividualAddress == frame.sourceAddress())
                sendFrameToTunnel(&tunnels[i], frame);
        // TODO check if source is from tunnel
        return;
    }

    KnxIpTunnelConnection* tun = nullptr;
    for (int i = 0; i < KNX_TUNNELING; i++)
    {
        if (tunnels[i].IndividualAddress == frame.sourceAddress())
            continue;

        if (tunnels[i].IndividualAddress == frame.destinationAddress())
        {
            tun = &tunnels[i];
            break;
        }
    }

    if (tun == nullptr)
    {
        for (int i = 0; i < KNX_TUNNELING; i++)
        {
            if (tunnels[i].IsConfig)
            {
#ifdef KNX_LOG_TUNNELING
                println("Found config Channel");
#endif
                tun = &tunnels[i];
                break;
            }
        }
    }

    if (tun == nullptr)
    {
#ifdef KNX_LOG_TUNNELING
        print("Found no Tunnel for IA: ");
        println(frame.destinationAddress(), 16);
#endif
        return;
    }

    sendFrameToTunnel(tun, frame);
}

void IpTunnelServer::dataConfirmationToTunnel(CemiFrame& frame)
{
    if (frame.addressType() == AddressType::GroupAddress)
    {
        for (int i = 0; i < KNX_TUNNELING; i++)
            if (tunnels[i].ChannelId != 0 && tunnels[i].IndividualAddress == frame.sourceAddress())
                sendFrameToTunnel(&tunnels[i], frame);
        // TODO check if source is from tunnel
        return;
    }

    KnxIpTunnelConnection* tun = nullptr;
    for (int i = 0; i < KNX_TUNNELING; i++)
    {
        if (tunnels[i].IndividualAddress == frame.destinationAddress())
            continue;

        if (tunnels[i].IndividualAddress == frame.sourceAddress())
        {
            tun = &tunnels[i];
            break;
        }
    }

    if (tun == nullptr)
    {
        for (int i = 0; i < KNX_TUNNELING; i++)
        {
            if (tunnels[i].IsConfig)
            {
#ifdef KNX_LOG_TUNNELING
                println("Found config Channel");
#endif
                tun = &tunnels[i];
                break;
            }
        }
    }

    if (tun == nullptr)
    {
#ifdef KNX_LOG_TUNNELING
        print("Found no Tunnel for IA: ");
        println(frame.destinationAddress(), 16);
#endif
        return;
    }

    sendFrameToTunnel(tun, frame);
}

void IpTunnelServer::dataIndicationToTunnel(CemiFrame& frame)
{
    if (frame.addressType() == AddressType::GroupAddress)
    {
        for (int i = 0; i < KNX_TUNNELING; i++)
            if (tunnels[i].ChannelId != 0 && tunnels[i].IndividualAddress != frame.sourceAddress())
                sendFrameToTunnel(&tunnels[i], frame);
        return;
    }

    KnxIpTunnelConnection* tun = nullptr;
    for (int i = 0; i < KNX_TUNNELING; i++)
    {
        if (tunnels[i].ChannelId == 0 || tunnels[i].IndividualAddress == frame.sourceAddress())
            continue;

        if (tunnels[i].IndividualAddress == frame.destinationAddress())
        {
            tun = &tunnels[i];
            break;
        }
    }

    if (tun == nullptr)
    {
        for (int i = 0; i < KNX_TUNNELING; i++)
        {
            if (tunnels[i].IsConfig)
            {
#ifdef KNX_LOG_TUNNELING
                println("Found config Channel");
#endif
                tun = &tunnels[i];
                break;
            }
        }
    }

    if (tun == nullptr)
    {
#ifdef KNX_LOG_TUNNELING
        print("Found no Tunnel for IA: ");
        println(frame.destinationAddress(), 16);
#endif
        return;
    }

    sendFrameToTunnel(tun, frame);
}

void IpTunnelServer::sendFrameToTunnel(KnxIpTunnelConnection* tunnel, CemiFrame& frame)
{
#ifdef KNX_LOG_TUNNELING
    print("Send to Channel: ");
    println(tunnel->ChannelId, 16);
#endif
    KnxIpTunnelingRequest req(frame);
    req.connectionHeader().sequenceCounter(tunnel->SequenceCounter_S++);
    req.connectionHeader().length(LEN_CH);
    req.connectionHeader().channelId(tunnel->ChannelId);

    if (frame.messageCode() != L_data_req && frame.messageCode() != L_data_con && frame.messageCode() != L_data_ind)
        req.serviceTypeIdentifier(DeviceConfigurationRequest);

    _platform.sendBytesUniCast(tunnel->IpAddress, tunnel->PortData, req.data(), req.totalLength());
}

bool IpTunnelServer::isTunnelAddress(uint16_t addr)
{
    if (addr == 0)
        return false; // 0.0.0 is not a valid tunnel address and is used as default value

    for (int i = 0; i < KNX_TUNNELING; i++)
        if (tunnels[i].IndividualAddress == addr)
            return true;

    return false;
}

bool IpTunnelServer::isSentToTunnel(uint16_t address, bool isGrpAddr)
{
    if (isGrpAddr)
    {
        for (int i = 0; i < KNX_TUNNELING; i++)
            if (tunnels[i].ChannelId != 0)
                return true;
        return false;
    }
    else
    {
        for (int i = 0; i < KNX_TUNNELING; i++)
            if (tunnels[i].ChannelId != 0 && tunnels[i].IndividualAddress == address)
                return true;
        return false;
    }
}

bool IpTunnelServer::HandleIpFrame(uint8_t* buffer, uint16_t length, uint32_t& src_addr, uint16_t& src_port)
{

    uint16_t code;
    popWord(code, buffer + 2);
    switch ((KnxIpServiceType)code)
    {
        case ConnectRequest: {
            HandleConnectRequest(buffer, length, src_addr, src_port);
            break;
        }

        case ConnectionStateRequest: {
            HandleConnectionStateRequest(buffer, length);
            break;
        }

        case DisconnectRequest: {
            HandleDisconnectRequest(buffer, length);
            break;
        }

        case DescriptionRequest: {
            HandleDescriptionRequest(buffer, length);
            break;
        }

        case DeviceConfigurationRequest: {
            HandleDeviceConfigurationRequest(buffer, length);
            break;
        }

        case TunnelingRequest: {
            HandleTunnelingRequest(buffer, length);
            break;
        }

        case DeviceConfigurationAck: {
            // TOOD nothing to do now
            // println("got Ack");
            break;
        }

        case TunnelingAck: {
            // TOOD nothing to do now
            // println("got Ack");
            break;
        }
        default:
            return false;
            break;
    }
    return true;
}

void IpTunnelServer::HandleConnectRequest(uint8_t* buffer, uint16_t length, uint32_t& src_addr, uint16_t& src_port)
{
    KnxIpConnectRequest connRequest(buffer, length);
#ifdef KNX_LOG_TUNNELING
    println("Got Connect Request!");
    switch (connRequest.cri().type())
    {
        case DEVICE_MGMT_CONNECTION:
            println("Device Management Connection");
            break;
        case TUNNEL_CONNECTION:
            println("Tunnel Connection");
            break;
        case REMLOG_CONNECTION:
            println("RemLog Connection");
            break;
        case REMCONF_CONNECTION:
            println("RemConf Connection");
            break;
        case OBJSVR_CONNECTION:
            println("ObjectServer Connection");
            break;
    }

    print("Data Endpoint: ");
    uint32_t ip = connRequest.hpaiData().ipAddress();
    print(ip >> 24);
    print(".");
    print((ip >> 16) & 0xFF);
    print(".");
    print((ip >> 8) & 0xFF);
    print(".");
    print(ip & 0xFF);
    print(":");
    println(connRequest.hpaiData().ipPortNumber());
    print("Ctrl Endpoint: ");
    ip = connRequest.hpaiCtrl().ipAddress();
    print(ip >> 24);
    print(".");
    print((ip >> 16) & 0xFF);
    print(".");
    print((ip >> 8) & 0xFF);
    print(".");
    print(ip & 0xFF);
    print(":");
    println(connRequest.hpaiCtrl().ipPortNumber());
#endif

    // We only support 0x03 and 0x04!
    if (connRequest.cri().type() != TUNNEL_CONNECTION && connRequest.cri().type() != DEVICE_MGMT_CONNECTION)
    {
#ifdef KNX_LOG_TUNNELING
        println("Only Tunnel/DeviceMgmt Connection ist supported!");
#endif
        KnxIpConnectResponse connRes(0x00, E_CONNECTION_TYPE);
        _platform.sendBytesUniCast(connRequest.hpaiCtrl().ipAddress(), connRequest.hpaiCtrl().ipPortNumber(), connRes.data(), connRes.totalLength());
        return;
    }

    if (connRequest.cri().type() == TUNNEL_CONNECTION && connRequest.cri().layer() != 0x02) // LinkLayer
    {
#ifdef OPENKNX_HW_BUSMON
        // KNX Busmonitor tunnelling layer (0x80): put the TP chip into real HW monitor mode.
        // NOTE (03_08_04 Tunnelling 2.2.4): busmonitor is not conformant on a routing device; this is a
        // build-flag opt-in and routing is physically paused (chip passive) for the busmon session only.
        if (connRequest.cri().layer() == 0x80)
        {
            HandleBusMonitorConnect(connRequest, src_addr, src_port);
            return;
        }
#endif
        // We only support 0x02!
#ifdef KNX_LOG_TUNNELING
        println("Only LinkLayer ist supported!");
#endif
        KnxIpConnectResponse connRes(0x00, E_TUNNELING_LAYER);
        _platform.sendBytesUniCast(connRequest.hpaiCtrl().ipAddress(), connRequest.hpaiCtrl().ipPortNumber(), connRes.data(), connRes.totalLength());
        return;
    }

    // data preparation

    uint32_t srcIP = connRequest.hpaiCtrl().ipAddress() ? connRequest.hpaiCtrl().ipAddress() : src_addr;
    uint16_t srcPort = connRequest.hpaiCtrl().ipPortNumber() ? connRequest.hpaiCtrl().ipPortNumber() : src_port;

    // read current elements in PID_ADDITIONAL_INDIVIDUAL_ADDRESSES
    uint16_t propCount = 0;
    _ipParameters.readPropertyLength(PID_ADDITIONAL_INDIVIDUAL_ADDRESSES, propCount);
    const uint8_t* addresses;
    if (propCount == KNX_TUNNELING)
    {
        addresses = _ipParameters.propertyData(PID_ADDITIONAL_INDIVIDUAL_ADDRESSES);
    }
    else // no tunnel PA configured, that means device is unconfigured and has 15.15.0
    {
        uint8_t addrbuffer[KNX_TUNNELING * 2];
        addresses = (uint8_t*)addrbuffer;
        for (int i = 0; i < KNX_TUNNELING; i++)
        {
            addrbuffer[i * 2 + 1] = i + 1;
            addrbuffer[i * 2] = _deviceObject.individualAddress() / 0x0100;
        }
        uint8_t count = KNX_TUNNELING;
        _ipParameters.writeProperty(PID_ADDITIONAL_INDIVIDUAL_ADDRESSES, 1, addrbuffer, count);
#ifdef KNX_FIXES_EC
        // re-point `addresses` at the property's LIVE storage: addrbuffer[] is block-scoped and would
        // dangle when read later (popWord(addresses + tunIdx*2)); propertyData() returns the persisted
        // copy we just wrote (same source the configured `if` branch above uses).
        addresses = _ipParameters.propertyData(PID_ADDITIONAL_INDIVIDUAL_ADDRESSES);
#endif
#ifdef KNX_LOG_TUNNELING
        println("no Tunnel-PAs configured, using own subnet");
#endif
    }

    _ipParameters.readPropertyLength(PID_CUSTOM_RESERVED_TUNNELS_CTRL, propCount);
    const uint8_t* tunCtrlBytes = nullptr;
    if (propCount == KNX_TUNNELING)
        tunCtrlBytes = _ipParameters.propertyData(PID_CUSTOM_RESERVED_TUNNELS_CTRL);

    _ipParameters.readPropertyLength(PID_CUSTOM_RESERVED_TUNNELS_IP, propCount);
    const uint8_t* tunCtrlIp = nullptr;
    if (propCount == KNX_TUNNELING)
        tunCtrlIp = _ipParameters.propertyData(PID_CUSTOM_RESERVED_TUNNELS_IP);

    bool resTunActive = (tunCtrlBytes && tunCtrlIp);
#ifdef KNX_LOG_TUNNELING
    if (resTunActive)
        println("Reserved Tunnel Feature active");

    if (tunCtrlBytes)
        printHex("tunCtrlBytes", tunCtrlBytes, KNX_TUNNELING);
    if (tunCtrlIp)
        printHex("tunCtrlIp", tunCtrlIp, KNX_TUNNELING * 4);
#endif

    uint8_t tunIdx = 0xff;
    if (connRequest.cri().type() == DEVICE_MGMT_CONNECTION)
    {
        for (int i = KNX_TUNNELING; i < KNX_TUNNELING + KNX_TUNNELING_DEVMGMT; i++)
        {
            if (tunnels[i].ChannelId == 0)
            {
                tunIdx = i;
                break;
            }
        }
        if (tunIdx == 0xff) 
            ; // Todo? tunIdx = 0xff => tun = null => E_NO_MORE_CONNECTIONS
    }
    else if (connRequest.cri().type() == TUNNEL_CONNECTION) //
    {
        // check if there is a reserved tunnel for the source
        int firstFreeTunnel = -1;
        int firstResAndFreeTunnel = -1;
        int firstResAndOccTunnel = -1;
        bool tunnelResActive[KNX_TUNNELING];
        uint8_t tunnelResOptions[KNX_TUNNELING];
        for (int i = 0; i < KNX_TUNNELING; i++)
        {
            if (resTunActive)
            {
                tunnelResActive[i] = *(tunCtrlBytes + i) & 0x80;
                tunnelResOptions[i] = (*(tunCtrlBytes + i) & 0x60) >> 5;
            }

            if (resTunActive && tunnelResActive[i]) // tunnel reserve feature active for this tunnel
            {
#ifdef KNX_LOG_TUNNELING
                print("tunnel reserve feature active for this tunnel: ");
                print(tunnelResActive[i]);
                print("  options: ");
                println(tunnelResOptions[i]);
#endif

                uint32_t rIP = 0;
                popInt(rIP, tunCtrlIp + 4 * i);
                if (srcIP == rIP)
                {
                    // reserved tunnel for this ip found
                    if (tunnels[i].ChannelId == 0) // check if it is free
                    {
                        if (firstResAndFreeTunnel < 0)
                            firstResAndFreeTunnel = i;
                    }
                    else
                    {
                        if (firstResAndOccTunnel < 0)
                            firstResAndOccTunnel = i;
                    }
                }
            }
            else
            {
                if (tunnels[i].ChannelId == 0 && firstFreeTunnel < 0)
                    firstFreeTunnel = i;
            }
        }
#ifdef KNX_LOG_TUNNELING
        print("firstFreeTunnel: ");
        print(firstFreeTunnel);
        print(" firstResAndFreeTunnel: ");
        print(firstResAndFreeTunnel);
        print(" firstResAndOccTunnel: ");
        println(firstResAndOccTunnel);
#endif

        if (resTunActive & (firstResAndFreeTunnel >= 0 || firstResAndOccTunnel >= 0)) // tunnel reserve feature active (for this src)
        {
            if (firstResAndFreeTunnel >= 0)
            {
                tunIdx = firstResAndFreeTunnel;
            }
            else if (firstResAndOccTunnel >= 0)
            {
                if (tunnelResOptions[firstResAndOccTunnel] == 1) // decline req
                {
                    ; // do nothing => decline
                }
                else if (tunnelResOptions[firstResAndOccTunnel] == 2) // close current tunnel connection on this tunnel and assign to this request
                {
                    KnxIpDisconnectRequest discReq;
                    discReq.channelId(tunnels[firstResAndOccTunnel].ChannelId);
                    discReq.hpaiCtrl().length(LEN_IPHPAI);
                    discReq.hpaiCtrl().code(IPV4_UDP);
                    discReq.hpaiCtrl().ipAddress(tunnels[firstResAndOccTunnel].IpAddress);
                    discReq.hpaiCtrl().ipPortNumber(tunnels[firstResAndOccTunnel].PortCtrl);
                    _platform.sendBytesUniCast(tunnels[firstResAndOccTunnel].IpAddress, tunnels[firstResAndOccTunnel].PortCtrl, discReq.data(), discReq.totalLength());
                    tunnels[firstResAndOccTunnel].Reset();

                    tunIdx = firstResAndOccTunnel;
                }
                else if (tunnelResOptions[firstResAndOccTunnel] == 3) // use the first unreserved tunnel (if one)
                {
                    if (firstFreeTunnel >= 0)
                        tunIdx = firstFreeTunnel;
                    else
                        ; // do nothing => decline
                }
                // else
                //  should not happen
                //  do nothing => decline
            }
            // else
            //  should not happen
            //  do nothing => decline
        }
        else
        {
            if (firstFreeTunnel >= 0)
                tunIdx = firstFreeTunnel;
            // else
            //  do nothing => decline
        }
    }

    KnxIpTunnelConnection* tun = nullptr;
    if (tunIdx != 0xFF)
    {
        tun = &tunnels[tunIdx];

        if (connRequest.cri().type() == DEVICE_MGMT_CONNECTION)
        {
            tun->IsConfig = true;
            tun->IndividualAddress = 0; // not relevant
        }
        else
        {
            tun->IsConfig = false;  // default
            uint16_t tunPa = 0;
            popWord(tunPa, addresses + (tunIdx * 2));

            // check if this PA is in use (should not happen, only when there is one pa wrongly assigned to more then one tunnel)
            for (int x = 0; x < KNX_TUNNELING; x++)
                if (tunnels[x].IndividualAddress == tunPa)
                {
#ifdef KNX_LOG_TUNNELING
                    println("cannot use tunnel because PA is already in use");
#endif
                    tunIdx = 0xFF;
                    tun = nullptr;
                    break;
                }
            if (tun)
                tun->IndividualAddress = tunPa;
        }
    }

    if (tun == nullptr)
    {
        println("no free tunnel availible");
        KnxIpConnectResponse connRes(0x00, E_NO_MORE_CONNECTIONS);
        _platform.sendBytesUniCast(connRequest.hpaiCtrl().ipAddress(), connRequest.hpaiCtrl().ipPortNumber(), connRes.data(), connRes.totalLength());
        return;
    }

    // the channel ID shall be unique on this tunnel server. catch the rare case of a double channel ID
    bool channelIdInUse;
    do
    {
        _lastChannelId++;
        channelIdInUse = false;
        for (int x = 0; x < KNX_TUNNELING; x++)
            if (tunnels[x].ChannelId == _lastChannelId)
                channelIdInUse = true;
    } while (channelIdInUse);

    tun->ChannelId = _lastChannelId;
    tun->lastHeartbeat = millis();
    if (_lastChannelId == 255)
        _lastChannelId = 0;

    tun->IpAddress = srcIP;
    tun->PortData = connRequest.hpaiData().ipPortNumber() ? connRequest.hpaiData().ipPortNumber() : srcPort;
    tun->PortCtrl = connRequest.hpaiCtrl().ipPortNumber() ? connRequest.hpaiCtrl().ipPortNumber() : srcPort;

    print("New Tunnel-Connection[");
    print(tunIdx);
    print("], Channel: 0x");
    print(tun->ChannelId, 16);
    print(" PA: ");
    print(tun->IndividualAddress >> 12);
    print(".");
    print((tun->IndividualAddress >> 8) & 0xF);
    print(".");
    print(tun->IndividualAddress & 0xFF);

    print(" with ");
    print(tun->IpAddress >> 24);
    print(".");
    print((tun->IpAddress >> 16) & 0xFF);
    print(".");
    print((tun->IpAddress >> 8) & 0xFF);
    print(".");
    print(tun->IpAddress & 0xFF);
    print(":");
    print(tun->PortData);
    if (tun->PortData != tun->PortCtrl)
    {
        print(" (Ctrlport: ");
        print(tun->PortCtrl);
        print(")");
    }
    if (tun->IsConfig)
    {
        print(" (Config-Channel)");
    }
    println();

    KnxIpConnectResponse connRes(_ipParameters, tun->IndividualAddress, 3671, tun->ChannelId, connRequest.cri().type());
    _platform.sendBytesUniCast(tun->IpAddress, tun->PortCtrl, connRes.data(), connRes.totalLength());
}

void IpTunnelServer::HandleConnectionStateRequest(uint8_t* buffer, uint16_t length)
{
    KnxIpStateRequest stateRequest(buffer, length);

    KnxIpTunnelConnection* tun = nullptr;
    for (int i = 0; i < KNX_TUNNELING + KNX_TUNNELING_DEVMGMT; i++)
    {
        if (tunnels[i].ChannelId == stateRequest.channelId())
        {
            tun = &tunnels[i];
            break;
        }
    }

#ifdef OPENKNX_HW_BUSMON
    if (tun == nullptr && _busMonTunnel.ChannelId != 0 && _busMonTunnel.ChannelId == stateRequest.channelId())
        tun = &_busMonTunnel;
#endif

    if (tun == nullptr)
    {
#ifdef KNX_LOG_TUNNELING
        print("Channel ID nicht gefunden: ");
        println(stateRequest.channelId());
#endif
        KnxIpStateResponse stateRes(0x00, E_CONNECTION_ID);
        _platform.sendBytesUniCast(stateRequest.hpaiCtrl().ipAddress(), stateRequest.hpaiCtrl().ipPortNumber(), stateRes.data(), stateRes.totalLength());
        return;
    }

    // TODO check knx connection!
    // if no connection return E_KNX_CONNECTION

    // TODO check when to send E_DATA_CONNECTION

    tun->lastHeartbeat = millis();
    KnxIpStateResponse stateRes(tun->ChannelId, E_NO_ERROR);
    _platform.sendBytesUniCast(stateRequest.hpaiCtrl().ipAddress(), stateRequest.hpaiCtrl().ipPortNumber(), stateRes.data(), stateRes.totalLength());
}

void IpTunnelServer::HandleDisconnectRequest(uint8_t* buffer, uint16_t length)
{
    KnxIpDisconnectRequest discReq(buffer, length);

#ifdef KNX_LOG_TUNNELING
    print(">>> Disconnect Channel ID: ");
    println(discReq.channelId());
#endif

    KnxIpTunnelConnection* tun = nullptr;
    for (int i = 0; i < KNX_TUNNELING + KNX_TUNNELING_DEVMGMT; i++)
    {
        if (tunnels[i].ChannelId == discReq.channelId())
        {
            tun = &tunnels[i];
            break;
        }
    }

#ifdef OPENKNX_HW_BUSMON
    if (tun == nullptr && _busMonTunnel.ChannelId != 0 && _busMonTunnel.ChannelId == discReq.channelId())
    {
        // Busmon tunnel closed by ETS -> leave HW monitor mode, routing returns (see plan 5b/6).
        KnxIpDisconnectResponse discRes(_busMonTunnel.ChannelId, E_NO_ERROR);
        _platform.sendBytesUniCast(discReq.hpaiCtrl().ipAddress(), discReq.hpaiCtrl().ipPortNumber(), discRes.data(), discRes.totalLength());
        busMonitorTeardown();
        return;
    }
#endif

    if (tun == nullptr)
    {
#ifdef KNX_LOG_TUNNELING
        print("Channel ID nicht gefunden: ");
        println(discReq.channelId());
#endif
        KnxIpDisconnectResponse discRes(0x00, E_CONNECTION_ID);
        _platform.sendBytesUniCast(discReq.hpaiCtrl().ipAddress(), discReq.hpaiCtrl().ipPortNumber(), discRes.data(), discRes.totalLength());
        return;
    }

    KnxIpDisconnectResponse discRes(tun->ChannelId, E_NO_ERROR);
    _platform.sendBytesUniCast(discReq.hpaiCtrl().ipAddress(), discReq.hpaiCtrl().ipPortNumber(), discRes.data(), discRes.totalLength());
    tun->Reset();
}

void IpTunnelServer::HandleDescriptionRequest(uint8_t* buffer, uint16_t length)
{
    KnxIpDescriptionRequest descReq(buffer, length);
    KnxIpDescriptionResponse descRes(_ipParameters, _deviceObject);
    _platform.sendBytesUniCast(descReq.hpaiCtrl().ipAddress(), descReq.hpaiCtrl().ipPortNumber(), descRes.data(), descRes.totalLength());
}

void IpTunnelServer::HandleDeviceConfigurationRequest(uint8_t* buffer, uint16_t length)
{
    KnxIpConfigRequest confReq(buffer, length);

    KnxIpTunnelConnection* tun = nullptr;
    for (int i = KNX_TUNNELING; i < KNX_TUNNELING + KNX_TUNNELING_DEVMGMT; i++)
    {
        if (tunnels[i].ChannelId == confReq.connectionHeader().channelId())
        {
            tun = &tunnels[i];
            break;
        }
    }

    if (tun == nullptr)
    {
        print("Channel ID nicht gefunden: ");
        println(confReq.connectionHeader().channelId());
        KnxIpStateResponse stateRes(0x00, E_CONNECTION_ID);
        _platform.sendBytesUniCast(0, 0, stateRes.data(), stateRes.totalLength());
        return;
    }

    KnxIpTunnelingAck tunnAck;
    tunnAck.serviceTypeIdentifier(DeviceConfigurationAck);
    tunnAck.connectionHeader().length(4);
    tunnAck.connectionHeader().channelId(tun->ChannelId);
    tunnAck.connectionHeader().sequenceCounter(confReq.connectionHeader().sequenceCounter());
    tunnAck.connectionHeader().status(E_NO_ERROR);
    _platform.sendBytesUniCast(tun->IpAddress, tun->PortData, tunnAck.data(), tunnAck.totalLength());

    tun->lastHeartbeat = millis();
    _cemiServer.frameReceived(confReq.frame(), tun->ChannelId);
}

void IpTunnelServer::HandleTunnelingRequest(uint8_t* buffer, uint16_t length)
{
    KnxIpTunnelingRequest tunnReq(buffer, length);

    KnxIpTunnelConnection* tun = nullptr;
    for (int i = 0; i < KNX_TUNNELING; i++)
    {
        if (tunnels[i].ChannelId == tunnReq.connectionHeader().channelId())
        {
            tun = &tunnels[i];
            break;
        }
    }

    if (tun == nullptr)
    {
#ifdef KNX_LOG_TUNNELING
        print("Channel ID nicht gefunden: ");
        println(tunnReq.connectionHeader().channelId());
#endif
        KnxIpStateResponse stateRes(0x00, E_CONNECTION_ID);
        _platform.sendBytesUniCast(0, 0, stateRes.data(), stateRes.totalLength());
        return;
    }

    uint8_t sequence = tunnReq.connectionHeader().sequenceCounter();
    if (sequence == tun->SequenceCounter_R)
    {
#ifdef KNX_LOG_TUNNELING
        print("Received SequenceCounter again: ");
        println(tunnReq.connectionHeader().sequenceCounter());
#endif
        // we already got this one
        // so just ack it
        KnxIpTunnelingAck tunnAck;
        tunnAck.connectionHeader().length(4);
        tunnAck.connectionHeader().channelId(tun->ChannelId);
        tunnAck.connectionHeader().sequenceCounter(tunnReq.connectionHeader().sequenceCounter());
        tunnAck.connectionHeader().status(E_NO_ERROR);
        _platform.sendBytesUniCast(tun->IpAddress, tun->PortData, tunnAck.data(), tunnAck.totalLength());
        return;
    }
    else if ((uint8_t)(sequence - 1) != tun->SequenceCounter_R)
    {
#ifdef KNX_LOG_TUNNELING
        print("Wrong SequenceCounter: got ");
        print(tunnReq.connectionHeader().sequenceCounter());
        print(" expected ");
        println((uint8_t)(tun->SequenceCounter_R + 1));
#endif
        // Dont handle it
        return;
    }

    KnxIpTunnelingAck tunnAck;
    tunnAck.connectionHeader().length(4);
    tunnAck.connectionHeader().channelId(tun->ChannelId);
    tunnAck.connectionHeader().sequenceCounter(tunnReq.connectionHeader().sequenceCounter());
    tunnAck.connectionHeader().status(E_NO_ERROR);
    _platform.sendBytesUniCast(tun->IpAddress, tun->PortData, tunnAck.data(), tunnAck.totalLength());

    tun->SequenceCounter_R = tunnReq.connectionHeader().sequenceCounter();

    if (tunnReq.frame().sourceAddress() == 0)
        tunnReq.frame().sourceAddress(tun->IndividualAddress);

    _cemiServer.frameReceived(tunnReq.frame(), tun->ChannelId);
}

#ifdef OPENKNX_HW_BUSMON
void IpTunnelServer::HandleBusMonitorConnect(KnxIpConnectRequest& connRequest, uint32_t src_addr, uint16_t src_port)
{
    uint32_t srcIP = connRequest.hpaiCtrl().ipAddress() ? connRequest.hpaiCtrl().ipAddress() : src_addr;
    uint16_t srcPort = connRequest.hpaiCtrl().ipPortNumber() ? connRequest.hpaiCtrl().ipPortNumber() : src_port;

    // Single busmonitor connection only.
    if (_busMonTunnel.ChannelId != 0 || _hwBusMon == nullptr)
    {
        KnxIpConnectResponse connRes(0x00, _hwBusMon == nullptr ? E_TUNNELING_LAYER : E_NO_MORE_CONNECTIONS);
        _platform.sendBytesUniCast(connRequest.hpaiCtrl().ipAddress(), connRequest.hpaiCtrl().ipPortNumber(), connRes.data(), connRes.totalLength());
        return;
    }

    // Unique channel id across all normal tunnels and the busmon connection.
    bool channelIdInUse;
    do
    {
        _lastChannelId++;
        channelIdInUse = (_lastChannelId == 0);
        for (int x = 0; x < KNX_TUNNELING + KNX_TUNNELING_DEVMGMT; x++)
            if (tunnels[x].ChannelId == _lastChannelId)
                channelIdInUse = true;
    } while (channelIdInUse);

    _busMonTunnel.IsConfig = false;
    _busMonTunnel.IndividualAddress = 0;
    _busMonTunnel.IpAddress = srcIP;
    _busMonTunnel.PortData = connRequest.hpaiData().ipPortNumber() ? connRequest.hpaiData().ipPortNumber() : srcPort;
    _busMonTunnel.PortCtrl = connRequest.hpaiCtrl().ipPortNumber() ? connRequest.hpaiCtrl().ipPortNumber() : srcPort;
    _busMonTunnel.SequenceCounter_S = 0;
    _busMonTunnel.lastHeartbeat = millis();
    _busMonTunnel.ChannelId = _lastChannelId; // set last -> busMonitorActive() true only once fully set up
    _busMonExitPending = false;

    _hwBusMon->hwBusMonEnter(); // U_BUSMON_REQ -> chip passive, routing paused

    print("New HW-Busmon connection, Channel: 0x");
    print(_busMonTunnel.ChannelId, 16);
    println(" (routing paused until disconnect)");

    KnxIpConnectResponse connRes(_ipParameters, _deviceObject.individualAddress(), 3671, _busMonTunnel.ChannelId, TUNNEL_CONNECTION);
    _platform.sendBytesUniCast(_busMonTunnel.IpAddress, _busMonTunnel.PortCtrl, connRes.data(), connRes.totalLength());
}

void IpTunnelServer::busMonitorTeardown()
{
    if (_busMonTunnel.ChannelId == 0)
        return;

    _busMonTunnel.Reset(); // stop forwarding at once (busMonitorActive() -> false)
    if (_hwBusMon)
    {
        _hwBusMon->hwBusMonExit();     // reset() -> BCU_CONNECTED
        _busMonExitPending = true;     // bounded, non-blocking recovery poll in loop()
        _busMonExitStart = millis();
    }
}

void IpTunnelServer::busMonitorFrame(uint8_t* lpdu, uint16_t len)
{
    if (_busMonTunnel.ChannelId == 0 || len == 0)
        return;

    // cEMI L_Busmon.ind: [0x2B][AI len=3][AI type 0x03, len 0x01, status/seq][raw LPDU incl FCS].
    // CemiFrame carries a fixed (0xFF + NPDU_LPDU_DIFF)=263 byte buffer -> length-guard the copy.
    if ((uint16_t)(5 + len) > (0xFF + NPDU_LPDU_DIFF))
        return;

    uint8_t buf[0xFF + NPDU_LPDU_DIFF];
    buf[0] = L_busmon_ind; // 0x2B
    buf[1] = 0x03;         // additional info length
    buf[2] = 0x03;         // AI type: bus monitor status
    buf[3] = 0x01;         // AI value length
    buf[4] = _busMonSeq++ & 0x0F; // low nibble = rolling frame sequence
    memcpy(buf + 5, lpdu, len);

    CemiFrame frame(buf, 5 + len);
    KnxIpTunnelingRequest req(frame); // ctor sets serviceTypeIdentifier(TunnelingRequest)
    req.connectionHeader().sequenceCounter(_busMonTunnel.SequenceCounter_S++);
    req.connectionHeader().length(LEN_CH);
    req.connectionHeader().channelId(_busMonTunnel.ChannelId);
    _platform.sendBytesUniCast(_busMonTunnel.IpAddress, _busMonTunnel.PortData, req.data(), req.totalLength());
}
#endif

#endif
#include "knx_ip_tunnel_connection.h"

KnxIpTunnelConnection::KnxIpTunnelConnection()
{

}

void KnxIpTunnelConnection::Reset()
{
    print("Close Tunnel-Connection, Channel: 0x");
    println(ChannelId, 16);

    ChannelId = 0;
    IpAddress = 0;
    PortData = 0;
    PortCtrl = 0;
    lastHeartbeat = 0;
    SequenceCounter_S = 0;
    SequenceCounter_R = 255;
    IndividualAddress = 0;
    IsConfig = false;
    ReservedSlot = 0xFF;
#ifdef KNX_TUNNEL_RESEND
    _txHead = 0;
    _txTail = 0;
    _txCount = 0;
    _armed = false;
    _retries = 0;
#endif
}

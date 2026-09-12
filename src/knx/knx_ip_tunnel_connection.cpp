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
    // Not cleared, like connectStart: every connect overwrites it, and a zero would make a row read
    // during teardown say "connected since boot" -- a stale stamp is merely the previous session's.
    StatToClient = 0;
    StatFromClient = 0;
    StatResend = 0;
    StatSeqGap = 0;
    StatTxDrop = 0;
    StatGrpDrop = 0;
    StatQueuePeak = 0;
#ifdef KNX_TUNNEL_RESEND
    _txHead = 0;
    _txTail = 0;
    _txCount = 0;
    _armed = false;
    _headSent = false;
    _retries = 0;
#endif
}

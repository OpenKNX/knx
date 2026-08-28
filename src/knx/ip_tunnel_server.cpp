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

#if defined(OPENKNX_HW_BUSMON) || defined(KNX_TUNNEL_RESEND)
#include <string.h>
#endif

IpTunnelServer::IpTunnelServer(DeviceObject& devObj, IpParameterObject& ipParam, Platform& platform, CemiServer& cemiServer) : _deviceObject(devObj),
                                                                                                                               _ipParameters(ipParam),
                                                                                                                               _platform(platform),
                                                                                                                               _cemiServer(cemiServer)
{
}

bool IpTunnelServer::sendCounted(uint32_t addr, uint16_t port, uint8_t* buffer, uint16_t len)
{
    const bool sent = _platform.sendBytesUniCast(addr, port, buffer, len);
    if (_counters != nullptr)
    {
        if (sent)
            _counters->incrementTransmitToIp();
        else
            // A false return is dominated by a full IP TX buffer (ENOBUFS / no free W5500 slot) = a real
            // queue overflow to IP (PID 72, 03_08_03 2.5.23). A rare link-down / socket error also lands
            // here; splitting the two would need the platform to report the failure reason -- kept as-is.
            _counters->incrementOverflowToIp();
    }
    return sent;
}

uint8_t IpTunnelServer::tunnelCount() const
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < KNX_TUNNELING; i++)
        if (tunnels[i].ChannelId != 0) n++;
    return n;
}

const KnxIpTunnelConnection* IpTunnelServer::tunnelAt(uint8_t index) const
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < KNX_TUNNELING; i++)
    {
        if (tunnels[i].ChannelId == 0) continue;
        if (n == index) return &tunnels[i];
        n++;
    }
    return nullptr;
}

uint8_t IpTunnelServer::activeTunnels(TunnelEvent* out, uint8_t maxOut) const
{
    uint8_t n = 0;
    for (int i = 0; i < KNX_TUNNELING + KNX_TUNNELING_DEVMGMT && n < maxOut; i++)
    {
        if (tunnels[i].ChannelId == 0) continue;
        out[n].ip = tunnels[i].IpAddress;
        out[n].pa = tunnels[i].IndividualAddress;
        out[n].type = tunnels[i].IsConfig ? TUN_CONFIG : TUN_DATA;
        out[n].reason = END_ACTIVE;
        out[n].startMillis = tunnels[i].connectStart;
        out[n].endMillis = 0;
        n++;
    }
#ifdef OPENKNX_HW_BUSMON
    if (n < maxOut && _busMonTunnel.ChannelId != 0)
    {
        out[n].ip = _busMonTunnel.IpAddress;
        out[n].pa = 0;
        out[n].type = TUN_BUSMON;
        out[n].reason = END_ACTIVE;
        out[n].startMillis = _busMonTunnel.connectStart;
        out[n].endMillis = 0;
        n++;
    }
#endif
    return n;
}

void IpTunnelServer::recordTunnelSession(uint32_t ip, uint16_t pa, uint8_t type, unsigned long startMillis, uint8_t reason,
                                        uint8_t detail)
{
    TunnelEvent& e = _history[_historyHead];
    e.ip = ip;
    e.pa = pa;
    e.type = type;
    e.reason = reason;
    e.detail = detail;
    e.startMillis = startMillis;
    e.endMillis = millis();
    _historyHead = (_historyHead + 1) % TUNNEL_HISTORY_SIZE;
    if (_historyCount < TUNNEL_HISTORY_SIZE) _historyCount++;
}

// A refused connect is an event without a session (start == end, 0 s duration). An identical repeat from
// the same client only refreshes the newest entry, so a retrying client can't flush the 32-entry ring.
void IpTunnelServer::recordRejectedConnect(uint32_t ip, uint8_t type, uint8_t reason, uint8_t detail)
{
    if (_historyCount > 0)
    {
        TunnelEvent& last = _history[(uint8_t)((_historyHead + TUNNEL_HISTORY_SIZE - 1) % TUNNEL_HISTORY_SIZE)];
        if (last.ip == ip && last.reason == reason && last.detail == detail && last.type == type)
        {
            last.endMillis = millis(); // same refusal again: keep one entry, extend it
            return;
        }
    }
    recordTunnelSession(ip, 0, type, millis(), reason, detail);
}

uint8_t IpTunnelServer::tunnelHistoryCount() const
{
    return _historyCount;
}

const IpTunnelServer::TunnelEvent* IpTunnelServer::tunnelHistoryAt(uint8_t index) const
{
    if (index >= _historyCount) return nullptr;
    // Newest first: the slot just before _historyHead is the most recent.
    uint8_t slot = (uint8_t)((_historyHead + TUNNEL_HISTORY_SIZE - 1 - index) % TUNNEL_HISTORY_SIZE);
    return &_history[slot];
}

#ifdef OPENKNX_CON_DIAG
// L_Data.con-generation diagnostics: counters filled across the tpuart RX/TX path and this tunnel server,
// dumped once a probe burst settles. Gated by OPENKNX_CON_DIAG (off in normal builds).
static uint16_t g_conRouted = 0, g_conWire = 0, g_conSendFail = 0, g_conRetry = 0;
// bus->tunnel indication path: call / individual / matched-by-IA / no-tunnel.
static uint16_t g_indCall = 0, g_indIndiv = 0, g_indFound = 0, g_indNoTun = 0;
// client->device ACK path: reached handleTunnelAck / matched an in-flight head.
static uint16_t g_ackRecv = 0, g_ackMatch = 0;
extern uint16_t g_rxTxWaitAckn, g_rxTxCompleted, g_txAwaitEcho, g_echoMismatch, g_echoTxIdle; // tpuart Receiver
extern uint16_t g_txSent; // tpuart Transmitter (frames handed to TX_TRANSMIT)
extern uint16_t g_rxTxDropped; // tpuart DataLinkLayer (rx-frame-buffer overflow drop)
extern uint16_t g_conRxRecv; // knx TpUartDataLinkLayer (isTransmitted reaching dataConReceived)
extern uint16_t g_bef3Drop; // knx IpDataLinkLayer (KNXnet/IP header total-length mismatch drop)
#endif

void IpTunnelServer::loop()
{
#ifdef OPENKNX_CON_DIAG
    { // dump con counters once stable 4s after a probe burst (> the 3s probe timeout -> no mid-run dump)
        static uint16_t s_lastSum = 0; static uint32_t s_stableAt = 0; static bool s_dumped = false;
        uint16_t sum = g_conRouted + g_conWire + g_rxTxWaitAckn + g_rxTxCompleted + g_conRxRecv + g_rxTxDropped + g_indCall + g_ackRecv + g_bef3Drop;
        if (sum != s_lastSum) { s_lastSum = sum; s_stableAt = millis(); s_dumped = false; }
        else if (sum > 0 && !s_dumped && millis() - s_stableAt > 4000)
        {
            print("[CONCNT] routed="); print((int)g_conRouted); print(" wire="); print((int)g_conWire); print(" sendFail="); print((int)g_conSendFail); print(" retry="); print((int)g_conRetry); print(" rxWaitAckn="); print((int)g_rxTxWaitAckn); print(" rxCompleted="); print((int)g_rxTxCompleted); print(" rxRecv="); print((int)g_conRxRecv); print(" txAwait="); print((int)g_txAwaitEcho); print(" echoMiss="); print((int)g_echoMismatch); print(" txIdle="); print((int)g_echoTxIdle); print(" txSent="); print((int)g_txSent); print(" rxDropped="); print((int)g_rxTxDropped); print(" indCall="); print((int)g_indCall); print(" indIndiv="); print((int)g_indIndiv); print(" indFound="); print((int)g_indFound); print(" indNoTun="); print((int)g_indNoTun); print(" ackRecv="); print((int)g_ackRecv); print(" ackMatch="); print((int)g_ackMatch); print(" bef3Drop="); println((int)g_bef3Drop);
            g_conRouted = 0; g_conWire = 0; g_conSendFail = 0; g_conRetry = 0; g_rxTxWaitAckn = 0; g_rxTxCompleted = 0; g_rxTxDropped = 0; g_conRxRecv = 0; g_txAwaitEcho = 0; g_echoMismatch = 0; g_echoTxIdle = 0; g_txSent = 0; g_indCall = 0; g_indIndiv = 0; g_indFound = 0; g_indNoTun = 0; g_ackRecv = 0; g_ackMatch = 0; g_bef3Drop = 0;
            s_dumped = true;
        }
    }
#endif
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
                sendCounted(tunnels[i].IpAddress, tunnels[i].PortCtrl, discReq.data(), discReq.totalLength());
                recordTunnelSession(tunnels[i].IpAddress, tunnels[i].IndividualAddress, tunnels[i].IsConfig ? TUN_CONFIG : TUN_DATA, tunnels[i].connectStart, END_TIMEOUT);
                tunnels[i].Reset();
            }
#ifdef KNX_TUNNEL_RESEND
            // No ACK for the in-flight head within the request timeout -> resend verbatim (same stamped seq),
            // disconnect after the last repeat. Data (TUNNELLING_REQUEST): 1 s + 1 repeat (03_08_04 §2.6.1 p.9).
            // Config (DEVICE_CONFIGURATION_REQUEST): 10 s + 3 repeats (03_08_03; = certified refs 0/10/20/30 s).
            else if (tunnels[i]._armed && millis() - tunnels[i]._sentAt > (tunnels[i].IsConfig ? 10000u : 1000u))
            {
                if (tunnels[i]._retries < (tunnels[i].IsConfig ? 3 : 1))
                {
                    sendCounted(tunnels[i].IpAddress, tunnels[i].PortData, tunnels[i]._txBuf[tunnels[i]._txHead], tunnels[i]._txLen[tunnels[i]._txHead]);
#ifdef OPENKNX_CON_DIAG
                    g_conRetry++; // resend fired
#endif
                    tunnels[i]._retries++;
                    tunnels[i]._sentAt = millis();
                }
                else
                {
                    disconnectTunnel(&tunnels[i], END_TIMEOUT);
                }
            }
#endif
            // loop() scans ALL slots and reaps every expired tunnel -- no early break at the first occupied
            // slot (which would leave dead tunnels in later slots unreaped -> slot exhaustion).
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
        sendCounted(_busMonTunnel.IpAddress, _busMonTunnel.PortCtrl, discReq.data(), discReq.totalLength());
        busMonitorTeardown(END_TIMEOUT);
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
                _hwBusMon->hwBusMonExit(); // one more reset; a truly latched NCN may need a power-cycle
            println("HW-Busmon: chip did not return to CONNECTED within 3s (possible NCN latch)");
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

void IpTunnelServer::dataConfirmationToTunnel(CemiFrame& frame)
{
    if (frame.addressType() == AddressType::GroupAddress)
    {
        for (int i = 0; i < KNX_TUNNELING; i++)
            if (tunnels[i].ChannelId != 0 && tunnels[i].IndividualAddress == frame.sourceAddress())
                sendFrameToTunnel(&tunnels[i], frame);
        // source is always a tunnel PA here -- dataConReceived only calls this after isTunnelingPA(source)
        return;
    }

    KnxIpTunnelConnection* tun = nullptr;
    for (int i = 0; i < KNX_TUNNELING; i++)
    {
        // Route the con to the tunnel whose IA == source (the sender). The old extra skip on
        // IA == destinationAddress wrongly dropped the con when a client probes its own tunnel IA
        // (src == dest): that tunnel got skipped by dest before the source match could hit it.
        if (tunnels[i].ChannelId == 0)
            continue;

        if (tunnels[i].IndividualAddress == frame.sourceAddress())
        {
            tun = &tunnels[i];
            break;
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

#ifdef OPENKNX_CON_DIAG
    g_conRouted++;
#endif
    sendFrameToTunnel(tun, frame);
}

void IpTunnelServer::dataIndicationToTunnel(CemiFrame& frame)
{
#ifdef OPENKNX_CON_DIAG
    g_indCall++;
#endif
    if (frame.addressType() == AddressType::GroupAddress)
    {
        for (int i = 0; i < KNX_TUNNELING; i++)
            if (tunnels[i].ChannelId != 0 && tunnels[i].IndividualAddress != frame.sourceAddress())
                sendFrameToTunnel(&tunnels[i], frame);
        return;
    }

#ifdef OPENKNX_CON_DIAG
    g_indIndiv++;
#endif
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
#ifdef OPENKNX_CON_DIAG
        g_indNoTun++;
#endif
#ifdef KNX_LOG_TUNNELING
        print("Found no Tunnel for IA: ");
        println(frame.destinationAddress(), 16);
#endif
        return;
    }

#ifdef OPENKNX_CON_DIAG
    g_indFound++;
#endif
    sendFrameToTunnel(tun, frame);
}

void IpTunnelServer::sendFrameToTunnel(KnxIpTunnelConnection* tunnel, CemiFrame& frame)
{
#ifdef KNX_LOG_TUNNELING
    print("Send to Channel: ");
    println(tunnel->ChannelId, 16);
#endif

    // L_Data goes as a TUNNELLING_REQUEST; everything else (M_Prop*/cEMI mgmt) as DEVICE_CONFIGURATION_REQUEST.
    const uint16_t svc = (frame.messageCode() == L_data_req || frame.messageCode() == L_data_con
                          || frame.messageCode() == L_data_ind) ? TunnelingRequest : DeviceConfigurationRequest;

#ifdef KNX_TUNNEL_RESEND
    const uint16_t cemiLen = frame.totalLenght();
    const uint16_t totalLen = cemiLen + LEN_CH + LEN_KNXIP_HEADER;
    
    // Enqueue the datagram; pumpTunnel() keeps exactly one on the wire. The sequence counter is stamped at
    // pump time (offset 8), so an overflow-dropped frame consumes no seq -> the on-wire sequence stays gap-free.
    if (totalLen > KNX_TUNNEL_RESEND_BUF)
    {
        // Oversize (only ever a large group frame, never a CO frame on TP1). Drop it (group delivery has no
        // guarantee) rather than break the one-on-the-wire ordering.
    #ifdef KNX_LOG_TUNNELING
        println("tunnel tx: oversize frame dropped");
    #endif
        return;
    }
    if (tunnel->_txCount == KNX_TUNNEL_RESEND_DEPTH)
    {
        // FIFO full: drop a best-effort group telegram and keep the connection (the 1 s ACK-timeout in loop()
        // is the disconnect authority, 03_08_04 §2.6.1). A non-group (CO/mgmt) overflow still disconnects.
        if (frame.addressType() == AddressType::GroupAddress)
            return;
        disconnectTunnel(tunnel, END_TIMEOUT);
        return;
    }
    // Build the datagram DIRECTLY into the FIFO slot -> no per-frame new[] and no intermediate copy (was:
    // KnxIpTunnelingRequest new[] then memcpy into the slot). Fixed KNXnet/IP layout: 6-byte header + 4-byte
    // connection header + cEMI. Built by hand (not via KnxIpTunnelingRequest) so no CemiFrame view is placed
    // over the still-uninitialized slot -- the view ctor reads data[1] to compute its offsets. seqCounter
    // (offset 8) is stamped in pumpTunnel; status (offset 9) is reserved 0 on a request.
    uint8_t* buf = tunnel->_txBuf[tunnel->_txTail];
    buf[0] = LEN_KNXIP_HEADER;    // KNXnet/IP header length (6)
    buf[1] = KnxIp1_0;            // protocol version (0x10)
    pushWord(svc, buf + 2);       // service type identifier
    pushWord(totalLen, buf + 4);  // total datagram length
    buf[6] = LEN_CH;              // connection header structure length (4)
    buf[7] = tunnel->ChannelId;   // channel id
    buf[8] = 0;                   // sequence counter (set in pumpTunnel)
    buf[9] = 0;                   // status / reserved
    memcpy(buf + LEN_KNXIP_HEADER + LEN_CH, frame.data(), cemiLen);

    tunnel->_txLen[tunnel->_txTail] = totalLen;
    tunnel->_txTail = (tunnel->_txTail + 1) % KNX_TUNNEL_RESEND_DEPTH;
    tunnel->_txCount++;
    pumpTunnel(tunnel);
#else
    KnxIpTunnelingRequest req(frame);
    req.connectionHeader().length(LEN_CH);
    req.connectionHeader().channelId(tunnel->ChannelId);
    req.serviceTypeIdentifier(svc);
    req.connectionHeader().sequenceCounter(tunnel->SequenceCounter_S++);
    sendCounted(tunnel->IpAddress, tunnel->PortData, req.data(), req.totalLength());
#endif
}

#ifdef KNX_TUNNEL_RESEND
// Send the head of a tunnel's FIFO if nothing is in flight. Exactly one unacked TUNNELLING_REQUEST per
// tunnel; the sequence counter is stamped into the connection header here (KNX 03_08_04 Tunnelling §2.6.1).
void IpTunnelServer::pumpTunnel(KnxIpTunnelConnection* t)
{
    if (t->_armed || t->_txCount == 0) return;
    uint8_t h = t->_txHead;
    t->_txBuf[h][8] = t->SequenceCounter_S; // connection header: length@6, channelId@7, sequenceCounter@8
    t->_seq = t->SequenceCounter_S++;
    t->_retries = 0;
    t->_sentAt = millis();
    t->_armed = true;
#ifdef OPENKNX_CON_DIAG
    if (t->_txBuf[h][10] == 0x2E) g_conWire++; // 0x2E = L_data_con at cEMI offset 10
    if (!sendCounted(t->IpAddress, t->PortData, t->_txBuf[h], t->_txLen[h])) g_conSendFail++; // send returned false
#else
    sendCounted(t->IpAddress, t->PortData, t->_txBuf[h], t->_txLen[h]);
#endif
}

// Server-initiated teardown (retry-exhausted / queue-overflow): tell the client and reap the slot.
void IpTunnelServer::disconnectTunnel(KnxIpTunnelConnection* t, uint8_t reason)
{
    KnxIpDisconnectRequest discReq;
    discReq.channelId(t->ChannelId);
    discReq.hpaiCtrl().length(LEN_IPHPAI);
    discReq.hpaiCtrl().code(IPV4_UDP);
    discReq.hpaiCtrl().ipAddress(t->IpAddress);
    discReq.hpaiCtrl().ipPortNumber(t->PortCtrl);
    sendCounted(t->IpAddress, t->PortCtrl, discReq.data(), discReq.totalLength());
    recordTunnelSession(t->IpAddress, t->IndividualAddress, t->IsConfig ? TUN_CONFIG : TUN_DATA, t->connectStart, reason);
    t->Reset();
}

// A TUNNELLING_ACK / DEVICE_CONFIGURATION_ACK cleared the in-flight head -> pop it and pump the next.
void IpTunnelServer::handleTunnelAck(uint8_t* buffer, uint16_t length)
{
    if (length < LEN_KNXIP_HEADER + LEN_CH) return;
#ifdef OPENKNX_CON_DIAG
    g_ackRecv++;
#endif
    KnxIpTunnelingAck ack(buffer, length);
    uint8_t ch = ack.connectionHeader().channelId();
    uint8_t seq = ack.connectionHeader().sequenceCounter();
    for (int i = 0; i < KNX_TUNNELING + KNX_TUNNELING_DEVMGMT; i++)
        if (tunnels[i].ChannelId == ch && tunnels[i]._armed && tunnels[i]._seq == seq)
        {
            if (ack.connectionHeader().status() == E_NO_ERROR)
            {
#ifdef OPENKNX_CON_DIAG
                g_ackMatch++;
#endif
                tunnels[i]._txHead = (tunnels[i]._txHead + 1) % KNX_TUNNEL_RESEND_DEPTH;
                tunnels[i]._txCount--;
                tunnels[i]._armed = false;
                pumpTunnel(&tunnels[i]); // send the next queued frame, if any
            }
            break;
        }
}
#endif

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
    // The sole caller (ip_data_link_layer) already dropped datagrams shorter than the 6-byte KNXnet/IP
    // header, so buffer[2..3] is safe here. The per-service length guards below (KNX 03_08_04 §5.4.6 p.31)
    // are the ones that prevent the connection-header offset underflows.
    uint16_t code;
    popWord(code, buffer + 2);
    switch ((KnxIpServiceType)code)
    {
        case ConnectRequest: {
            // header + control HPAI + data HPAI + CRI (length+type) must be present, else the HPAI/CRI reads in
            // HandleConnectRequest run on stale bytes (03_08_02 Core). The layer byte is read only for a TUNNEL
            // type whose CRI is longer, so guarding the CRI type byte is enough here.
            if (length < LEN_KNXIP_HEADER + 2 * LEN_IPHPAI + 2) return true;
            HandleConnectRequest(buffer, length, src_addr, src_port);
            break;
        }

        case ConnectionStateRequest: {
            if (length < LEN_KNXIP_HEADER + 2 + LEN_IPHPAI) return true; // channel id + reserved + the reply HPAI at buffer[8..]; else the reply endpoint is stale
            HandleConnectionStateRequest(buffer, length);
            break;
        }

        case DisconnectRequest: {
            if (length < LEN_KNXIP_HEADER + 2 + LEN_IPHPAI) return true; // channel id + reserved + the reply HPAI at buffer[8..]; else the reply endpoint is stale
            HandleDisconnectRequest(buffer, length);
            break;
        }

        case DescriptionRequest: {
            if (length < LEN_KNXIP_HEADER + LEN_IPHPAI) return true; // header + control HPAI; else hpaiCtrl() is stale
            HandleDescriptionRequest(buffer, length);
            break;
        }

        case DeviceConfigurationRequest: {
            if (length < LEN_KNXIP_HEADER + LEN_CH + 1) return true; // header + conn header + >=1 cEMI byte (messageCode); the M_Prop path has no L_Data valid() gate, so guard the messageCode read here
            HandleDeviceConfigurationRequest(buffer, length);
            break;
        }

        case TunnelingRequest: {
            if (length < LEN_KNXIP_HEADER + LEN_CH) return true; // header + connection header; else ctor underflows
            HandleTunnelingRequest(buffer, length);
            break;
        }

        case DeviceConfigurationAck: {
#ifdef KNX_TUNNEL_RESEND
            handleTunnelAck(buffer, length); // pop the acked head on a devmgmt channel + pump the next
#endif
            break;
        }

        case TunnelingAck: {
#ifdef KNX_TUNNEL_RESEND
            handleTunnelAck(buffer, length); // pop the acked head on a data tunnel + pump the next
#endif
            break;
        }
        case DisconnectResponse:
            // The client acked our own DISCONNECT_REQUEST (sent on timeout/busmon-takeover/retry-fail).
            // Nothing to do -> consume it so it is not logged as an unhandled service.
            break;
        default:
            return false;
            break;
    }
    return true;
}

void IpTunnelServer::HandleConnectRequest(uint8_t* buffer, uint16_t length, uint32_t& src_addr, uint16_t& src_port)
{
    // TODO EC tunnelling-v2: the Extended CRI (03_08_04 §5.4.3.2) that requests a specific tunnelling IA on
    // CONNECT is not parsed (KnxIpCRI is Basic-only) -- unreachable while the device advertises Core v1, so
    // a conformant ETS never sends it. Adding it means raising Core to v2 plus the 2Dh/28h/2Eh evaluation
    // and a CONNECT_RESPONSE response-matrix re-audit.
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

    // Route-back (03_08_02 Core 5.2): a route-back client's control HPAI is 0.0.0.0:0 -> reply to the packet
    // source; resolved once here and used for every reject response below.
    uint32_t rIp = connRequest.hpaiCtrl().ipAddress() ? connRequest.hpaiCtrl().ipAddress() : src_addr;
    uint16_t rPort = connRequest.hpaiCtrl().ipPortNumber() ? connRequest.hpaiCtrl().ipPortNumber() : src_port;

    // We only support 0x03 and 0x04!
    if (connRequest.cri().type() != TUNNEL_CONNECTION && connRequest.cri().type() != DEVICE_MGMT_CONNECTION)
    {
        recordRejectedConnect(rIp, TUN_OTHER, END_REJ_TYPE, (uint8_t)connRequest.cri().type());
#ifdef KNX_LOG_TUNNELING
        println("Only Tunnel/DeviceMgmt Connection ist supported!");
#endif
        KnxIpConnectResponse connRes(0x00, E_CONNECTION_TYPE);
        sendCounted(rIp, rPort, connRes.data(), connRes.totalLength());
        return;
    }

    // Guard the CRI layer octet (CRI[2]) before reading it: the generic dispatch length check only ensures the
    // CRI type octet is present. A TUNNEL_CONNECTION CRI is 4 octets (03_08_04 §5.4.3.1); reject a truncated one
    // instead of reading a stale leftover byte (in-bounds of the rx buffer, but undefined) as the layer.
    if (connRequest.cri().type() == TUNNEL_CONNECTION && length < LEN_KNXIP_HEADER + 2 * LEN_IPHPAI + 4)
    {
        recordRejectedConnect(rIp, TUN_DATA, END_REJ_TYPE, (uint8_t)TUNNEL_CONNECTION);
        KnxIpConnectResponse connRes(0x00, E_CONNECTION_TYPE);
        sendCounted(rIp, rPort, connRes.data(), connRes.totalLength());
        return;
    }

    if (connRequest.cri().type() == TUNNEL_CONNECTION && connRequest.cri().layer() != 0x02) // LinkLayer
    {
#ifdef OPENKNX_HW_BUSMON
        // KNX Busmonitor tunnelling layer (0x80): put the TP chip into real HW monitor mode.
        // NOTE (KNX 03_08_04 Tunnelling §2.2.4 p.8): busmonitor is not conformant on a routing device; this is a
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
        recordRejectedConnect(rIp, TUN_DATA, END_REJ_LAYER, connRequest.cri().layer());
        KnxIpConnectResponse connRes(0x00, E_TUNNELING_LAYER);
        sendCounted(rIp, rPort, connRes.data(), connRes.totalLength());
        return;
    }

#ifdef OPENKNX_HW_BUSMON
    // While the HW busmonitor is active the TP chip is passive (bus TX paused): a link-layer tunnel would
    // connect but every telegram it sends would be silently dropped -> ETS times out with a meaningless
    // error. Reject up front with the transient "no capacity" code E_NO_MORE_CONNECTIONS (KNX 03_08_02 Core
    // Table 8 p.39; busmon exclusivity KNX 03_08_04 Tunnelling §2.2.4 p.8) so programming/device-info fails
    // fast and deterministically. Device management (local interface
    // object, no bus TX) stays allowed. Received frames still flow to existing tunnels (soft busmon RX).
    // Reject a LinkLayer/group-monitor tunnel whenever the HW chip is in monitor mode -- from an ETS busmon
    // tunnel OR a local `bcu mon` (dual owner). A busmon tunnel (0x80) returned above; DEVICE_MGMT is not a
    // TUNNEL_CONNECTION so it stays allowed. hwBusMonActive() covers both owners; busMonitorActive() alone
    // would miss the local-console case.
    if (_hwBusMon != nullptr && _hwBusMon->hwBusMonActive() && connRequest.cri().type() == TUNNEL_CONNECTION)
    {
        println("IP tunnel connect rejected: HW busmonitor active (bus TX paused) - close the KNX Busmonitor to program via this interface");
        recordRejectedConnect(rIp, TUN_DATA, END_REJ_BUSY, 0);
        KnxIpConnectResponse connRes(0x00, E_NO_MORE_CONNECTIONS);
        sendCounted(rIp, rPort, connRes.data(), connRes.totalLength());
        return;
    }
#endif

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
        // re-point `addresses` at the property's live storage: addrbuffer[] is block-scoped and would dangle
        // when read later; propertyData() returns the persisted copy we just wrote.
        addresses = _ipParameters.propertyData(PID_ADDITIONAL_INDIVIDUAL_ADDRESSES);
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
    bool tunReservedAssign = false; // true = tunIdx is a RESERVED slot (fixed slot->IA); false = free pool
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
                tunReservedAssign = true; // reserved slot -> keep its fixed designated IA
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
                    sendCounted(tunnels[firstResAndOccTunnel].IpAddress, tunnels[firstResAndOccTunnel].PortCtrl, discReq.data(), discReq.totalLength());
                    // Audit-trail parity with every other teardown path: log the evicted session before Reset().
                    recordTunnelSession(tunnels[firstResAndOccTunnel].IpAddress, tunnels[firstResAndOccTunnel].IndividualAddress,
                                        tunnels[firstResAndOccTunnel].IsConfig ? TUN_CONFIG : TUN_DATA, tunnels[firstResAndOccTunnel].connectStart, END_CLOSED);
                    tunnels[firstResAndOccTunnel].Reset();

                    tunIdx = firstResAndOccTunnel;
                    tunReservedAssign = true; // reserved slot -> keep its fixed designated IA
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
    bool paNotUnique = false; // a free slot exists but its assignable tunnelling IA is already in use -> 0x25
    if (tunIdx != 0xFF)
    {
        tun = &tunnels[tunIdx];

        if (connRequest.cri().type() == DEVICE_MGMT_CONNECTION)
        {
            tun->IsConfig = true;
            tun->IndividualAddress = 0; // not relevant
        }
        else if (tunReservedAssign)
        {
            // Reserved tunnel: keep the fixed slot->IA mapping (behaviour unchanged).
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
                    paNotUnique = true; // slot was free, but its IA is not unique -> report 0x25 not 0x24
                    break;
                }
            if (tun)
                tun->IndividualAddress = tunPa;
        }
        else
        {
            // Non-reserved: assign the FIRST Additional IA that is unique among the OPEN connections
            // (KNX 03_08_04 Tunnelling §2.2.2). Scan the WHOLE pool -- not just addresses[slot] -- so gaps,
            // duplicate/unassigned entries and partial ETS assignment are handled; a free unique IA at any
            // index is used. 0x25 (paNotUnique) only when the list is truly depleted.
            tun->IsConfig = false;  // default
            uint16_t tunPa = 0;
            bool found = false;
            for (int a = 0; a < KNX_TUNNELING && !found; a++)
            {
                uint16_t cand = 0;
                popWord(cand, addresses + a * 2);
                if (cand == 0) continue;                                             // empty / invalid entry
                if (resTunActive && tunCtrlBytes && (*(tunCtrlBytes + a) & 0x80)) continue; // don't steal a reserved slot's IA
                bool inUse = false;
                for (int x = 0; x < KNX_TUNNELING; x++)
                    if (tunnels[x].ChannelId != 0 && tunnels[x].IndividualAddress == cand)
                    {
                        inUse = true;
                        break;
                    }
                if (!inUse)
                {
                    tunPa = cand;
                    found = true;
                }
            }
            if (!found)
            {
#ifdef KNX_LOG_TUNNELING
                println("no free unique tunnelling IA available");
#endif
                tunIdx = 0xFF;
                tun = nullptr;
                paNotUnique = true; // list depleted -> report 0x25 (E_NO_MORE_UNIQUE_CONNECTIONS)
            }
            else
                tun->IndividualAddress = tunPa;
        }
    }

    if (tun == nullptr)
    {
        // KNX 03_08_04 Tunnelling §2.2.2 p.6-7: a device offering >1 tunnelling connection must distinguish "no free slot"
        // (E_NO_MORE_CONNECTIONS 0x24) from "slot free but the assignable tunnelling IA is not unique"
        // (E_NO_MORE_UNIQUE_CONNECTIONS 0x25).
        println(paNotUnique ? "tunnel connect rejected: no unique individual address available"
                            : "no free tunnel availible");
        KnxIpConnectResponse connRes(0x00, paNotUnique ? E_NO_MORE_UNIQUE_CONNECTIONS : E_NO_MORE_CONNECTIONS);
        sendCounted(rIp, rPort, connRes.data(), connRes.totalLength());
        return;
    }

    // the channel ID shall be unique on this tunnel server. catch the rare case of a double channel ID
    bool channelIdInUse;
    do
    {
        _lastChannelId++;
        channelIdInUse = false;
        // KNX 03_08_02 (Core) §5.3.3 p.13: the communication channel id must be unique across ALL
        // connections, incl. the device-management slots (the busmon path already scans the full range) -
        // otherwise a data tunnel and a devmgmt connection could share an id.
        for (int x = 0; x < KNX_TUNNELING + KNX_TUNNELING_DEVMGMT; x++)
            if (tunnels[x].ChannelId == _lastChannelId)
                channelIdInUse = true;
#ifdef OPENKNX_HW_BUSMON
        // The busmon connection's id lives in _busMonTunnel, outside tunnels[] -> include it so a wrapped
        // _lastChannelId can never collide with an active busmon channel (03_08_02 Core §5.3.3 uniqueness).
        if (_busMonTunnel.ChannelId != 0 && _busMonTunnel.ChannelId == _lastChannelId)
            channelIdInUse = true;
#endif
    } while (channelIdInUse);

    tun->ChannelId = _lastChannelId;
    tun->lastHeartbeat = millis();
    if (_lastChannelId == 255)
        _lastChannelId = 0;

    // NAT route-back (03_08_02 §8.6.2.2): substitute the UDP source PER FIELD (like the control endpoint +
    // connect paths). Deciding per-endpoint carried a port of 0 into the data endpoint -> indications to
    // port 0 (TSSH 5.4.5 failed). A fully specified HPAI and a full 0.0.0.0:0 route-back stay unchanged.
    tun->IpAddress = connRequest.hpaiData().ipAddress() ? connRequest.hpaiData().ipAddress() : src_addr;
    tun->PortData = connRequest.hpaiData().ipPortNumber() ? connRequest.hpaiData().ipPortNumber() : src_port;
    tun->PortCtrl = connRequest.hpaiCtrl().ipPortNumber() ? connRequest.hpaiCtrl().ipPortNumber() : srcPort;

    tun->connectStart = millis(); // start of this session (for duration in the history)

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
    sendCounted(tun->IpAddress, tun->PortCtrl, connRes.data(), connRes.totalLength());
}

void IpTunnelServer::HandleConnectionStateRequest(uint8_t* buffer, uint16_t length)
{
    KnxIpStateRequest stateRequest(buffer, length);

    KnxIpTunnelConnection* tun = nullptr;
    for (int i = 0; i < KNX_TUNNELING + KNX_TUNNELING_DEVMGMT; i++)
    {
        // channel 0 marks an unused slot -> never matches as an open channel
        if (tunnels[i].ChannelId != 0 && tunnels[i].ChannelId == stateRequest.channelId())
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
        // Echo the requested (unknown) channel id in the error, not 0 (03_08_04 §7.8.3; both refs do this).
        KnxIpStateResponse stateRes(stateRequest.channelId(), E_CONNECTION_ID);
        sendCounted(stateRequest.hpaiCtrl().ipAddress(), stateRequest.hpaiCtrl().ipPortNumber(), stateRes.data(), stateRes.totalLength());
        return;
    }

    // Set knxState to E_KNX_CONNECTION while the TP bus is not usable (bus down / chip absent), so the client
    // sees "tunnel up, bus down" instead of a false-healthy heartbeat. busOperational() covers both the
    // host<->chip link and the NCN bus-voltage bit (an externally-powered NCN keeps the link up when the bus
    // power drops); an active busmon counts as usable. The driver debounces all of it -> no transient false alarm.
    uint8_t knxState = E_NO_ERROR;
#ifdef OPENKNX_HW_BUSMON
    if (_hwBusMon && !_hwBusMon->hwBusOperational())
        knxState = E_KNX_CONNECTION;
#endif

    // No E_DATA_CONNECTION: an IP data-channel fault sits on the very path this reply travels -> undeliverable by
    // construction (link down = the request never arrives and the reply never leaves); the client reaps such a
    // dead IP path via its heartbeat timeout instead. E_KNX_CONNECTION differs: the bus fault is behind us while IP
    // still carries the reply.

    tun->lastHeartbeat = millis();
    KnxIpStateResponse stateRes(tun->ChannelId, knxState);
    // Route-back (03_08_02 Core §5.2): a heartbeat may carry HPAI 0.0.0.0:0 -> reply to the stored control
    // endpoint (resolved from the packet source at CONNECT), else the response is lost and the client reaps.
    uint32_t rIp = stateRequest.hpaiCtrl().ipAddress() ? stateRequest.hpaiCtrl().ipAddress() : tun->IpAddress;
    uint16_t rPort = stateRequest.hpaiCtrl().ipPortNumber() ? stateRequest.hpaiCtrl().ipPortNumber() : tun->PortCtrl;
    sendCounted(rIp, rPort, stateRes.data(), stateRes.totalLength());
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
        // channel 0 marks an unused slot -> never matches as an open channel
        if (tunnels[i].ChannelId != 0 && tunnels[i].ChannelId == discReq.channelId())
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
        // Route-back (03_08_02 Core §5.2): a route-back client's control HPAI is 0.0.0.0:0 -> reply to the
        // stored busmon control endpoint (mirrors the data/config disconnect path below).
        uint32_t rIp = discReq.hpaiCtrl().ipAddress() ? discReq.hpaiCtrl().ipAddress() : _busMonTunnel.IpAddress;
        uint16_t rPort = discReq.hpaiCtrl().ipPortNumber() ? discReq.hpaiCtrl().ipPortNumber() : _busMonTunnel.PortCtrl;
        sendCounted(rIp, rPort, discRes.data(), discRes.totalLength());
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
        // Echo the requested (unknown) channel id back in the error, not 0 (03_08_04 §7.8.4 / TSSH 3.6.2).
        KnxIpDisconnectResponse discRes(discReq.channelId(), E_CONNECTION_ID);
        sendCounted(discReq.hpaiCtrl().ipAddress(), discReq.hpaiCtrl().ipPortNumber(), discRes.data(), discRes.totalLength());
        return;
    }

    KnxIpDisconnectResponse discRes(tun->ChannelId, E_NO_ERROR);
    // Route-back (03_08_02 Core §5.2): reply to the stored control endpoint when the request HPAI is 0.0.0.0:0.
    uint32_t rIp = discReq.hpaiCtrl().ipAddress() ? discReq.hpaiCtrl().ipAddress() : tun->IpAddress;
    uint16_t rPort = discReq.hpaiCtrl().ipPortNumber() ? discReq.hpaiCtrl().ipPortNumber() : tun->PortCtrl;
    sendCounted(rIp, rPort, discRes.data(), discRes.totalLength());
    recordTunnelSession(tun->IpAddress, tun->IndividualAddress, tun->IsConfig ? TUN_CONFIG : TUN_DATA, tun->connectStart, END_CLOSED);
    tun->Reset();
}

void IpTunnelServer::HandleDescriptionRequest(uint8_t* buffer, uint16_t length)
{
    KnxIpDescriptionRequest descReq(buffer, length);
    KnxIpDescriptionResponse descRes(_ipParameters, _deviceObject);
    sendCounted(descReq.hpaiCtrl().ipAddress(), descReq.hpaiCtrl().ipPortNumber(), descRes.data(), descRes.totalLength());
}

void IpTunnelServer::HandleDeviceConfigurationRequest(uint8_t* buffer, uint16_t length)
{
    KnxIpConfigRequest confReq(buffer, length);

    KnxIpTunnelConnection* tun = nullptr;
    for (int i = KNX_TUNNELING; i < KNX_TUNNELING + KNX_TUNNELING_DEVMGMT; i++)
    {
        // channel 0 marks an unused slot -> never matches as an open channel (no M_Prop/T_Data without a connection)
        if (tunnels[i].ChannelId != 0 && tunnels[i].ChannelId == confReq.connectionHeader().channelId())
        {
            tun = &tunnels[i];
            break;
        }
    }

    if (tun == nullptr)
    {
#ifdef KNX_LOG_TUNNELING
        print("Channel ID nicht gefunden: ");
        println(confReq.connectionHeader().channelId());
#endif
        // KNX 03_08_02 Core §5.5 p.14: unknown communication channel id -> silently ignore (no malformed reply to 0.0.0.0:0).
        return;
    }

    // KNX 03_08_03 Management p.7 + 03_08_02 Core §5.3.4/§5.4: evaluate the sequence counter exactly like the
    // tunnelling path. A duplicate (client retransmit after a lost ACK) must be re-ACK'd but NOT re-processed --
    // otherwise an M_PropWrite (e.g. ETS writing the IP config) runs twice; an unexpected sequence must be
    // discarded WITHOUT an ACK and WITHOUT retriggering the 120 s timeout.
    uint8_t sequence = confReq.connectionHeader().sequenceCounter();
    if (sequence == tun->SequenceCounter_R)
    {
        // already received -> just re-ACK, no reprocess, no heartbeat retrigger
        KnxIpTunnelingAck dupAck;
        dupAck.serviceTypeIdentifier(DeviceConfigurationAck);
        dupAck.connectionHeader().length(4);
        dupAck.connectionHeader().channelId(tun->ChannelId);
        dupAck.connectionHeader().sequenceCounter(sequence);
        dupAck.connectionHeader().status(E_NO_ERROR);
        sendCounted(tun->IpAddress, tun->PortData, dupAck.data(), dupAck.totalLength());
        return;
    }
    else if ((uint8_t)(sequence - 1) != tun->SequenceCounter_R)
    {
        // unexpected sequence -> discard, no ACK, no heartbeat retrigger
        return;
    }

    KnxIpTunnelingAck tunnAck;
    tunnAck.serviceTypeIdentifier(DeviceConfigurationAck);
    tunnAck.connectionHeader().length(4);
    tunnAck.connectionHeader().channelId(tun->ChannelId);
    tunnAck.connectionHeader().sequenceCounter(sequence);
    tunnAck.connectionHeader().status(E_NO_ERROR);
    sendCounted(tun->IpAddress, tun->PortData, tunnAck.data(), tunnAck.totalLength());

    tun->SequenceCounter_R = sequence;
    tun->lastHeartbeat = millis();
    _cemiServer.frameReceived(confReq.frame(), tun->ChannelId);
}

void IpTunnelServer::HandleTunnelingRequest(uint8_t* buffer, uint16_t length)
{
    KnxIpTunnelingRequest tunnReq(buffer, length);

    KnxIpTunnelConnection* tun = nullptr;
    for (int i = 0; i < KNX_TUNNELING; i++)
    {
        // channel 0 marks an unused slot -> never matches as an open channel (no L_Data without a connection)
        if (tunnels[i].ChannelId != 0 && tunnels[i].ChannelId == tunnReq.connectionHeader().channelId())
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
        // KNX 03_08_02 Core §5.5 p.14: an unknown communication channel id is silently ignored (no reply). The previous
        // code sent a malformed KnxIpStateResponse to 0.0.0.0:0 (wrong service type + null endpoint).
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
        sendCounted(tun->IpAddress, tun->PortData, tunnAck.data(), tunnAck.totalLength());
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
    sendCounted(tun->IpAddress, tun->PortData, tunnAck.data(), tunnAck.totalLength());

    tun->SequenceCounter_R = tunnReq.connectionHeader().sequenceCounter();
    tun->lastHeartbeat = millis(); // KNX 03_08_02 Core §5.4 p.14: any correctly received frame retriggers the 120s timer

    // KNX 03_06_03 (cEMI) §4.1.5 p.19: validate the tunnelled cEMI before it is put on the TP bus. A malformed
    // frame from an open tunnel (buggy or hostile client) would otherwise be parsed further down (OOB reads on
    // apduLength/addInfo) and could emit garbage onto TP. The datagram itself is already ACK'd above (transport
    // receipt per KNX 03_08_04 §2.6 p.9) and the R-counter advanced; we only drop the invalid L_Data payload.
    // Order is load-bearing and mirrors the proven routing-indication gate (ip_data_link_layer.cpp): the
    // totalLenght()!=0 test MUST short-circuit first, because valid()'s OOB bounds-check only self-bounds for
    // _length!=0 -- on a zero-length cEMI valid() would itself OOB-read _data[_data[1]+8]. Validate BEFORE the
    // source-address rewrite below so no cEMI field is read/written on an unvalidated frame.
    if (tunnReq.frame().totalLenght() == 0 || !tunnReq.frame().valid())
    {
#ifdef KNX_LOG_TUNNELING
        println("Tunnelling: dropping invalid cEMI frame (not forwarded to TP)");
#endif
        return;
    }

    if (tunnReq.frame().sourceAddress() == 0)
        tunnReq.frame().sourceAddress(tun->IndividualAddress);

    _cemiServer.frameReceived(tunnReq.frame(), tun->ChannelId);
}

#ifdef OPENKNX_HW_BUSMON
void IpTunnelServer::closeTunnelsForBusmon()
{
    // Disconnect every open data/config tunnel and record it as ended-by-busmon. Idempotent: with no open
    // tunnels (e.g. an ETS busmon already cleared them) the loop is a no-op. Shared by the ETS busmon connect
    // and the local console `bcu mon` toggle so both make the busmonitor equally exclusive.
    for (int i = 0; i < KNX_TUNNELING + KNX_TUNNELING_DEVMGMT; i++)
    {
        if (tunnels[i].ChannelId == 0) continue;
        KnxIpDisconnectRequest discReq;
        discReq.channelId(tunnels[i].ChannelId);
        discReq.hpaiCtrl().length(LEN_IPHPAI);
        discReq.hpaiCtrl().code(IPV4_UDP);
        discReq.hpaiCtrl().ipAddress(tunnels[i].IpAddress);
        discReq.hpaiCtrl().ipPortNumber(tunnels[i].PortCtrl);
        sendCounted(tunnels[i].IpAddress, tunnels[i].PortCtrl, discReq.data(), discReq.totalLength());
        recordTunnelSession(tunnels[i].IpAddress, tunnels[i].IndividualAddress, tunnels[i].IsConfig ? TUN_CONFIG : TUN_DATA, tunnels[i].connectStart, END_BUSMON);
        tunnels[i].Reset();
    }
}

void IpTunnelServer::HandleBusMonitorConnect(KnxIpConnectRequest& connRequest, uint32_t src_addr, uint16_t src_port)
{
    uint32_t srcIP = connRequest.hpaiCtrl().ipAddress() ? connRequest.hpaiCtrl().ipAddress() : src_addr;
    uint16_t srcPort = connRequest.hpaiCtrl().ipPortNumber() ? connRequest.hpaiCtrl().ipPortNumber() : src_port;

    // Single busmonitor connection only.
    if (_busMonTunnel.ChannelId != 0 || _hwBusMon == nullptr)
    {
        KnxIpConnectResponse connRes(0x00, _hwBusMon == nullptr ? E_TUNNELING_LAYER : E_NO_MORE_CONNECTIONS);
        sendCounted(srcIP, srcPort, connRes.data(), connRes.totalLength()); // route-back (srcIP/srcPort resolved at fn top)
        return;
    }

    // KNX 03_08_04 Tunnelling §2.2.4 p.8: a busmonitor connection is exclusive per subnetwork -> close any
    // open data/config tunnels (e.g. a running group monitor) so the busmonitor becomes the only connection.
    closeTunnelsForBusmon();

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
    _busMonSeq = 0; // restart the cEMI L_Busmon.ind status-octet sequence counter for the new busmon session
    _busMonTunnel.lastHeartbeat = millis();
    _busMonTunnel.ChannelId = _lastChannelId; // set last -> busMonitorActive() true only once fully set up
    _busMonTunnel.connectStart = millis();
    _busMonExitPending = false;

    _hwBusMon->hwBusMonEnter(); // U_BUSMON_REQ -> chip passive, routing paused

    print("New HW-Busmon connection, Channel: 0x");
    print(_busMonTunnel.ChannelId, 16);
    println(" (routing paused until disconnect)");

    KnxIpConnectResponse connRes(_ipParameters, _deviceObject.individualAddress(), 3671, _busMonTunnel.ChannelId, TUNNEL_CONNECTION);
    sendCounted(_busMonTunnel.IpAddress, _busMonTunnel.PortCtrl, connRes.data(), connRes.totalLength());
}

void IpTunnelServer::busMonitorTeardown(uint8_t reason)
{
    if (_busMonTunnel.ChannelId == 0)
        return;

    recordTunnelSession(_busMonTunnel.IpAddress, 0, TUN_BUSMON, _busMonTunnel.connectStart, reason);
    _busMonTunnel.Reset(); // stop forwarding at once (busMonitorActive() -> false)
    if (_hwBusMon)
    {
        // hwBusMonExit() returns true only if it ACTUALLY left monitor mode (reset the chip). If a local
        // console busmon (`bcu mon`) still owns it, it keeps the chip monitoring and returns false -> no
        // recovery is due, so we must not arm the "did the chip come back?" watchdog (it would fire a false
        // "NCN latch" warning while the console busmon legitimately keeps the chip passive). A genuine
        // ETS-only teardown returns true, so a real latch is still caught by the poll below.
        if (_hwBusMon->hwBusMonExit())
        {
            _busMonExitPending = true; // bounded, non-blocking recovery poll in loop()
            _busMonExitStart = millis();
        }
    }
}

void IpTunnelServer::busMonitorFrame(uint8_t* lpdu, uint16_t len, uint8_t status)
{
    if (_busMonTunnel.ChannelId == 0 || len == 0)
        return;

    // Max raw TP1 LPDU incl. FCS: extended frame = 9 metadata bytes (incl. FCS) + up to 255 LSDU = 264.
    // Compare len DIRECTLY (never (uint16_t)(9+len), which wraps for a corrupt oversize len) and size the
    // buffer to the 9-byte cEMI header + that max, so the largest legal frame fits and no overrun is possible.
    constexpr uint16_t MAX_LPDU = 264;
    constexpr uint16_t HDR = 11; // MC(1) + AddIL(1) + status AI(3) + extended-timestamp AI(6)
    if (len > MAX_LPDU)
        return;

    // cEMI L_Busmon.ind (03_06_03 §4.1.5.8.1, p.96): [0x2B][AddIL=9][AI 0x03,len1,status/seq]
    // [AI 0x06,len4,ext-rel-timestamp][raw LPDU incl FCS]. AddIL 9 = status AI(3) + ext-timestamp AI(6);
    // AI fields ascending by type ID (§4.1.4.3.1). We use the EXTENDED relative timestamp (type 0x06,
    // 4 octets, §4.1.4.3.3) rather than the legacy 2-octet 0x04: 0x06 pairs with PID_TIME_BASE (ns/tick)
    // on the cEMI server object, so the client computes real inter-frame times instead of raw ticks
    // (§4.1.4.3.3 calls out that 0x04 forces the tool to guess the clock). Counter = micros() latched at
    // forward time -> 1 tick = 1 us -> PID_TIME_BASE = 1000 ns.
    const uint32_t ts = micros();
    uint8_t buf[HDR + MAX_LPDU];
    buf[0] = L_busmon_ind; // 0x2B
    buf[1] = 0x09;         // additional info length (status AI + extended-timestamp AI)
    buf[2] = 0x03;         // AI type: bus monitor status
    buf[3] = 0x01;         // AI value length
    // KNX 03_06_03 EMI §3.3.3.2: status octet F B P x L sss -- bits 0-2 = seq (mod 8), bit 3 = lost,
    // bit 5 = parity error, bit 6 = bit error, bit 7 = frame error. seq is ours; error/lost bits from caller.
    buf[4] = (uint8_t)((_busMonSeq++ & 0x07) | (status & 0xF8));
    buf[5] = 0x06;                 // AI type: extended relative timestamp
    buf[6] = 0x04;                 // AI value length (4 octets)
    buf[7]  = (uint8_t)(ts >> 24); // big-endian (network order)
    buf[8]  = (uint8_t)(ts >> 16);
    buf[9]  = (uint8_t)(ts >> 8);
    buf[10] = (uint8_t)(ts & 0xFF);
    memcpy(buf + HDR, lpdu, len);

    CemiFrame frame(buf, HDR + len);
    KnxIpTunnelingRequest req(frame); // ctor sets serviceTypeIdentifier(TunnelingRequest)
    req.connectionHeader().sequenceCounter(_busMonTunnel.SequenceCounter_S++);
    req.connectionHeader().length(LEN_CH);
    req.connectionHeader().channelId(_busMonTunnel.ChannelId);
    sendCounted(_busMonTunnel.IpAddress, _busMonTunnel.PortData, req.data(), req.totalLength());
}
#endif

#endif
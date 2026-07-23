#pragma once
#include "config.h"
#include "platform.h"
#include "bits.h"

class KnxIpTunnelConnection
{
  public:
    KnxIpTunnelConnection();
    uint8_t ChannelId = 0;
    uint16_t IndividualAddress = 0;
    uint32_t IpAddress = 0;
    uint16_t PortData = 0;
    uint16_t PortCtrl = 0;
    uint8_t SequenceCounter_S = 0;
    uint8_t SequenceCounter_R = 255;
    unsigned long lastHeartbeat = 0;
    unsigned long connectStart = 0; // millis() when this connection was established (for session duration)
    bool IsConfig = false;

#ifdef KNX_TUNNEL_RESEND
    // Server->client TUNNELLING_REQUEST reliability (KNX 03_08_04 Tunnelling §2.6.1 p.9): a per-tunnel FIFO
    // of the datagrams still to send. Exactly one is on the wire at a time (the head); it is resent verbatim
    // (same seq) after 1 s and the tunnel is disconnected on the 2nd timeout. Queueing (never dropping) is
    // required because the device's own connection-oriented responses arrive in local bursts, faster than
    // the client acks -- dropping them (the earlier single-slot design) collapsed reading/programming.
    #ifndef KNX_TUNNEL_RESEND_BUF
        // Must hold the LARGEST tunnelling datagram, incl. extended frames (memory read/write, long property
        // responses during programming): KNXnet/IP hdr 6 + connection hdr 4 + cEMI (<= ~9 + maxAPDU 254) ~= 273.
        // Undersizing this would silently drop large frames and re-break device read/programming.
        #define KNX_TUNNEL_RESEND_BUF 280
    #endif
    #ifndef KNX_TUNNEL_RESEND_DEPTH
        // FIFO slots per connection. The device's connection-oriented layer is window-1 and the client acks in
        // ms, so the realistic queue depth is 1-2; 3 gives margin. A stuck client that overruns it is disconnected.
        #define KNX_TUNNEL_RESEND_DEPTH 3
    #endif
    uint8_t _txBuf[KNX_TUNNEL_RESEND_DEPTH][KNX_TUNNEL_RESEND_BUF]; // full datagrams, resent verbatim
    uint16_t _txLen[KNX_TUNNEL_RESEND_DEPTH] = {0}; // used bytes per slot (<= BUF=280); MUST be 16-bit -- an
                                                    // extended-frame datagram (~273 B) truncated in a uint8_t
                                                    // would make pumpTunnel send the wrong length
    uint8_t _txHead = 0;    // slot in flight / next to send
    uint8_t _txTail = 0;    // next free slot to enqueue
    uint8_t _txCount = 0;   // queued slots (0..DEPTH)
    bool _armed = false;    // head is on the wire, awaiting ACK
    uint8_t _seq = 0;       // sequence counter stamped into the in-flight head
    uint8_t _retries = 0;   // 0 = original send, 1 = repeat already sent
    uint32_t _sentAt = 0;   // millis() of the last (re)send of the head
#endif

    void Reset();

  private:

};
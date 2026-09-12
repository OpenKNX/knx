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
    unsigned long connectStart = 0; // millis() when this connection was established (wall-clock stamp)
    uint32_t ConnectUptimeS = 0;    // uptime seconds at connect; the duration is derived from this
    bool IsConfig = false;
    // Which slot the reservation table held for this client at connect time, 0xFF for none. Recorded
    // here because only the connect knows it: the reservation is matched against the CONTROL HPAI,
    // while IpAddress above is the DATA HPAI, and the two need not carry the same address.
    uint8_t ReservedSlot = 0xFF;

    // Per-session counters for the diagnostics UI. Kept for the whole session and copied into the
    // history on disconnect. StatFromClient is what was ACCEPTED from the client, not what reached TP:
    // a self-addressed or tunnel-PA frame is answered locally and never put on the bus.
    uint32_t StatToClient = 0;  // requests put on the wire for this client (resends not counted again)
    uint32_t StatFromClient = 0;
    uint16_t StatResend = 0;    // repeats of an unacked request
    uint16_t StatSeqGap = 0;    // datagrams discarded: sequence counter was not the expected one
    uint16_t StatTxDrop = 0;    // frames for this client that were never sent: oversize, FIFO full,
                                // still queued at teardown, or refused with no retry behind it
    uint16_t StatGrpDrop = 0;   // group frames dropped because the FIFO was full -- best-effort by design
                                // (03_08_04 2.6.1), kept apart so one does not paint a loaded tunnel red
    uint8_t StatQueuePeak = 0;  // deepest send-FIFO fill reached (stays 0 without KNX_TUNNEL_RESEND)

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
    bool _headSent = false; // the head left the device at least once (first try or a repeat)
    uint8_t _seq = 0;       // sequence counter stamped into the in-flight head
    uint8_t _retries = 0;   // repeats already sent for the in-flight head (0 = original only; max 1 data / 3 config)
    uint32_t _sentAt = 0;   // millis() of the last (re)send of the head
#endif

    void Reset();

  private:

};
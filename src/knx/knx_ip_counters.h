#pragma once

#include <stdint.h>

/**
 * @brief KNXnet/IP parameter object counters (03_08_03 2.5.23-2.5.26); PID counters saturate, lostToIp/lostToKnx wrap, the others are ours, not spec.
 * PID 72/73 (2.5.23/2.5.24): KNX telegrams finally lost: send limit, queue full, failed send, link down, oversize tunnel frame, allocation failure; to KNX also TP chip down.
 * Not counted: control datagrams, filtered telegrams, frames queued behind a tunnel head (txDrop).
 */
class KnxIpCounters
{
  public:
    void incrementTransmitToIp() { inc32(_transmitToIp); }
    void incrementTransmitToKnx() { inc32(_transmitToKnx); }
    void incrementOverflowToIp() { inc16(_overflowToIp); _lostToIp = _lostToIp + 1; }
    void addOverflowToIp(uint8_t n) { add16(_overflowToIp, n); _lostToIp = _lostToIp + n; }
    void incrementOverflowToKnx() { inc16(_overflowToKnx); _lostToKnx = _lostToKnx + 1; }
    void incrementRoutedToIp() { inc32(_routedToIp); }
    void incrementRoutedToKnx() { inc32(_routedToKnx); }
    void incrementFilteredToIp() { inc32(_filteredToIp); }
    void incrementFilteredToKnx() { inc32(_filteredToKnx); }
    // Telegrams that arrived with hop count 0: the coupler must not pass them on. Not a spec counter
    // and nothing else counts them, so a vanished telegram had no trace at all before.
    void incrementHopCountToIp() { inc32(_hopCountToIp); }
    void incrementHopCountToKnx() { inc32(_hopCountToKnx); }
    void incrementLostMessageRx(uint16_t lost) { inc32(_lostMessageRx); add32(_lostReportedRx, lost); }
    void incrementBusyRx() { inc32(_busyRx); }

    uint32_t transmitToIp() const { return _transmitToIp; }
    uint32_t transmitToKnx() const { return _transmitToKnx; }
    uint16_t overflowToIp() const { return (uint16_t)_overflowToIp; }
    uint16_t overflowToKnx() const { return (uint16_t)_overflowToKnx; }
    uint32_t lostToKnx() const { return _lostToKnx; }
    uint32_t lostToIp() const { return _lostToIp; }
    uint32_t routedToIp() const { return _routedToIp; }
    uint32_t routedToKnx() const { return _routedToKnx; }
    uint32_t filteredToIp() const { return _filteredToIp; }
    uint32_t filteredToKnx() const { return _filteredToKnx; }
    uint32_t hopCountToIp() const { return _hopCountToIp; }
    uint32_t hopCountToKnx() const { return _hopCountToKnx; }
    uint32_t lostMessageRx() const { return _lostMessageRx; }
    uint32_t lostReportedRx() const { return _lostReportedRx; }
    uint32_t busyRx() const { return _busyRx; }

  private:
    // 03_08_03: "The count does not wrap around when the maximum is reached."
    static void inc32(volatile uint32_t& v) { const uint32_t c = v; if (c < 0xFFFFFFFF) v = c + 1; }
    static void inc16(volatile uint32_t& v) { const uint32_t c = v; if (c < 0xFFFF) v = c + 1; }
    static void add32(volatile uint32_t& v, uint16_t n) { const uint32_t c = v; v = (c > 0xFFFFFFFF - n) ? 0xFFFFFFFF : c + n; }
    static void add16(volatile uint32_t& v, uint8_t n) { const uint32_t c = v + n; v = (c > 0xFFFF) ? 0xFFFF : c; }

    volatile uint32_t _transmitToIp = 0;  // PID 74: every KNXnet/IP datagram sent, ACKs included
    volatile uint32_t _transmitToKnx = 0; // PID 75: telegrams put on the KNX subnetwork
    volatile uint32_t _overflowToIp = 0;
    volatile uint32_t _overflowToKnx = 0;
    volatile uint32_t _lostToKnx = 0; // wraps on purpose; ROUTING_LOST_MESSAGE reports its delta (03_08_05 2.3.4)
    volatile uint32_t _lostToIp = 0;  // wraps on purpose: only differences are read
    volatile uint32_t _routedToIp = 0;
    volatile uint32_t _routedToKnx = 0;
    volatile uint32_t _filteredToIp = 0;
    volatile uint32_t _filteredToKnx = 0;
    volatile uint32_t _hopCountToIp = 0;
    volatile uint32_t _hopCountToKnx = 0;
    volatile uint32_t _lostMessageRx = 0;
    volatile uint32_t _lostReportedRx = 0;
    volatile uint32_t _busyRx = 0;
};

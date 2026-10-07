#pragma once

#include <stdint.h>

/**
 * @brief KNXnet/IP telegram counters of the KNXnet/IP parameter object (03_08_03 2.5.23-2.5.26).
 *
 * Spec semantics, verified against the standard and deliberately NOT "routed telegrams":
 *  - transmitToIp  (PID 74, PDT_UNSIGNED_LONG): every KNXnet/IP datagram of any kind put on the IP
 *    network -- Core, Tunnelling, Routing, Device Management, ACKs included.
 *  - transmitToKnx (PID 75, PDT_UNSIGNED_LONG): telegrams a router put on the KNX subnetwork.
 *  - overflowToIp/ToKnx (PID 72/73, PDT_UNSIGNED_INT): telegrams LOST on the way to that side (send
 *    limit, queue full; on the router also IP down or IP fault, TP chip not connected, allocation
 *    failure, queue dropped by a chip reset). Filtered telegrams are not losses and are not counted in PID 72/73.
 *
 * The spec requires the counts not to wrap; all four saturate at their property width.
 * routedToIp/ToKnx, filteredToIp/ToKnx and hopCountToIp/ToKnx are ours, not spec: the coupler's
 * routing decision per direction, for the console and the display.
 * lostMessageRx/lostReportedRx/busyRx are ours too: ROUTING_LOST_MESSAGE and ROUTING_BUSY received
 * from other devices, and the sum of their NumberOfLostMessages fields (03_08_05 5.3/5.4 p.16-17).
 * lostToKnx is the wrapping twin of PID 73; ROUTING_LOST_MESSAGE reports its delta (03_08_05 2.3.4 p.10).
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

    volatile uint32_t _transmitToIp = 0;
    volatile uint32_t _transmitToKnx = 0;
    volatile uint32_t _overflowToIp = 0;
    volatile uint32_t _overflowToKnx = 0;
    volatile uint32_t _lostToKnx = 0; // wraps on purpose: only differences are read
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

#pragma once

#include <stdint.h>

/**
 * @brief KNXnet/IP telegram counters of the KNXnet/IP parameter object (03_08_03 2.5.23-2.5.26).
 *
 * Spec semantics, verified against the standard and deliberately NOT "routed telegrams":
 *  - transmitToIp  (PID 74, PDT_UNSIGNED_LONG): every KNXnet/IP datagram of any kind put on the IP
 *    network -- Core, Tunnelling, Routing, Device Management, ACKs included.
 *  - transmitToKnx (PID 75, PDT_UNSIGNED_LONG): telegrams a router put on the KNX subnetwork.
 *  - overflowToIp/ToKnx (PID 72/73, PDT_UNSIGNED_INT): telegrams LOST because that side refused
 *    them (send limit reached, transmit queue full). Filtered telegrams are not losses and are
 *    counted here nowhere.
 *
 * The spec requires the counts not to wrap; all four saturate at their property width.
 * routedToIp/ToKnx, filteredToIp/ToKnx and hopCountToIp/ToKnx are ours, not spec: the coupler's
 * routing decision per direction, for the console and the display.
 */
class KnxIpCounters
{
  public:
    void incrementTransmitToIp() { inc32(_transmitToIp); }
    void incrementTransmitToKnx() { inc32(_transmitToKnx); }
    void incrementOverflowToIp() { inc16(_overflowToIp); }
    void incrementOverflowToKnx() { inc16(_overflowToKnx); }
    void incrementRoutedToIp() { inc32(_routedToIp); }
    void incrementRoutedToKnx() { inc32(_routedToKnx); }
    void incrementFilteredToIp() { inc32(_filteredToIp); }
    void incrementFilteredToKnx() { inc32(_filteredToKnx); }
    // Telegrams that arrived with hop count 0: the coupler must not pass them on. Not a spec counter
    // and nothing else counts them, so a vanished telegram had no trace at all before.
    void incrementHopCountToIp() { inc32(_hopCountToIp); }
    void incrementHopCountToKnx() { inc32(_hopCountToKnx); }

    uint32_t transmitToIp() const { return _transmitToIp; }
    uint32_t transmitToKnx() const { return _transmitToKnx; }
    uint16_t overflowToIp() const { return (uint16_t)_overflowToIp; }
    uint16_t overflowToKnx() const { return (uint16_t)_overflowToKnx; }
    uint32_t routedToIp() const { return _routedToIp; }
    uint32_t routedToKnx() const { return _routedToKnx; }
    uint32_t filteredToIp() const { return _filteredToIp; }
    uint32_t filteredToKnx() const { return _filteredToKnx; }
    uint32_t hopCountToIp() const { return _hopCountToIp; }
    uint32_t hopCountToKnx() const { return _hopCountToKnx; }

  private:
    // 03_08_03: "The count does not wrap around when the maximum is reached."
    static void inc32(volatile uint32_t& v) { const uint32_t c = v; if (c < 0xFFFFFFFF) v = c + 1; }
    static void inc16(volatile uint32_t& v) { const uint32_t c = v; if (c < 0xFFFF) v = c + 1; }

    volatile uint32_t _transmitToIp = 0;
    volatile uint32_t _transmitToKnx = 0;
    volatile uint32_t _overflowToIp = 0;
    volatile uint32_t _overflowToKnx = 0;
    volatile uint32_t _routedToIp = 0;
    volatile uint32_t _routedToKnx = 0;
    volatile uint32_t _filteredToIp = 0;
    volatile uint32_t _filteredToKnx = 0;
    volatile uint32_t _hopCountToIp = 0;
    volatile uint32_t _hopCountToKnx = 0;
};

#pragma once

// Routing diagnostics for a coupler: the last decisions as a list, plus a long-running count per
// group address. Both exist only with OPENKNX_ROUTE_TRACE, which no product but the IP router sets --
// without it this header defines nothing and costs no byte anywhere.

#ifdef OPENKNX_ROUTE_TRACE

    #include <stdint.h>
    #include <string.h>

    #include "bits.h" // millis()

class RouteTrace
{
  public:
    // What the coupler did with the telegram. HOPCOUNT and the two PHYS cases have no counter of
    // their own anywhere -- the list is the only place they become visible.
    enum Decision : uint8_t
    {
        ROUTED = 0,
        FILTERED = 1,      // group address not in the filter table
        HOPCOUNT = 2,      // arrived with hop count 0: it has used up its couplers
        PHYS_LOCKED = 3,   // individual address, forwarding locked by configuration
        PHYS_NOT_ROUTED = 4 // individual address outside the routed range
    };

    static const uint8_t TRACE_SIZE = 32;
    static const uint8_t TOP_SIZE = 16; // twice the eight shown, so the tail cannot squat the table

    struct Entry
    {
        uint32_t at;   // millis() of the decision
        uint16_t dst;  // group or individual address, as addressed
        uint16_t src;  // sender
        uint8_t flags; // decision | toIp << 3 | group << 4 | hop << 5
    };

    // Field order matters: count first packs the struct into 8 bytes instead of 12.
    struct Top
    {
        uint32_t count;
        uint16_t ga;
        bool toIp;
    };

    /** @brief One decision. Called from the routing path, so it only stores. */
    void record(Decision dec, bool toIp, bool group, uint8_t hop, uint16_t src, uint16_t dst)
    {
        Entry &e = _entries[_next];
        e.at = _now();
        e.dst = dst;
        e.src = src;
        e.flags = (uint8_t)(dec | (toIp ? 0x08 : 0) | (group ? 0x10 : 0) | ((hop & 0x07) << 5));
        _next = (uint8_t)((_next + 1) % TRACE_SIZE);
        if (_count < TRACE_SIZE) _count++;

        // Only the two decisions a group address can be counted under. A hop-count drop is not a
        // forward, and destination 0 is broadcast, not a routable address.
        if (group && dst != 0 && (dec == ROUTED || dec == FILTERED))
            bump(dec == FILTERED ? _topFiltered : _topRouted, dst, toIp);
    }

    uint8_t traceCount() const { return _count; }

    // Both readers copy: on the ESP32 the web request runs in the httpd task while the routing path
    // writes from the main loop, and a returned pointer could be overwritten while it is being read.
    /** @brief i-th entry, 0 = newest; false when out of range. */
    bool traceAt(uint8_t i, Entry &out) const
    {
        if (i >= _count) return false;
        out = _entries[(uint8_t)((_next + TRACE_SIZE - 1 - i) % TRACE_SIZE)];
        return true;
    }

    bool topAt(bool filtered, uint8_t i, Top &out) const
    {
        if (i >= TOP_SIZE) return false;
        out = (filtered ? _topFiltered : _topRouted)[i];
        return out.count != 0;
    }

    /** @brief Clear the long-running counts (the list keeps running). */
    void resetTop()
    {
        memset(_topFiltered, 0, sizeof(_topFiltered));
        memset(_topRouted, 0, sizeof(_topRouted));
        _topSince = _now();
    }

    uint32_t topSince() const { return _topSince; }

  private:
    // Space-Saving: on a miss the smallest counter is taken over, so a heavy hitter that starts late
    // still climbs. A plain "first come, first served" table would freeze after the first minute.
    static void bump(Top *tab, uint16_t ga, bool toIp)
    {
        uint8_t min = 0;
        for (uint8_t i = 0; i < TOP_SIZE; i++)
        {
            if (tab[i].count && tab[i].ga == ga && tab[i].toIp == toIp)
            {
                if (tab[i].count < 0xFFFFFFFF) tab[i].count++; // 0 means "slot free" - must not wrap
                return;
            }
            if (tab[i].count < tab[min].count) min = i;
        }
        const uint32_t taken = tab[min].count;
        tab[min].ga = ga;
        tab[min].toIp = toIp;
        tab[min].count = (taken < 0xFFFFFFFF) ? taken + 1 : taken;
    }

    static uint32_t _now() { return millis(); }

    Entry _entries[TRACE_SIZE] = {};
    Top _topFiltered[TOP_SIZE] = {};
    Top _topRouted[TOP_SIZE] = {};
    uint32_t _topSince = 0;
    uint8_t _next = 0;
    uint8_t _count = 0;
};

#endif // OPENKNX_ROUTE_TRACE

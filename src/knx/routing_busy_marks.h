/**
 * @file        routing_busy_marks.h
 * @brief       ROUTING_BUSY datagrams in a DatagramRing: sequence number of the last one, arrival time of the last four
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */
#pragma once

#include <stdint.h>

// The ring's producer records after its push; the consumer reads with the ring's popped count.
class RoutingBusyMarks
{
  public:
    /** @brief Producer, after the ROUTING_BUSY was pushed as record seq (DatagramRing::pushed()). */
    void record(uint32_t seq, uint32_t ms)
    {
        Mark& m = _marks[_next++ & (kMarks - 1)];
        // Seqlock write: ver is odd while seq and ms change.
        const uint32_t v = __atomic_load_n(&m.ver, __ATOMIC_RELAXED);
        __atomic_store_n(&m.ver, v + 1u, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        __atomic_store_n(&m.ms, ms, __ATOMIC_RELAXED);
        __atomic_store_n(&m.seq, seq, __ATOMIC_RELAXED);
        __atomic_store_n(&m.ver, v + 2u, __ATOMIC_RELEASE);
        if (_filled < kMarks)
            __atomic_store_n(&_filled, (uint8_t)(_filled + 1), __ATOMIC_RELEASE);
        __atomic_store_n(&_last, seq, __ATOMIC_RELEASE);
    }

    /**
     * @brief Consumer: records after the last read up to the last ROUTING_BUSY, minus one. A ROUTING_BUSY is unread
     * while this is below DatagramRing::backlog() loaded afterwards; after 2^32 records without one it reads unread once.
     */
    uint32_t ahead(uint32_t popped) const
    {
        return (uint32_t)(__atomic_load_n(&_last, __ATOMIC_ACQUIRE) - popped - 1u);
    }

    /**
     * @brief Consumer: arrival time of record seq; fallback (the later read time) when it is no ROUTING_BUSY held here
     * or a mark stays inconsistent for kReadTries reads.
     */
    uint32_t arrivedAt(uint32_t seq, uint32_t fallback) const
    {
        const uint8_t filled = __atomic_load_n(&_filled, __ATOMIC_ACQUIRE); // never-written marks hold no seq
        for (uint8_t i = 0; i < filled; i++)
        {
            const Mark& m = _marks[i];
            for (uint8_t t = 0; t < kReadTries; t++)
            {
                const uint32_t v = __atomic_load_n(&m.ver, __ATOMIC_ACQUIRE);
                const uint32_t s = __atomic_load_n(&m.seq, __ATOMIC_RELAXED);
                const uint32_t ms = __atomic_load_n(&m.ms, __ATOMIC_RELAXED);
                __atomic_thread_fence(__ATOMIC_ACQUIRE);
                if ((v & 1u) != 0 || __atomic_load_n(&m.ver, __ATOMIC_RELAXED) != v)
                    continue; // the producer wrote this mark meanwhile
                if (s == seq)
                    return ms;
                break;
            }
        }
        return fallback;
    }

  private:
    static constexpr uint8_t kMarks = 4;     // power of two
    static constexpr uint8_t kReadTries = 3; // a write takes a few stores; repeated overlaps fall back to the read time
    struct Mark
    {
        uint32_t ver = 0; // seqlock version, odd while written
        uint32_t seq = 0;
        uint32_t ms = 0;
    };
    Mark _marks[kMarks];
    uint32_t _last = 0;
    uint8_t _next = 0;
    uint8_t _filled = 0; // marks written so far, up to kMarks
};

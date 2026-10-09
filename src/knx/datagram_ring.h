/**
 * @file        datagram_ring.h
 * @brief       Single-producer single-consumer byte ring of received datagrams with their sender
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */
#pragma once

#include <stdint.h>
#include <string.h>

// Each side owns its index and publishes it with release ordering; a record may wrap at the buffer end.
template <uint32_t Size, uint16_t MaxPayload>
class DatagramRing
{
  public:
    static constexpr uint32_t kHeaderLen = 9; // length 2, source port 2, source address 4, destination class 1

    static_assert(Size != 0 && (Size & (Size - 1)) == 0, "Size must be a power of two");
    static_assert(Size >= kHeaderLen + MaxPayload, "Size must hold one record of MaxPayload octets");
    static_assert(Size <= 0x80000000u, "free-running 32-bit indices");
    static_assert(MaxPayload != 0, "len - 1 bound check");
    static_assert(Size / (kHeaderLen + 1) <= 0xFFFF, "backlog and peak fit 16 bits");

    /** @brief Sender as lwIP keeps it (address in network order) and a destination class chosen by the producer. */
    struct Meta
    {
        uint32_t srcAddr = 0;
        uint16_t srcPort = 0;
        uint8_t dst = 0;
    };

    /**
     * @brief Producer: stores one datagram of len octets; copy(dst, n, offset) writes payload [offset, offset + n).
     * @return false when it was dropped (empty, longer than MaxPayload, or ring full).
     */
    template <typename CopyFn>
    bool push(const Meta& meta, uint16_t len, CopyFn copy)
    {
        if ((uint16_t)(len - 1) >= MaxPayload) // 0 wraps to 0xFFFF: one compare for both bounds
        {
            if (len != 0)
                inc(_oversize); // an empty datagram carries nothing and is not a loss
            return false;
        }
        const uint32_t head = _head; // own index
        const uint32_t need = kHeaderLen + len;
        if (Size - (head - __atomic_load_n(&_tail, __ATOMIC_ACQUIRE)) < need)
        {
            inc(_dropped);
            return false;
        }

        put(head, (uint8_t)len);
        put(head + 1, (uint8_t)(len >> 8));
        put(head + 2, (uint8_t)meta.srcPort);
        put(head + 3, (uint8_t)(meta.srcPort >> 8));
        put(head + 4, (uint8_t)meta.srcAddr);
        put(head + 5, (uint8_t)(meta.srcAddr >> 8));
        put(head + 6, (uint8_t)(meta.srcAddr >> 16));
        put(head + 7, (uint8_t)(meta.srcAddr >> 24));
        put(head + 8, meta.dst);

        const uint32_t pos = (head + kHeaderLen) & kMask;
        const uint16_t first = (Size - pos < len) ? (uint16_t)(Size - pos) : len;
        copy(_buf + pos, first, 0);
        if (first < len) // only a record that wraps needs the second part
            copy(_buf, (uint16_t)(len - first), first);

        // _pushed before _head: a consumer that sees the record also sees it counted.
        const uint32_t pushed = _pushed + 1;
        __atomic_store_n(&_pushed, pushed, __ATOMIC_RELEASE);
        __atomic_store_n(&_head, head + need, __ATOMIC_RELEASE);
        const uint32_t backlog = pushed - __atomic_load_n(&_popped, __ATOMIC_ACQUIRE);
        if (backlog > _peak)
            __atomic_store_n(&_peak, backlog, __ATOMIC_RELAXED);
        return true;
    }

    /**
     * @brief Consumer: copies the oldest datagram straight into buffer.
     * @return its length; 0 when the ring is empty; -1 when it was longer than maxLen (consumed, not copied).
     */
    int pop(uint8_t* buffer, uint16_t maxLen, Meta& meta)
    {
        const uint32_t tail = _tail; // own index
        if (__atomic_load_n(&_head, __ATOMIC_ACQUIRE) == tail)
            return 0;
        const uint16_t len = (uint16_t)(get(tail) | (get(tail + 1) << 8));
        meta.srcPort = (uint16_t)(get(tail + 2) | (get(tail + 3) << 8));
        meta.srcAddr = (uint32_t)get(tail + 4) | ((uint32_t)get(tail + 5) << 8) | ((uint32_t)get(tail + 6) << 16) |
                       ((uint32_t)get(tail + 7) << 24);
        meta.dst = get(tail + 8);
        const bool fits = len <= maxLen; // the caller's buffer
        if (fits)
        {
            const uint32_t pos = (tail + kHeaderLen) & kMask;
            const uint32_t first = (Size - pos < len) ? Size - pos : len;
            memcpy(buffer, _buf + pos, first);
            memcpy(buffer + first, _buf, len - first);
        }
        // Copied before the release: once _tail moves, the producer may overwrite the record.
        __atomic_store_n(&_popped, _popped + 1, __ATOMIC_RELEASE);
        __atomic_store_n(&_tail, tail + kHeaderLen + len, __ATOMIC_RELEASE);
        return fits ? (int)len : -1;
    }

    /** @brief Consumer, producer stopped: empties the ring in O(1). Not counted as a loss. */
    void reset()
    {
        __atomic_store_n(&_popped, __atomic_load_n(&_pushed, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
        __atomic_store_n(&_tail, __atomic_load_n(&_head, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    }

    /** @brief Producer stopped (any task): the write index, for a later discardTo() by the consumer. */
    uint32_t mark() const { return __atomic_load_n(&_head, __ATOMIC_ACQUIRE); }

    /** @brief Consumer, producer may run: drops the records before a mark(), unless already read. Not a loss. */
    void discardTo(uint32_t mark)
    {
        uint32_t tail = _tail;
        uint32_t n = 0;
        while ((int32_t)(mark - tail) > 0)
        {
            tail += kHeaderLen + (uint32_t)(get(tail) | (get(tail + 1) << 8));
            n++;
        }
        __atomic_store_n(&_popped, _popped + n, __ATOMIC_RELEASE);
        __atomic_store_n(&_tail, tail, __ATOMIC_RELEASE);
    }

    /**
     * @brief Consumer, producer may run: drops every published datagram, walking the record lengths.
     * @return the number dropped; added to dropped().
     */
    uint32_t discard()
    {
        const uint32_t head = __atomic_load_n(&_head, __ATOMIC_ACQUIRE);
        uint32_t tail = _tail;
        uint32_t n = 0;
        while (tail != head)
        {
            tail += kHeaderLen + (uint32_t)(get(tail) | (get(tail + 1) << 8));
            n++;
        }
        __atomic_store_n(&_popped, _popped + n, __ATOMIC_RELEASE);
        __atomic_store_n(&_tail, head, __ATOMIC_RELEASE);
        const uint32_t d = _discarded;
        __atomic_store_n(&_discarded, (d > 0xFFFFFFFFu - n) ? 0xFFFFFFFFu : d + n, __ATOMIC_RELAXED);
        return n;
    }

    /** @brief Datagrams waiting. */
    uint16_t backlog() const
    {
        const uint32_t popped = __atomic_load_n(&_popped, __ATOMIC_ACQUIRE); // before _pushed: never ahead of it
        return (uint16_t)(__atomic_load_n(&_pushed, __ATOMIC_ACQUIRE) - popped);
    }
    /** @brief Octets in use, headers included. */
    uint32_t usedBytes() const
    {
        return __atomic_load_n(&_head, __ATOMIC_ACQUIRE) - __atomic_load_n(&_tail, __ATOMIC_ACQUIRE);
    }
    /** @brief Producer: records stored so far, the sequence number of the last push. */
    uint32_t pushed() const { return _pushed; }
    /** @brief Consumer: records taken so far, the sequence number of the last pop. */
    uint32_t popped() const { return _popped; }
    uint16_t peak() const { return (uint16_t)__atomic_load_n(&_peak, __ATOMIC_RELAXED); }
    /** @brief Lost: ring full, or dropped by discard(). Saturates. */
    uint32_t dropped() const
    {
        const uint32_t a = __atomic_load_n(&_dropped, __ATOMIC_RELAXED);
        const uint32_t b = __atomic_load_n(&_discarded, __ATOMIC_RELAXED);
        return (a > 0xFFFFFFFFu - b) ? 0xFFFFFFFFu : a + b;
    }
    uint32_t oversize() const { return __atomic_load_n(&_oversize, __ATOMIC_RELAXED); }
    static constexpr uint32_t capacity() { return Size; }

  private:
    static constexpr uint32_t kMask = Size - 1;

    void put(uint32_t index, uint8_t v) { _buf[index & kMask] = v; }
    uint8_t get(uint32_t index) const { return _buf[index & kMask]; }

    // Saturating; producer only, on the drop path.
    static void inc(uint32_t& v)
    {
        const uint32_t c = v;
        if (c != 0xFFFFFFFFu)
            __atomic_store_n(&v, c + 1, __ATOMIC_RELAXED);
    }

    uint8_t _buf[Size];
    uint32_t _head = 0;      // free-running write index, producer
    uint32_t _tail = 0;      // free-running read index, consumer
    uint32_t _pushed = 0;    // records stored, producer
    uint32_t _popped = 0;    // records taken or discarded, consumer
    uint32_t _peak = 0;      // highest backlog seen by the producer
    uint32_t _dropped = 0;   // ring full, producer
    uint32_t _oversize = 0;  // longer than MaxPayload, producer
    uint32_t _discarded = 0; // dropped by discard(), consumer
};

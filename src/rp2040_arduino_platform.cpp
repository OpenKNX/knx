/*-----------------------------------------------------

Plattform for Raspberry Pi Pico and other RP2040 boards
by SirSydom <com@sirsydom.de> 2021-2022

made to work with arduino-pico - "Raspberry Pi Pico Arduino core, for all RP2040 boards"
by Earl E. Philhower III https://github.com/earlephilhower/arduino-pico


RTTI must be set to enabled in the board options

Uses direct flash reading/writing.
Size ist defined by KNX_FLASH_SIZE (default 4k) - must be a multiple of 4096.
Offset in Flash is defined by KNX_FLASH_OFFSET (default 1,5MiB / 0x180000) - must be a multiple of 4096.

EEPROM Emulation from arduino-pico core (max 4k) can be use by defining USE_RP2040_EEPROM_EMULATION

A RAM-buffered Flash can be use by defining USE_RP2040_LARGE_EEPROM_EMULATION

For usage of KNX-IP you have to define either
- KNX_IP_LAN (use the arduino-pico core's w5500 lwip stack)
- KNX_IP_WIFI (use the arduino-pico core's PiPicoW lwip stack)

----------------------------------------------------*/

#include "rp2040_arduino_platform.h"

#ifdef ARDUINO_ARCH_RP2040
#include "knx/bits.h"


#include <Arduino.h>

// Pi Pico specific libs
#include <EEPROM.h>            // EEPROM emulation in flash, part of Earl E Philhowers Pi Pico Arduino support
#include <hardware/flash.h>    // from Pico SDK
#include <hardware/watchdog.h> // from Pico SDK
#include <pico/unique_id.h>    // from Pico SDK

#define FLASHPTR ((uint8_t*)XIP_BASE + KNX_FLASH_OFFSET)

#ifndef USE_RP2040_EEPROM_EMULATION
#if KNX_FLASH_SIZE % 4096
#error "KNX_FLASH_SIZE must be multiple of 4096"
#endif

#if KNX_FLASH_OFFSET % 4096
#error "KNX_FLASH_OFFSET must be multiple of 4096"
#endif
#endif

// KNX_RP2040_HAVE_IGMP (header) guards the lwIP includes and the routing socket code; the netif code is under KNX_IP_LAN.
#ifdef KNX_RP2040_HAVE_IGMP
#include <lwip/igmp.h>
#include <lwip/ip.h>
#include <lwip/pbuf.h>
#include <lwip/udp.h>
#include <lwip_wrap.h> // LWIPMutex: the async-context lock the core takes around every wrapped lwIP call
#include "knx/config.h"   // KNX_IP_RX_RING_SIZE
#include "knx/datagram_ring.h"
#include "knx/routing_busy_marks.h"
#ifdef KNX_IS_ROUTER
#include "knx/knx_ip_frame.h" // knxIpSetupGroupService()
#endif
#include <hardware/timer.h> // time_us_32()

// No readBytesMultiCast() for this long drops everything queued, fresh datagrams included (a failed OTA blocks loop() for minutes).
// time_us_32() is one timer register read; it wraps after 71 min, which only shortens a gap that long.
static constexpr uint32_t kRxStaleUs = 10000000;
typedef DatagramRing<KNX_IP_RX_RING_SIZE, KNXIP_RX_MAX_DATAGRAM> RxRing;
static RxRing s_rxRing;

// Destination class per datagram; the groups are written before their join and read by the callback.
// Without an extra group s_groupExtra equals s_groupRouting, so only a joined group is ever classed as a group.
enum : uint8_t { kDstOther = 0, kDstRouting = 1, kDstExtra = 2 };
static volatile uint32_t s_groupRouting = 0; // network order
static volatile uint32_t s_groupExtra = 0;   // network order
static RoutingBusyMarks s_busyMarks;           // ROUTING_BUSY from other hosts, recorded by the callback only

/**
 * @brief udp_recv callback of the routing socket: copies the datagram into s_rxRing and frees it.
 * Runs with the lwIP lock held: async-context IRQ, lock release of any lwIP call, or the main-loop pump. p is never null.
 */
static void rxCallback(void* arg, struct udp_pcb* pcb, struct pbuf* p, const ip_addr_t* addr, u16_t port)
{
    (void)arg;
    (void)pcb;
    RxRing::Meta meta;
    meta.srcAddr = ip4_addr_get_u32(ip_2_ip4(addr));
    meta.srcPort = port;
    bool busy = false;
#ifdef KNX_IS_ROUTER // only the router asks lastMulticastDestination()
    const uint32_t dst = ip4_addr_get_u32(ip4_current_dest_addr());
    if (dst == s_groupRouting)
    {
        meta.dst = kDstRouting;
        // ROUTING_BUSY stops sending at reception, not when loop() reads it (03_08_05 2.3.5 p.11).
        const uint8_t* b = (const uint8_t*)p->payload;
        busy = p->len >= 4 && b[2] == 0x05 && b[3] == 0x32 && meta.srcAddr != ip4_addr_get_u32(netif_ip4_addr(ip_current_input_netif()));
    }
    else if (dst == s_groupExtra) // the system setup group, joined apart from the routing group
    {
        // Service type at octets 2-3; pbuf_get_at() reads 0 past the end, so a short datagram is dropped too.
        if (!knxIpSetupGroupService((uint16_t)((pbuf_get_at(p, 2) << 8) | pbuf_get_at(p, 3))))
        {
            pbuf_free(p);
            return;
        }
        meta.dst = kDstExtra;
    } // else kDstOther, the Meta default
#endif
    // offset + n <= tot_len, so pbuf_copy_partial() copies all n octets.
    // Recorded only once stored, so a dropped ROUTING_BUSY never holds sending.
    if (s_rxRing.push(meta, p->tot_len, [p](uint8_t* to, uint16_t n, uint16_t offset) { pbuf_copy_partial(p, to, n, offset); }) && busy)
        s_busyMarks.record(s_rxRing.pushed(), millis());
    pbuf_free(p);
}

/**
 * @brief Sends one datagram from the routing socket (pcb not null), so the source port is the bound one (3671).
 * ttl is applied for a group destination.
 */
static err_t routingSendTo(struct udp_pcb* pcb, uint32_t netAddr, uint16_t port, const uint8_t* data, uint16_t len, bool group, uint8_t ttl)
{
    LWIPMutex lock; // the TTL is a plain pcb field; the wrapped calls below nest in this lock
    struct pbuf* p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
    if (p == nullptr)
        return ERR_MEM;
    memcpy(p->payload, data, len); // PBUF_RAM: one contiguous pbuf of len octets
    if (group)
        udp_set_multicast_ttl(pcb, ttl);
    ip_addr_t dst = IPADDR4_INIT(netAddr);
    const err_t err = udp_sendto(pcb, p, &dst, port);
    pbuf_free(p);
    return err;
}
#endif

#ifdef KNX_IP_LAN
#include <lwip/netif.h>
// The network module's lwIP interface, registered at its init: the stack reads it as plain lwIP, whatever driver
// class the module instantiates.
static struct netif* s_lanNetif = nullptr;
void RP2040ArduinoPlatform::setNetif(struct netif* netif)
{
    s_lanNetif = netif;
}
#elif defined(KNX_IP_WIFI)
#elif defined(KNX_IP_GENERIC)

#endif

RP2040ArduinoPlatform::RP2040ArduinoPlatform()
{
    // Platform::_memoryType defaults to Eeprom and the facade default-constructs the platform, so a
    // product that never calls registerFlashCallbacks() would run the RP2040 in Eeprom mode.
#ifndef USE_RP2040_EEPROM_EMULATION
    _memoryType = Flash;
#endif
}

RP2040ArduinoPlatform::RP2040ArduinoPlatform(TPUart::Interface::Abstract* interface) : ArduinoPlatform(interface)
{
#ifndef USE_RP2040_EEPROM_EMULATION
    _memoryType = Flash;
#endif
}

uint32_t RP2040ArduinoPlatform::uniqueSerialNumber()
{
    pico_unique_board_id_t id; // 64Bit unique serial number

    noInterrupts();
    rp2040.idleOtherCore();

    // Retrieve a stable unique device ID across platforms.
    // RP2040: reads JEDEC ID from external flash
    // RP2350: falls back to OTP-based chip ID (preferred, flash may not be unique)
    pico_get_unique_board_id(&id);

    rp2040.resumeOtherCore();
    interrupts();

    // use lower 4 byte and convert to unit32_t
    uint32_t uid = ((uint32_t)(id.id[4]) << 24) | ((uint32_t)(id.id[5]) << 16) | ((uint32_t)(id.id[6]) << 8) | (uint32_t)(id.id[7]);

    return uid;
}

void RP2040ArduinoPlatform::restart()
{
    println("restart");
    watchdog_reboot(0, 0, 0);
}

#ifdef USE_RP2040_EEPROM_EMULATION

#pragma warning "Using EEPROM Simulation"

#ifdef USE_RP2040_LARGE_EEPROM_EMULATION

uint8_t* RP2040ArduinoPlatform::getEepromBuffer(uint32_t size)
{
    if (size % 4096)
    {
        println("KNX_FLASH_SIZE must be a multiple of 4096");
        fatalError();
    }

    if (!_rambuff_initialized)
    {
        memcpy(_rambuff, FLASHPTR, KNX_FLASH_SIZE);
        _rambuff_initialized = true;
    }

    return _rambuff;
}

void RP2040ArduinoPlatform::commitToEeprom()
{
    noInterrupts();
    rp2040.idleOtherCore();

    // ToDo: write block-by-block to prevent writing of untouched blocks
    if (memcmp(_rambuff, FLASHPTR, KNX_FLASH_SIZE))
    {
        flash_range_erase(KNX_FLASH_OFFSET, KNX_FLASH_SIZE);
        flash_range_program(KNX_FLASH_OFFSET, _rambuff, KNX_FLASH_SIZE);
    }

    rp2040.resumeOtherCore();
    interrupts();
}

#else

uint8_t* RP2040ArduinoPlatform::getEepromBuffer(uint32_t size)
{
    if (size > 4096)
    {
        println("KNX_FLASH_SIZE to big for EEPROM emulation (max. 4kB)");
        fatalError();
    }

    uint8_t* eepromptr = EEPROM.getDataPtr();

    if (eepromptr == nullptr)
    {
        EEPROM.begin(4096);
        eepromptr = EEPROM.getDataPtr();
    }

    return eepromptr;
}

void RP2040ArduinoPlatform::commitToEeprom()
{
    EEPROM.commit();
}

#endif

#else

size_t RP2040ArduinoPlatform::flashEraseBlockSize()
{
    return 16; // 16 pages x 256byte/page = 4096byte
}

size_t RP2040ArduinoPlatform::flashPageSize()
{
    return 256;
}

uint8_t* RP2040ArduinoPlatform::userFlashStart()
{
    return (uint8_t*)XIP_BASE + KNX_FLASH_OFFSET;
}

size_t RP2040ArduinoPlatform::userFlashSizeEraseBlocks()
{
    if (KNX_FLASH_SIZE <= 0)
        return 0;
    else
        return ((KNX_FLASH_SIZE - 1) / (flashPageSize() * flashEraseBlockSize())) + 1;
}

void RP2040ArduinoPlatform::flashErase(uint16_t eraseBlockNum)
{
    noInterrupts();
    rp2040.idleOtherCore();

    flash_range_erase(KNX_FLASH_OFFSET + eraseBlockNum * flashPageSize() * flashEraseBlockSize(), flashPageSize() * flashEraseBlockSize());

    rp2040.resumeOtherCore();
    interrupts();
}

void RP2040ArduinoPlatform::flashWritePage(uint16_t pageNumber, uint8_t* data)
{
    noInterrupts();
    rp2040.idleOtherCore();

    flash_range_program(KNX_FLASH_OFFSET + pageNumber * flashPageSize(), data, flashPageSize());

    rp2040.resumeOtherCore();
    interrupts();
}

void RP2040ArduinoPlatform::writeBufferedEraseBlock()
{
    if (_bufferedEraseblockNumber > -1 && _bufferedEraseblockDirty)
    {
        noInterrupts();
        rp2040.idleOtherCore();

        flash_range_erase(KNX_FLASH_OFFSET + _bufferedEraseblockNumber * flashPageSize() * flashEraseBlockSize(), flashPageSize() * flashEraseBlockSize());
        flash_range_program(KNX_FLASH_OFFSET + _bufferedEraseblockNumber * flashPageSize() * flashEraseBlockSize(), _eraseblockBuffer, flashPageSize() * flashEraseBlockSize());

        rp2040.resumeOtherCore();
        interrupts();

        _bufferedEraseblockDirty = false;
    }
}
#endif

#if defined(KNX_IP_LAN)
// Network order; 0 until the network module has registered its interface.
uint32_t RP2040ArduinoPlatform::currentIpAddress()
{
    return s_lanNetif ? ip4_addr_get_u32(netif_ip4_addr(s_lanNetif)) : 0;
}
uint32_t RP2040ArduinoPlatform::currentSubnetMask()
{
    return s_lanNetif ? ip4_addr_get_u32(netif_ip4_netmask(s_lanNetif)) : 0;
}
uint32_t RP2040ArduinoPlatform::currentDefaultGateway()
{
    return s_lanNetif ? ip4_addr_get_u32(netif_ip4_gw(s_lanNetif)) : 0;
}
void RP2040ArduinoPlatform::macAddress(uint8_t* addr)
{
    if (s_lanNetif)
        memcpy(addr, s_lanNetif->hwaddr, 6);
    else
        memset(addr, 0, 6);
}
#elif defined(KNX_NETIF)
uint32_t RP2040ArduinoPlatform::currentIpAddress()
{
    return KNX_NETIF.localIP();
}
uint32_t RP2040ArduinoPlatform::currentSubnetMask()
{
    return KNX_NETIF.subnetMask();
}
uint32_t RP2040ArduinoPlatform::currentDefaultGateway()
{
    return KNX_NETIF.gatewayIP();
}
void RP2040ArduinoPlatform::macAddress(uint8_t* addr)
{
    KNX_NETIF.macAddress(addr);
}
#endif

#ifdef KNX_RP2040_HAVE_IGMP
// multicast
bool RP2040ArduinoPlatform::setupMultiCast(uint32_t addr, uint16_t port)
{
    mcastaddr = IPAddress(htonl(addr));
    println("Initializing KNX multicast.");
    print("  Bind ");
    print(mcastaddr.toString().c_str());
    print(":");
    println(port);

    _port = port;
    removeRoutingSocket(); // replaces a socket still open; its group stays joined
    s_groupRouting = (uint32_t)mcastaddr;
    s_groupExtra = s_groupRouting; // no extra group yet
    bool joined = false;
    if (igmp_joingroup(IP4_ADDR_ANY4, mcastaddr) == ERR_OK)
    {
        LWIPMutex lock;
        _pcb = udp_new();
        if (_pcb != nullptr)
        {
            udp_recv(_pcb, rxCallback, nullptr);
            if (udp_bind(_pcb, IP_ADDR_ANY, port) == ERR_OK)
                joined = true;
            else
            {
                udp_remove(_pcb);
                _pcb = nullptr;
            }
        }
    }
    _lastReadUs = time_us_32();
    if (!joined) println("  KNX multicast join FAILED");
    return joined;
}

/** @brief Removes the routing socket and empties its queue; after udp_remove() the callback cannot run again. */
void RP2040ArduinoPlatform::removeRoutingSocket()
{
    if (_pcb != nullptr)
    {
        udp_remove(_pcb);
        _pcb = nullptr;
    }
    s_rxRing.reset();
}

void RP2040ArduinoPlatform::closeMultiCast()
{
    // Removing the socket does not leave the group, and igmp_joingroup only reports for a NON_MEMBER group:
    // without this pairing the re-join is silent on the wire and group->use grows unbounded.
    if ((uint32_t)mcastaddr != 0)
        igmp_leavegroup(IP4_ADDR_ANY4, mcastaddr);
    if ((uint32_t)mcastaddrExtra != 0)
        igmp_leavegroup(IP4_ADDR_ANY4, mcastaddrExtra);
    mcastaddrExtra = IPAddress();
    removeRoutingSocket();
}

// The routing socket is bound to ANY, so a joined group reaches it; the destination class tells the groups apart.
bool RP2040ArduinoPlatform::joinMultiCastExtra(uint32_t addr)
{
    mcastaddrExtra = IPAddress(htonl(addr));
    s_groupExtra = (uint32_t)mcastaddrExtra; // before the join: no datagram of the group arrives unclassified
    if (igmp_joingroup(IP4_ADDR_ANY4, mcastaddrExtra) == ERR_OK)
        return true;
    mcastaddrExtra = IPAddress();
    s_groupExtra = s_groupRouting;
    return false;
}

// Never called here: multiCastSecondaryIsolated() returns false, and lwIP refuses a second ANY:3671 bind (SO_REUSE 0).
bool RP2040ArduinoPlatform::setupMultiCastSecondary(uint32_t addr, uint16_t port)
{
    mcastaddrSbc = IPAddress(htonl(addr));
    const bool joined = (_udpSbc.beginMulticast(mcastaddrSbc, port) != 0);

    if (!joined)
        println("  KNX system broadcast multicast join FAILED");

    return joined;
}

// No igmp_leavegroup here: the group may be the routing socket's as well. Never reached: no socket is opened.
void RP2040ArduinoPlatform::closeMultiCastSecondary()
{
    _udpSbc.stop();
}

int RP2040ArduinoPlatform::readBytesMultiCastSecondary(uint8_t* buffer, uint16_t maxLen)
{
    int len = _udpSbc.parsePacket();

    if (len == 0)
        return 0;

    if (len > maxLen)
    {
        println("Unexpected UDP data packet length - drop packet");
        for (size_t i = 0; i < len; i++)
            _udpSbc.read();
        return 0;
    }

    _udpSbc.read(buffer, len);
    return len;
}

bool RP2040ArduinoPlatform::sendBytesMultiCast(uint8_t* buffer, uint16_t len)
{
    // printHex("<- ",buffer, len);

    // _pcb is set: the caller sends only while enabled, which needs a successful setupMultiCast().
    return routingSendTo(_pcb, (uint32_t)mcastaddr, _port, buffer, len, true, _mcastTtl) == ERR_OK;
}

int RP2040ArduinoPlatform::readBytesMultiCast(uint8_t* buffer, uint16_t maxLen, uint32_t& src_addr, uint16_t& src_port)
{
    const uint32_t now = time_us_32();
    if ((uint32_t)(now - _lastReadUs) >= kRxStaleUs && s_rxRing.discard() != 0)
        println("KNX IP receive queue: stale datagrams dropped");
    _lastReadUs = now;

    RxRing::Meta meta;
    const int len = s_rxRing.pop(buffer, maxLen, meta);
    if (len <= 0)
        return 0; // empty, or longer than maxLen (consumed; not with the 512-octet caller, the ring drops longer ones)

    _remoteIP = IPAddress(meta.srcAddr);
    _remotePort = meta.srcPort;
    _lastDstClass = meta.dst;
    src_addr = htonl(_remoteIP);
    src_port = _remotePort;

    // printHex("-> ", buffer, len);

    return len;
}

// Mapped on request: only the router asks. A destination that is no joined group reads as 255.255.255.255.
uint32_t RP2040ArduinoPlatform::lastMulticastDestination()
{
    return _lastDstClass == kDstRouting ? htonl((uint32_t)mcastaddr)
         : _lastDstClass == kDstExtra   ? htonl((uint32_t)mcastaddrExtra)
                                        : 0xFFFFFFFFu;
}

uint16_t RP2040ArduinoPlatform::multicastRxBacklog()
{
    return s_rxRing.backlog();
}

static void (*s_receivePoll)() = nullptr;

void RP2040ArduinoPlatform::setReceivePoll(void (*poll)())
{
    s_receivePoll = poll;
}

void RP2040ArduinoPlatform::pollReceive()
{
    if (s_receivePoll != nullptr) // set only by a driver whose chip is pumped from the main loop
        s_receivePoll();
}

bool RP2040ArduinoPlatform::routingBusyPending()
{
    const uint32_t ahead = s_busyMarks.ahead(s_rxRing.popped()); // before the backlog, so a newer mark is counted in it
    return ahead < s_rxRing.backlog(); // never set outside router builds: only the router callback records
}

uint32_t RP2040ArduinoPlatform::routingBusyReceivedAt(uint32_t fallback)
{
    return s_busyMarks.arrivedAt(s_rxRing.popped(), fallback);
}

bool RP2040ArduinoPlatform::multicastRxStats(MulticastRxStats& stats)
{
    stats.backlog = s_rxRing.backlog();
    stats.peak = s_rxRing.peak();
    stats.capacity = RxRing::capacity();
    stats.dropped = s_rxRing.dropped();
    stats.oversize = s_rxRing.oversize();
    return true;
}

// unicast
bool RP2040ArduinoPlatform::sendBytesUniCast(uint32_t addr, uint16_t port, uint8_t* buffer, uint16_t len)
{
    IPAddress ucastaddr(htonl(addr));

    if (!addr)
        ucastaddr = _remoteIP;

    if (!port)
        port = _remotePort;
    // print("sendBytesUniCast to:");
    // println(ucastaddr.toString().c_str());

    if (_pcb == nullptr)
    {
        println("sendBytesUniCast: no socket");
        return false; // no routing socket: nothing is sent
    }
    // A multicast destination (IP system broadcast) gets PID 67 as well.
    const bool group = (addr & 0xF0000000) == 0xE0000000;
    if (routingSendTo(_pcb, (uint32_t)ucastaddr, port, buffer, len, group, _mcastTtl) != ERR_OK)
    {
        println("sendBytesUniCast fail");
        return false; // no pbuf, no route (link down) or the driver refused the frame
    }
    return true;
}
#endif

#endif

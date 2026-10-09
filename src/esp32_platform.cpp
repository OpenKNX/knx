#include "esp32_platform.h"

#ifdef ARDUINO_ARCH_ESP32
#include <Arduino.h>
#include <EEPROM.h>

#include "knx/bits.h"

// #ifndef KNX_SERIAL
//     #define KNX_SERIAL Serial1
//     #pragma warn "KNX_SERIAL not defined, using Serial1"
// #endif
 
#ifdef KNX_IP_LAN
    #include "ETH.h"
    #define KNX_NETIF ETH
#else // KNX_IP_WIFI
    #include <WiFi.h>
    #define KNX_NETIF WiFi
#endif

Esp32Platform::Esp32Platform()
{
}

Esp32Platform::Esp32Platform(TPUart::Interface::Abstract* interface) : ArduinoPlatform(interface)
{
}

uint32_t Esp32Platform::currentIpAddress()
{
    return KNX_NETIF.localIP();
}

uint32_t Esp32Platform::currentSubnetMask()
{
    return KNX_NETIF.subnetMask();
}

uint32_t Esp32Platform::currentDefaultGateway()
{
    return KNX_NETIF.gatewayIP();
}

void Esp32Platform::macAddress(uint8_t * addr)
{
    KNX_NETIF.macAddress(addr);
}

uint32_t Esp32Platform::uniqueSerialNumber()
{
    uint64_t chipid = ESP.getEfuseMac();
    uint32_t upperId = (chipid >> 32) & 0xFFFFFFFF;
    uint32_t lowerId = (chipid & 0xFFFFFFFF);
    return (upperId ^ lowerId);
}

void Esp32Platform::restart()
{
    println("restart");
    ESP.restart();
}

#ifdef USE_IP // KNX_IP_LAN: Ethernet, else WiFi
#include <lwip/igmp.h>
#include <lwip/ip.h>
#include <lwip/pbuf.h>
#include <lwip/tcpip.h>
#include <lwip/udp.h>
#include "knx/config.h" // KNX_IP_RX_RING_SIZE
#include "knx/datagram_ring.h"
#include "knx/routing_busy_marks.h"
#ifdef KNX_IS_ROUTER
#include "knx/knx_ip_frame.h" // knxIpSetupGroupService()
#endif

#if !LWIP_TCPIP_CORE_LOCKING
#error "the raw udp_pcb needs CONFIG_LWIP_TCPIP_CORE_LOCKING (set in the prebuilt Arduino-ESP32 libraries)"
#endif

// No readBytesMultiCast() for this long drops everything queued, fresh datagrams included (a failed OTA blocks loop() for minutes).
static const TickType_t kRxStaleTicks = pdMS_TO_TICKS(10000);
typedef DatagramRing<KNX_IP_RX_RING_SIZE, KNXIP_RX_MAX_DATAGRAM> RxRing;
static RxRing s_rxRing;

// Destination class per datagram; the groups are written under the core lock before their join, read by the callback.
// Without an extra group s_groupExtra equals s_groupRouting, so only a joined group is ever classed as a group.
enum : uint8_t { kDstOther = 0, kDstRouting = 1, kDstExtra = 2 };
static volatile uint32_t s_groupRouting = 0; // network order
static volatile uint32_t s_groupExtra = 0;   // network order
static RoutingBusyMarks s_busyMarks;           // ROUTING_BUSY from other hosts, recorded by the callback only

/**
 * @brief udp_recv callback of the routing socket: copies the datagram into s_rxRing and frees it.
 * Runs in the tcpip task with the core lock held; loop() reads the ring unlocked on the other core. p is never null.
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
 * @brief Sends one datagram from the routing socket, so the source port is the bound one (3671).
 * false when it did not leave: no socket (endpoint closed), no pbuf, or lwIP refused it. ttl for a group.
 */
bool Esp32Platform::routingSend(uint32_t netAddr, uint16_t port, const uint8_t* data, uint16_t len, bool group)
{
    LOCK_TCPIP_CORE();
    err_t err = ERR_CONN;
    if (_pcb != nullptr) // no socket while the endpoint is closed
    {
        struct pbuf* p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
        err = ERR_MEM;
        if (p != nullptr)
        {
            memcpy(p->payload, data, len); // PBUF_RAM: one contiguous pbuf of len octets
            if (group)
                udp_set_multicast_ttl(_pcb, _mcastTtl);
            ip_addr_t dst = IPADDR4_INIT(netAddr);
            err = udp_sendto(_pcb, p, &dst, port);
            pbuf_free(p);
        }
    }
    UNLOCK_TCPIP_CORE();
    return err == ERR_OK;
}

/**
 * @brief Removes the routing socket, leaves its groups and marks its queued datagrams for loop() to drop.
 * Caller holds the core lock; after udp_remove() the callback cannot run for it again, so the ring head is final.
 */
void Esp32Platform::removeRoutingSocket()
{
    if (_pcb == nullptr)
        return;
    ip4_addr_t group;
    ip4_addr_set_u32(&group, s_groupRouting);
    igmp_leavegroup(IP4_ADDR_ANY4, &group);
    if (s_groupExtra != s_groupRouting)
    {
        ip4_addr_set_u32(&group, s_groupExtra);
        igmp_leavegroup(IP4_ADDR_ANY4, &group);
    }
    s_groupExtra = s_groupRouting;
    udp_remove(_pcb);
    _pcb = nullptr;
    // loop() owns the read index: it drops up to _rxCut itself once it sees the new epoch.
    __atomic_store_n(&_rxCut, s_rxRing.mark(), __ATOMIC_RELAXED);
    __atomic_store_n(&_rxEpoch, _rxEpoch + 1, __ATOMIC_RELEASE);
}

bool Esp32Platform::setupMultiCast(uint32_t addr, uint16_t port)
{
#ifdef KNX_IP_LAN
    esp_netif_t* check = esp_netif_get_handle_from_ifkey("ETH_DEF");
#else
    esp_netif_t* check = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
#endif
    if (check == nullptr)
    {
        println("No network interface initialized"); // recoverable: the caller retries on the next edge
        return false;
    }
    IPAddress mcastaddr(htonl(addr));

    println("Initializing KNX multicast.");
    print("  Bind ");
    print(mcastaddr.toString().c_str());
    print(":");
    println(port);

    LOCK_TCPIP_CORE();
    removeRoutingSocket(); // replaces a socket still open and leaves its groups
    _port = port;
    _lastReadTick = xTaskGetTickCount();
    bool joined = false;
    struct udp_pcb* pcb = udp_new();
    if (pcb != nullptr)
    {
        ip_set_option(pcb, SOF_REUSEADDR); // SO_REUSEADDR on the routing socket
        udp_recv(pcb, rxCallback, nullptr);
        s_groupRouting = (uint32_t)mcastaddr;
        s_groupExtra = s_groupRouting; // no extra group yet
        ip4_addr_t group;
        ip4_addr_set_u32(&group, s_groupRouting);
        if (udp_bind(pcb, IP4_ADDR_ANY, port) == ERR_OK && igmp_joingroup(IP4_ADDR_ANY4, &group) == ERR_OK)
        {
            _pcb = pcb;
            joined = true;
        }
        else
            udp_remove(pcb); // nothing was received on it: the ring holds no record of it
    }
    UNLOCK_TCPIP_CORE();

    if (!joined)
        println("  KNX multicast join FAILED");
    return joined;
}

void Esp32Platform::closeMultiCast()
{
    LOCK_TCPIP_CORE();
    removeRoutingSocket();
    UNLOCK_TCPIP_CORE();
}

// The routing socket is bound to ANY, so a joined group reaches it; the destination class tells the groups apart.
bool Esp32Platform::joinMultiCastExtra(uint32_t addr)
{
    LOCK_TCPIP_CORE();
    ip4_addr_t group;
    ip4_addr_set_u32(&group, htonl(addr));
    s_groupExtra = ip4_addr_get_u32(&group); // before the join: no datagram of the group arrives unclassified
    // _pcb: without the routing socket a join would never be left.
    const bool joined = _pcb != nullptr && igmp_joingroup(IP4_ADDR_ANY4, &group) == ERR_OK;
    if (!joined)
        s_groupExtra = s_groupRouting;
    UNLOCK_TCPIP_CORE();
    return joined;
}

// Mapped on request: only the router asks. A destination that is no joined group reads as 255.255.255.255.
uint32_t Esp32Platform::lastMulticastDestination()
{
    return _lastDstClass == kDstRouting ? htonl(s_groupRouting)
         : _lastDstClass == kDstExtra   ? htonl(s_groupExtra)
                                        : 0xFFFFFFFFu;
}

// Never called here: multiCastSecondaryIsolated() returns false, since SO_REUSE_RXTOALL also hands the group to the routing socket.
bool Esp32Platform::setupMultiCastSecondary(uint32_t addr, uint16_t port)
{
#ifdef KNX_IP_LAN
    esp_netif_t* check = esp_netif_get_handle_from_ifkey("ETH_DEF");
#else
    esp_netif_t* check = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
#endif

    if (check == nullptr)
        return false; // no interface yet; the caller retries when the mode is set again

    IPAddress mcastaddr(htonl(addr));
    const bool joined = (_udpSbc.beginMulticast(mcastaddr, port) != 0);

    if (!joined)
        println("  KNX system broadcast multicast join FAILED");

    return joined;
}

void Esp32Platform::closeMultiCastSecondary()
{
    _udpSbc.stop();
}

int Esp32Platform::readBytesMultiCastSecondary(uint8_t* buffer, uint16_t maxLen)
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

bool Esp32Platform::sendBytesMultiCast(uint8_t * buffer, uint16_t len)
{
    //printHex("<- ",buffer, len);
    return routingSend(s_groupRouting, _port, buffer, len, true);
}

int Esp32Platform::readBytesMultiCast(uint8_t * buffer, uint16_t maxLen, uint32_t& src_addr, uint16_t& src_port)
{
    // A removed socket's datagrams are dropped here, by the only task that moves the read index.
    const uint32_t epoch = __atomic_load_n(&_rxEpoch, __ATOMIC_ACQUIRE);
    if (epoch != _rxEpochSeen)
    {
        _rxEpochSeen = epoch;
        s_rxRing.discardTo(__atomic_load_n(&_rxCut, __ATOMIC_RELAXED));
    }
    const TickType_t now = xTaskGetTickCount();
    // Signed as a guard only: both writers of _lastReadTick run on this task, so it is never ahead of now.
    if ((int32_t)(now - _lastReadTick) >= (int32_t)kRxStaleTicks && s_rxRing.discard() != 0)
        println("KNX IP receive queue: stale datagrams dropped");
    _lastReadTick = now;

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

uint16_t Esp32Platform::multicastRxBacklog()
{
    return s_rxRing.backlog();
}

bool Esp32Platform::routingBusyPending()
{
    const uint32_t ahead = s_busyMarks.ahead(s_rxRing.popped()); // before the backlog, so a newer mark is counted in it
    return ahead < s_rxRing.backlog(); // never set outside router builds: only the router callback records
}

uint32_t Esp32Platform::routingBusyReceivedAt(uint32_t fallback)
{
    return s_busyMarks.arrivedAt(s_rxRing.popped(), fallback);
}

bool Esp32Platform::multicastRxStats(MulticastRxStats& stats)
{
    stats.backlog = s_rxRing.backlog();
    stats.peak = s_rxRing.peak();
    stats.capacity = RxRing::capacity();
    stats.dropped = s_rxRing.dropped();
    stats.oversize = s_rxRing.oversize();
    return true;
}

bool Esp32Platform::sendBytesUniCast(uint32_t addr, uint16_t port, uint8_t* buffer, uint16_t len)
{
    IPAddress ucastaddr(htonl(addr));

    if(!addr)
        ucastaddr = _remoteIP;
    
    if(!port)
        port = _remotePort;

    // A multicast destination (IP system broadcast) gets PID 67 as well.
    if (!routingSend((uint32_t)ucastaddr, port, buffer, len, (addr & 0xF0000000) == 0xE0000000))
    {
        println("sendBytesUniCast fail");
        return false;
    }
    return true;
}
#endif

uint8_t * Esp32Platform::getEepromBuffer(uint32_t size)
{
    // An earlier begin() may describe a SMALLER buffer than this caller needs, and its result was
    // discarded - both ended past the allocation. length() reports 16 bits while begin() takes a size_t,
    // so 64 KiB or more could never be reported back and every call would reallocate. Refuse it.
    if (size > 0xFFFF)
    {
        println("EEPROM size of 64 KiB or more cannot be served on this platform");
        return nullptr;
    }

    if (EEPROM.length() < size)
    {
        if (!EEPROM.begin(size))
        {
            println("EEPROM.begin failed -- no non-volatile buffer");
            return nullptr;
        }
    }

    uint8_t* eepromptr = EEPROM.getDataPtr();
    if (eepromptr == nullptr)
        println("EEPROM data pointer is null after begin");

    return eepromptr;
}

void Esp32Platform::commitToEeprom()
{
    EEPROM.getDataPtr(); // trigger dirty flag in EEPROM lib to make sure data will be written to flash
    EEPROM.commit();
}

#endif

#include "esp32_platform.h"

#ifdef ARDUINO_ARCH_ESP32
#include <Arduino.h>
#include <EEPROM.h>

#include "knx/bits.h"



#include "lwip/udp.h"
#include "lwip/igmp.h"
#include "lwip/ip4_addr.h"
#include "lwip/pbuf.h"


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

static void udpReceiveCallback(void* arg,
                               struct udp_pcb* pcb,
                               struct pbuf* p,
                               const ip_addr_t* addr,
                               u16_t port)
{
    Esp32Platform* self = (Esp32Platform*)arg;

    if (!p) return;

    if (p->tot_len > sizeof(self->_rxBuffer))
    {
        pbuf_free(p);
        return;
    }

    pbuf_copy_partial(p, self->_rxBuffer, p->tot_len, 0);

    self->_rxLen = p->tot_len;

    self->_srcIP   = ntohl(ip4_addr_get_u32(ip_2_ip4(addr)));
    self->_srcPort = port;

    self->_dstIP   = ntohl(ip4_addr_get_u32(ip_2_ip4(&pcb->local_ip)));

    pbuf_free(p);
}

void Esp32Platform::setupMultiCast(uint32_t addr, uint16_t port)
{
#ifdef KNX_IP_LAN
    esp_netif_t* check = esp_netif_get_handle_from_ifkey("ETH_DEF");
#else
    esp_netif_t* check = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
#endif

    if (!check)
    {
        println("No network interface initialized");
        fatalError();
    }

    _udpPcb = udp_new();

    if (!_udpPcb)
    {
        println("Failed to create UDP PCB");
        return;
    }

    ip_addr_t any;
    ip4_addr_set_any(ip_2_ip4(&any));

    if (udp_bind(_udpPcb, &any, port) != ERR_OK)
    {
        println("UDP bind failed");
        udp_remove(_udpPcb);
        _udpPcb = nullptr;
        return;
    }

    udp_recv(_udpPcb, udpReceiveCallback, this);

    // Join multicast
    joinMultiCast(addr);

    println("Initializing KNX multicast.");
}

void Esp32Platform::joinMultiCast(uint32_t addr)
{
    ip4_addr_t maddr;
    ip4_addr_set_u32(&maddr, htonl(addr));

    igmp_joingroup(IP4_ADDR_ANY4, &maddr);
}

int Esp32Platform::readBytesMultiCast(uint8_t *buffer,
                                      uint16_t maxLen,
                                      uint32_t& src_addr,
                                      uint16_t& src_port)
{
    if (_rxLen == 0)
        return 0;

    if (_rxLen > maxLen)
        return 0;

    memcpy(buffer, _rxBuffer, _rxLen);

    src_addr = _srcIP;
    src_port = _srcPort;

    print("Receive UDP ");
    print(IPAddress(_srcIP).toString().c_str());
    print(":");
    print(_srcPort);
    print(" -> ");
    println(IPAddress(_dstIP).toString().c_str());

    int len = _rxLen;
    _rxLen = 0;

    return len;
}

void Esp32Platform::closeMultiCast()
{
    if (_udpPcb)
    {
        udp_remove(_udpPcb);
        _udpPcb = nullptr;
    }
}


bool Esp32Platform::sendBytesMultiCast(uint8_t* buffer, uint16_t len)
{
    if (!_udpPcb) return false;

    struct pbuf* p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
    if (!p) return false;

    memcpy(p->payload, buffer, len);

    ip_addr_t dest;
    ip4_addr_set_u32(ip_2_ip4(&dest), htonl(_udpSockMulticastAddr.sin_addr.s_addr));

    err_t err = udp_sendto(_udpPcb, p, &dest,
                           ntohs(_udpSockMulticastAddr.sin_port));

    pbuf_free(p);

    return err == ERR_OK;
}


bool Esp32Platform::sendBytesUniCast(uint32_t addr, uint16_t port, uint8_t* buffer, uint16_t len)
{
    if (!_udpPcb) return false;

    struct pbuf* p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
    if (!p) return false;

    memcpy(p->payload, buffer, len);

    ip_addr_t dest;
    ip4_addr_set_u32(ip_2_ip4(&dest), htonl(addr));

    err_t err = udp_sendto(_udpPcb, p, &dest, port);

    pbuf_free(p);

    return err == ERR_OK;
}

uint8_t * Esp32Platform::getEepromBuffer(uint32_t size)
{
    uint8_t * eepromptr = EEPROM.getDataPtr();
    if(eepromptr == nullptr) {
        EEPROM.begin(size);
        eepromptr = EEPROM.getDataPtr();
    }
    return eepromptr;
}

void Esp32Platform::commitToEeprom()
{
    EEPROM.getDataPtr(); // trigger dirty flag in EEPROM lib to make sure data will be written to flash
    EEPROM.commit();
}

#endif

#include "esp32_platform.h"

#ifdef ARDUINO_ARCH_ESP32
#include <Arduino.h>
#include <EEPROM.h>

#include "knx/bits.h"


#include <string.h>

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

void Esp32Platform::setupMultiCast(uint32_t addr, uint16_t port)
{
#ifdef KNX_IP_LAN
    esp_netif_t* check = esp_netif_get_handle_from_ifkey("ETH_DEF");
#else
    esp_netif_t* check = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
#endif
    if (check == nullptr)
    {
        println("No network interface initialized");
        fatalError();
    }
    
    _udpSockMulticastAddr.sin_family = AF_INET;
    _udpSockMulticastAddr.sin_addr.s_addr = htonl(addr);
    _udpSockMulticastAddr.sin_port = htons(port);

    // Create multicast socket
    int _udpSockMulticast = socket(AF_INET, SOCK_DGRAM, 0);
    if (_udpSockMulticast < 0)
    {
        println("Failed to create multicast socket");
    }

    // Set socket options for multicast
    int opt = 1;
    setsockopt(_udpSockMulticast, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind the socket to the multicast address
    if (bind(_udpSockMulticast, (struct sockaddr*)&_udpSockMulticastAddr, sizeof(_udpSockMulticastAddr)) < 0)
    {
        println("Failed to bind multicast socket");
        close(_udpSockMulticast);
    }

    // Join multicast group
    ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = htonl(addr);
    mreq.imr_interface.s_addr = INADDR_ANY;
    if (setsockopt(_udpSockMulticast, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
    {
        println("Failed to join multicast group");
        close(_udpSockMulticast);
    }

    // Create unicast socket
    int _udpSockUnicast = socket(AF_INET, SOCK_DGRAM, 0);
    if (_udpSockUnicast < 0)
    {
        println("Failed to create unicast socket");
    }

    // Bind the unicast socket to the same port
    struct sockaddr_in unicast_addr;
    unicast_addr.sin_family = AF_INET;
    unicast_addr.sin_addr.s_addr = INADDR_ANY; // or specific unicast address
    unicast_addr.sin_port = htons(port); // use the same port as multicast
    if (bind(_udpSockUnicast, (struct sockaddr*)&unicast_addr, sizeof(unicast_addr)) < 0)
    {
        println("Failed to bind unicast socket");
        close(_udpSockUnicast);
    }

    println("Initializing KNX multicast.");
    print("  Bind ");
    print(IPAddress(htonl(addr)).toString().c_str());
    print(":");
    println(port);
}

void Esp32Platform::closeMultiCast()
{
    if (_udpSockMulticast >= 0)
    {
        close(_udpSockMulticast);
        _udpSockMulticast = -1; // Mark as closed
        println("Multicast socket closed");
    }

    if (_udpSockUnicast >= 0)
    {
        close(_udpSockUnicast);
        _udpSockUnicast = -1; // Mark as closed
        println("Unicast socket closed");
    }
}

bool Esp32Platform::sendBytesMultiCast(uint8_t * buffer, uint16_t len)
{
    if (_udpSockMulticast < 0)
    {
        println("Multicast socket not initialized");
        return false;
    }

    ssize_t sentBytes = sendto(_udpSockMulticast, buffer, len, 0, (struct sockaddr*)&_udpSockMulticastAddr, sizeof(_udpSockMulticastAddr));
    if (sentBytes < 0)
    {
        println("Failed to send multicast data");
        return false;
    }

    return true;
}

int Esp32Platform::readBytesMultiCast(uint8_t * buffer, uint16_t maxLen, uint32_t& src_addr, uint16_t& src_port)
{
    struct sockaddr_in src_addr_struct;
    socklen_t addrlen = sizeof(src_addr_struct);
    ssize_t len = recvfrom(_udpSockMulticast, buffer, maxLen, 0, (struct sockaddr*)&src_addr_struct, &addrlen);
    
    if (len < 0)
    {
        return 0;
    }

    if (len > maxLen)
    {
        println("Unexpected UDP data packet length - drop packet");
        return 0;
    }

    src_addr = ntohl(src_addr_struct.sin_addr.s_addr);
    src_port = ntohs(src_addr_struct.sin_port);

    _remoteIP = src_addr;
    _remotePort = src_port;

    print("Receive Multicast UDP ");
    print(IPAddress(src_addr).toString().c_str());
    print(":");
    println(src_port);
    printHex("-> ", buffer, len);

    return len;
}

int Esp32Platform::readBytesUniCast(uint8_t * buffer, uint16_t maxLen, uint32_t& src_addr, uint16_t& src_port)
{
    struct sockaddr_in src_addr_struct;
    socklen_t addrlen = sizeof(src_addr_struct);
    ssize_t len = recvfrom(_udpSockUnicast, buffer, maxLen, 0, (struct sockaddr*)&src_addr_struct, &addrlen);
    
    if (len < 0)
    {
        return 0;
    }

    if (len > maxLen)
    {
        println("Unexpected UDP data packet length - drop packet");
        return 0;
    }

    src_addr = ntohl(src_addr_struct.sin_addr.s_addr);
    src_port = ntohs(src_addr_struct.sin_port);

    _remoteIP = src_addr;
    _remotePort = src_port;

    print("Receive Unicast UDP ");
    print(IPAddress(src_addr).toString().c_str());
    print(":");
    println(src_port);
    printHex("-> ", buffer, len);

    return len;
}

bool Esp32Platform::sendBytesUniCast(uint32_t addr, uint16_t port, uint8_t* buffer, uint16_t len)
{
    struct sockaddr_in dest_addr;
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_addr.s_addr = htonl(addr);
    
    if (!addr)
        dest_addr.sin_addr.s_addr = htonl(_remoteIP);
    
    if (!port)
        port = _remotePort;

    dest_addr.sin_port = htons(port);

    ssize_t sentBytes = sendto(_udpSockUnicast, buffer, len, 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
    if (sentBytes < 0)
    {
        println("Failed to send unicast data");
        return false;
    }

    return true;
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

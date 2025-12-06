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
    IPAddress mcastaddr(htonl(addr));

    println("Initializing KNX multicast.");
    print("  Bind ");
    print(mcastaddr.toString().c_str());
    print(":");
    println(port);
    uint8_t result = _udp.beginMulticast(mcastaddr, port);
    (void)result; // Suppress unused variable warning
    // KNX_DEBUG_SERIAL.printf("result %d\n", result);
}

void Esp32Platform::closeMultiCast()
{
    _udp.stop();
}

bool Esp32Platform::sendBytesMultiCast(uint8_t * buffer, uint16_t len)
{
    //printHex("<- ",buffer, len);
    _udp.beginMulticastPacket();
    _udp.write(buffer, len);
    _udp.endPacket();
    return true;
}

int Esp32Platform::readBytesMultiCast(uint8_t * buffer, uint16_t maxLen, uint32_t& src_addr, uint16_t& src_port)
{
    int len = _udp.parsePacket();
    if (len == 0)
        return 0;

    if (len > maxLen)
    {
        println("Unexpected UDP data packet length - drop packet");
        for (size_t i = 0; i < len; i++)
            _udp.read();
        return 0;
    }

    _udp.read(buffer, len);
    _remoteIP = _udp.remoteIP();
    _remotePort = _udp.remotePort();
    src_addr = htonl(_remoteIP);
    src_port = _remotePort;

    // print("Remote IP: ");
    // print(_udp.remoteIP().toString().c_str());
    // printHex("-> ", buffer, len);

    return len;
}

bool Esp32Platform::sendBytesUniCast(uint32_t addr, uint16_t port, uint8_t* buffer, uint16_t len)
{
    IPAddress ucastaddr(htonl(addr));

    if(!addr)
        ucastaddr = _remoteIP;
    
    if(!port)
        port = _remotePort;

    if(_udp.beginPacket(ucastaddr, port) == 1)
    {
        _udp.write(buffer, len);
        if(_udp.endPacket() == 0) println("sendBytesUniCast endPacket fail");
    }
    else
        println("sendBytesUniCast beginPacket fail");
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

#ifdef ARDUINO_ARCH_ESP32
#include "arduino_platform.h"
#include "TPUart/Interface/Abstract.h"


#include <WiFiUdp.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

class Esp32Platform : public ArduinoPlatform
{
public:
    Esp32Platform();
    Esp32Platform(TPUart::Interface::Abstract* interface);

    // ip stuff
    uint32_t currentIpAddress() override;
    uint32_t currentSubnetMask() override;
    uint32_t currentDefaultGateway() override;
    void macAddress(uint8_t* addr) override;

    // unique serial number
    uint32_t uniqueSerialNumber() override;

    // basic stuff
    void restart();

    //multicast
    void setupMultiCast(uint32_t addr, uint16_t port) override;
    void closeMultiCast() override;
    bool sendBytesMultiCast(uint8_t* buffer, uint16_t len) override;
    int readBytesMultiCast(uint8_t* buffer, uint16_t maxLen, uint32_t& src_addr, uint16_t& src_port) override;
    
    //unicast 
    bool sendBytesUniCast(uint32_t addr, uint16_t port, uint8_t* buffer, uint16_t len) override;

    //memory
    uint8_t* getEepromBuffer(uint32_t size);
    void commitToEeprom();

    protected: uint32_t _remoteIP;
    protected: uint16_t _remotePort;

private:
    //WiFiUDP _udp;
    //WiFiUDP _udp2;

    //int _udpSockUnicast = -1;

    int _udpSock = -1;
    struct sockaddr_in _udpSockMulticastAddr;


    //int _udpSockMulticast2 = -1;
    //uint32_t _multicastAddr2 = 0;
    // int8_t _rxPin = -1; 
    // int8_t _txPin = -1;
};

#endif

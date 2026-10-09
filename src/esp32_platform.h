#ifdef ARDUINO_ARCH_ESP32
#include "arduino_platform.h"
#include "TPUart/Interface/Abstract.h"
#include "knx/config.h" // USE_IP


#include <WiFiUdp.h>

struct udp_pcb;

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

#ifdef USE_IP // KNX_IP_LAN: Ethernet, else WiFi
    //multicast
    bool setupMultiCast(uint32_t addr, uint16_t port) override;
    void closeMultiCast() override;
    bool sendBytesMultiCast(uint8_t* buffer, uint16_t len) override;
    int readBytesMultiCast(uint8_t* buffer, uint16_t maxLen, uint32_t& src_addr, uint16_t& src_port) override;
    void multicastTtl(uint8_t ttl) override { _mcastTtl = ttl; }
    bool multicastTtlAtJoinOnly() override { return false; } // applied per send
    bool joinMultiCastExtra(uint32_t addr) override;
    uint32_t lastMulticastDestination() override;

    //second multicast socket: system setup group, for IP system broadcast
    bool setupMultiCastSecondary(uint32_t addr, uint16_t port) override;
    void closeMultiCastSecondary() override;
    int readBytesMultiCastSecondary(uint8_t* buffer, uint16_t maxLen) override;

    //unicast
    bool sendBytesUniCast(uint32_t addr, uint16_t port, uint8_t* buffer, uint16_t len) override;

    //receive queue of the routing socket
    uint16_t multicastRxBacklog() override;
    bool routingBusyPending() override;
    uint32_t routingBusyReceivedAt(uint32_t fallback) override;
    bool multicastRxStats(MulticastRxStats& stats) override;
#endif

    //memory
    uint8_t* getEepromBuffer(uint32_t size);
    void commitToEeprom();

    protected: IPAddress _remoteIP;
    protected: uint16_t _remotePort;

private:
#ifdef USE_IP
    void removeRoutingSocket(); // caller holds the lwIP core lock
    bool routingSend(uint32_t netAddr, uint16_t port, const uint8_t* data, uint16_t len, bool group);
    // _pcb, _port, _rxCut, _rxEpoch: written under the lwIP core lock by setup/close, both on the loop task.
    struct udp_pcb* _pcb = nullptr; // routing socket, ANY:port; also the unicast source
    uint16_t _port = 0;
    uint32_t _rxCut = 0;            // ring index up to which the records belong to a removed socket
    uint32_t _rxEpoch = 0;          // raised after _rxCut at every socket removal
    uint32_t _rxEpochSeen = 0;      // loop() only
    uint32_t _lastReadTick = 0;     // xTaskGetTickCount() of the last readBytesMultiCast() or setupMultiCast()
    uint8_t _lastDstClass = 0;      // destination class of the datagram last read
    WiFiUDP _udpSbc;
#endif
    uint8_t _mcastTtl = 16; // PID 67 default; applied per send
    // int8_t _rxPin = -1; 
    // int8_t _txPin = -1;
};

#endif

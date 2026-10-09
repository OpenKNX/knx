#pragma once

#include <stdint.h>
#include <stddef.h>
#include "save_restore.h"

#include "TPUart/Interface/Abstract.h"

#ifndef KNX_FLASH_CALLBACK
#ifndef KNX_FLASH_SIZE
#define KNX_FLASH_SIZE 1024
#pragma warning "KNX_FLASH_SIZE not defined, using 1024"
#endif
#endif

#ifdef KNX_FLASH_CALLBACK
#ifndef KNX_FLASH_SIZE
#define KNX_FLASH_SIZE 0
#endif
typedef uint32_t (*FlashCallbackSize)();
typedef uint8_t* (*FlashCallbackRead)();
typedef uint32_t (*FlashCallbackWrite)(uint32_t relativeAddress, uint8_t* buffer, size_t len);
typedef void (*FlashCallbackCommit)();
#endif

// Octets the KNXnet/IP layer reads a datagram into; a platform queue drops longer ones.
#define KNXIP_RX_MAX_DATAGRAM 512

/** @brief Receive queue of the routing socket, where the platform keeps one of its own. Its drops are not in PID 72/73. */
struct MulticastRxStats
{
    uint16_t backlog = 0;  // datagrams waiting
    uint16_t peak = 0;     // highest backlog since boot
    uint32_t capacity = 0; // queue size in octets
    uint32_t dropped = 0;  // lost: the queue was full, or its content went stale unread
    uint32_t oversize = 0; // longer than KNXIP_RX_MAX_DATAGRAM, discarded
};

enum NvMemoryType
{
    Eeprom,
    Flash,
    Callback
};

class Platform
{
  public:
    virtual ~Platform() {}

    TPUart::Interface::Abstract* interface();
    void interface(TPUart::Interface::Abstract* interface);

    // ip config
    virtual uint32_t currentIpAddress();
    virtual uint32_t currentSubnetMask();
    virtual uint32_t currentDefaultGateway();
    virtual void macAddress(uint8_t* data);

    // unique serial number
    virtual uint32_t uniqueSerialNumber();

    // basic stuff
    virtual void restart() = 0;
    virtual void fatalError() = 0;

    //multicast socket
    virtual bool setupMultiCast(uint32_t addr, uint16_t port); // false: the group was not joined
    virtual void closeMultiCast();
    virtual bool sendBytesMultiCast(uint8_t* buffer, uint16_t len);
    virtual int readBytesMultiCast(uint8_t* buffer, uint16_t maxLen);
    virtual int readBytesMultiCast(uint8_t* buffer, uint16_t maxLen, uint32_t& src_addr, uint16_t& src_port);
    // TTL of outgoing multicast datagrams (PID 67, 03_08_03 2.5.18). Kept by the platform across re-joins;
    // set it before setupMultiCast(). Default: no-op.
    virtual void multicastTtl(uint8_t ttl);
    // A second group on the routing socket, told apart per datagram by lastMulticastDestination(). Default: unsupported.
    virtual bool joinMultiCastExtra(uint32_t addr) { (void)addr; return false; }
    virtual bool multicastTtlAtJoinOnly() { return true; }    // a changed TTL reaches the socket at a join, if at all
    // True when a second multicast socket receives only its own group; not on the lwIP platforms here. Default: false.
    virtual bool multiCastSecondaryIsolated() { return false; }
    // Destination (host order) of the datagram the last readBytesMultiCast() returned: the joined group it was sent
    // to, otherwise an address that is no group (255.255.255.255); 0 if the platform cannot tell.
    virtual uint32_t lastMulticastDestination() { return 0; }
    // Datagrams received on the routing socket and not yet read. Default: the platform cannot tell.
    virtual uint16_t multicastRxBacklog() { return 0; }
    // A ROUTING_BUSY from another host is received and not yet read. Default: not tracked.
    virtual bool routingBusyPending() { return false; }
    // millis() at reception of the ROUTING_BUSY the last readBytesMultiCast() returned; fallback when not recorded.
    virtual uint32_t routingBusyReceivedAt(uint32_t fallback) { return fallback; }
    // Hands datagrams still waiting in the network chip to the stack (a ROUTING_BUSY among them). Default: no-op.
    virtual void pollReceive() {}
    // false: the platform keeps no receive queue of its own and counts nothing.
    virtual bool multicastRxStats(MulticastRxStats& stats) { (void)stats; return false; }

    // Second multicast socket for the system setup group (03_02_06 4.1.3). Default: not joined, nothing received.
    virtual bool setupMultiCastSecondary(uint32_t addr, uint16_t port); // false: the group was not joined
    virtual void closeMultiCastSecondary();
    virtual int readBytesMultiCastSecondary(uint8_t* buffer, uint16_t maxLen);

    //unicast socket
    virtual bool sendBytesUniCast(uint32_t addr, uint16_t port, uint8_t* buffer, uint16_t len);

    // SPI
    virtual void setupSpi();
    virtual void closeSpi();
    virtual int readWriteSpi(uint8_t *data, size_t len);

    //Memory

    // --- Overwrite these methods in the device-plattform to use the EEPROM Emulation API for UserMemory ----
    //
    // --- changes to the UserMemory are written directly into the address space starting at getEepromBuffer
    // --- commitToEeprom must save this to a non-volatile area if neccessary
    virtual uint8_t* getEepromBuffer(uint32_t size);
    virtual void commitToEeprom();
    // -------------------------------------------------------------------------------------------------------

    virtual uint8_t* getNonVolatileMemoryStart();
    virtual size_t getNonVolatileMemorySize();
    virtual void commitNonVolatileMemory();
    // address is relative to start of nonvolatile memory
    virtual uint32_t writeNonVolatileMemory(uint32_t relativeAddress, uint8_t* buffer, size_t size);
    virtual uint32_t readNonVolatileMemory(uint32_t relativeAddress, uint8_t* buffer, size_t size);
    virtual uint32_t writeNonVolatileMemory(uint32_t relativeAddress, uint8_t value, size_t repeat);

    NvMemoryType NonVolatileMemoryType();
    void NonVolatileMemoryType(NvMemoryType type);

    // --- Overwrite these methods in the device-plattform to use flash memory handling by the knx stack ---
    // --- also set _memoryType = Flash in the device-plattform's contructor
    // --- optional: overwrite writeBufferedEraseBlock() in the device-plattform to reduce overhead when flashing multiple pages

    // size of one flash page in bytes
    virtual size_t flashPageSize();
    
#ifdef KNX_FLASH_CALLBACK
    void registerFlashCallbacks(
        FlashCallbackSize callbackFlashSize,
        FlashCallbackRead callbackFlashRead,
        FlashCallbackWrite callbackFlashWrite,
        FlashCallbackCommit callbackFlashCommit);

    FlashCallbackSize callbackFlashSize();
    FlashCallbackRead callbackFlashRead();
    FlashCallbackWrite callbackFlashWrite();
    FlashCallbackCommit callbackFlashCommit();
#endif

  protected:
    // size of one EraseBlock in pages
    virtual size_t flashEraseBlockSize();
    // start of user flash aligned to start of an erase block
    virtual uint8_t* userFlashStart();
    // size of the user flash in EraseBlocks
    virtual size_t userFlashSizeEraseBlocks();
    //relativ to userFlashStart
    virtual void flashErase(uint16_t eraseBlockNum);
    //write a single page to flash (pageNumber relative to userFashStart
    virtual void flashWritePage(uint16_t pageNumber, uint8_t* data);

    TPUart::Interface::Abstract* _interface;

    // -------------------------------------------------------------------------------------------------------

    NvMemoryType _memoryType = Eeprom;

    void loadEraseblockContaining(uint32_t relativeAddress);
    int32_t getEraseBlockNumberOf(uint32_t relativeAddress);
    // writes _eraseblockBuffer to flash
    virtual void writeBufferedEraseBlock();
    // copies a EraseBlock into the _eraseblockBuffer
    void bufferEraseBlock(int32_t eraseBlockNumber);

    // in theory we would have to use this buffer for memory reads too,
    // but because ets always restarts the device after programming it
    // we can ignore this issue
    uint8_t* _eraseblockBuffer = nullptr;
    int32_t _bufferedEraseblockNumber = -1;
    bool _bufferedEraseblockDirty = false;

#ifdef KNX_FLASH_CALLBACK
    FlashCallbackSize _callbackFlashSize = nullptr;
    FlashCallbackRead _callbackFlashRead = nullptr;
    FlashCallbackWrite _callbackFlashWrite = nullptr;
    FlashCallbackCommit _callbackFlashCommit = nullptr;
#endif
};

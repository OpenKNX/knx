#pragma once

#include "knx_types.h"
#include "stdint.h"
#include "npdu.h"
#include "tpdu.h"
#include "apdu.h"
#include "config.h"

#define NPDU_LPDU_DIFF 8
#define TPDU_NPDU_DIFF 1
#define APDU_TPDU_DIFF 0
#define TPDU_LPDU_DIFF (TPDU_NPDU_DIFF + NPDU_LPDU_DIFF)
#define APDU_LPDU_DIFF (APDU_TPDU_DIFF + TPDU_NPDU_DIFF + NPDU_LPDU_DIFF)

// Mesg Code and additional info length
#define CEMI_HEADER_SIZE 2

// Largest APDU octet count the internal buffer can carry: the built LPDU occupies octetCount + 10 bytes
// (8 header + length octet + TPCI octet), buffer is 0xFF + APDU_LPDU_DIFF = 264 -> 264 - 10 = 254.
// 255 is the reserved escape and valid() drops it anyway; 254 is also PID_MAX_APDU_LENGTH.
#define MAX_APDU_OCTET_COUNT 254

class CemiFrame
{
    friend class DataLinkLayer;

  public:
    CemiFrame(uint8_t* data, uint16_t length);
    CemiFrame(uint16_t apduLength); // uint16_t, not uint8_t: a uint8_t parameter wrapped every apduLength > 255 silently
    CemiFrame(const CemiFrame& other);
    CemiFrame& operator=(CemiFrame other);

    MessageCode messageCode() const;
    void messageCode(MessageCode value);
    uint16_t totalLenght() const;
    uint16_t telegramLengthtTP() const;
    void fillTelegramTP(uint8_t* data);
    uint16_t telegramLengthtRF() const;
    void fillTelegramRF(uint8_t* data);
    uint8_t* data();
    const uint8_t* data() const; // read-only access for a const CemiFrame& (e.g. the IP encoders) -> lets them take the frame by const reference instead of by value (avoids a per-frame full-frame copy)
    uint16_t dataLength();

    FrameFormat frameType() const;
    void frameType(FrameFormat value);
    Repetition repetition() const;
    void repetition(Repetition value);
    SystemBroadcast systemBroadcast() const;
    void systemBroadcast(SystemBroadcast value);
    Priority priority() const;
    void priority(Priority value);
    AckType ack() const;
    void ack(AckType value);
    Confirm confirm() const;
    void confirm(Confirm value);
    AddressType addressType() const;
    void addressType(AddressType value);
    uint8_t hopCount() const;
    void hopCount(uint8_t value);
    uint16_t sourceAddress() const;
    void sourceAddress(uint16_t value);
    uint16_t destinationAddress() const;
    void destinationAddress(uint16_t value);

#ifdef USE_RF
    // only for RF medium
    uint8_t* rfSerialOrDoA() const;
    void rfSerialOrDoA(const uint8_t* rfSerialOrDoA);
    uint8_t rfInfo() const;
    void rfInfo(uint8_t rfInfo);
    uint8_t rfLfn() const;
    void rfLfn(uint8_t rfInfo);
#endif
    NPDU& npdu();
    TPDU& tpdu();
    APDU& apdu();

    uint8_t calcCrcTP(uint8_t* buffer, uint16_t len);
    bool valid() const;
    // True when the ctor could not carry the requested apduLength. The caller may then have written
    // past buffer[] into the members behind it, so nothing in this frame may be dereferenced.
    bool oversized() const { return _oversized; }

  private:
    // 264 bytes, and the size is load-bearing: the built LPDU occupies octetCount + 10 (8 header + length
    // octet + TPCI octet), so at the maximum octetCount of 254 the last payload byte lands on buffer[263].
    // Shrinking it reintroduces an out-of-bounds write. Valid only if additional info is zero.
    uint8_t buffer[0xff + APDU_LPDU_DIFF] = {0};
    uint8_t* _data = 0;
    uint8_t* _ctrl1 = 0;
    NPDU _npdu;
    TPDU _tpdu;
    APDU _apdu;
    uint16_t _length = 0; // only set if created from byte array
    bool _oversized = false; // apduLength > MAX_APDU_OCTET_COUNT was requested -> valid() is false, frame carries nothing

#ifdef USE_RF
    // FIXME: integrate this propery in _data
    // only for RF medium
    uint8_t* _rfSerialOrDoA = 0;
    uint8_t  _rfInfo = 0;
    uint8_t  _rfLfn = 0xFF; // RF Data Link layer frame number
 #endif

    uint8_t _sourceInterfaceIndex;
};

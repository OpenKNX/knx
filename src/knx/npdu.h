#pragma once

#include <stdint.h>

class CemiFrame;
class TPDU;

class NPDU
{
    friend class CemiFrame;

  public:
    uint8_t octetCount() const;
    void octetCount(uint8_t value);
    uint16_t length() const; // uint16: octetCount+2 reaches 256 at the spec-legal max octetCount 254 (0xFF=escape); uint8 overflowed to 0
    uint8_t hopCount() const;
    void hopCount(uint8_t value);
    CemiFrame& frame();
    TPDU& tpdu();

  protected:
    NPDU(uint8_t* data, CemiFrame& frame);

  private:
    uint8_t* _data = 0;
    CemiFrame& _frame;
};
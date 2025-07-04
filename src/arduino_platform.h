#pragma once

#include "knx/platform.h"

#include "Arduino.h"

#ifndef KNX_DEBUG_SERIAL
#define KNX_DEBUG_SERIAL Serial
#endif

class ArduinoPlatform : public Platform
{
  public:
    ArduinoPlatform();
    ArduinoPlatform(TPUart::Interface::Abstract* interface);

    // basic stuff
    void fatalError();

    //spi
#ifndef KNX_NO_SPI
    void setupSpi() override;
    void closeSpi() override;
    int readWriteSpi (uint8_t *data, size_t len) override;
#endif
#ifndef KNX_NO_PRINT
    static Stream* SerialDebug;
#endif

};

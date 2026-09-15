#include <Arduino.h>
#include <knx.h>
#include "TPUart/Interface/ArduinoSerial.h"

// The two OpenKNX KNXnet/IP products select their BAU from a mask PLUS KNX_TUNNELING:
//   0x07B0 + KNX_TUNNELING -> Bau07B0IP, the IP-Interface
//   0x091A + KNX_TUNNELING -> Bau091A,   the IP-Router
// Neither combination was compiled by any example, although both pull the tunnel server, the IP
// data link layer and tpuart_data_link_layer.cpp. This sketch exists to build them, so it carries
// no application of its own: no group objects, nothing mask specific, nothing that would make it
// fail for a reason other than the stack not compiling.

void setup()
{
    Serial.begin(115200);
    ArduinoPlatform::SerialDebug = &Serial;
    knx.platform().interface(new TPUart::Interface::ArduinoSerial(Serial1));

    randomSeed(millis());

    // read address table, association table, group object table and parameters from flash
    knx.readMemory();

    if (knx.configured())
        Serial.println("Configured.");

    knx.start();
}

void loop()
{
    // don't delay here too much, or packets are lost and the ETS timing is missed
    knx.loop();

    if (!knx.configured())
        return;
}

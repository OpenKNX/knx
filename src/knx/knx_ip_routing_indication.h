#pragma once

#include "knx_ip_frame.h"
#include "cemi_frame.h"
#ifdef USE_IP

class KnxIpRoutingIndication : public KnxIpFrame
{
  public:
    KnxIpRoutingIndication(uint8_t* data, uint16_t length);
    KnxIpRoutingIndication(const CemiFrame& frame); // by const ref: no per-frame CemiFrame copy on routing fan-out
    CemiFrame& frame();
  private:
    CemiFrame _frame;
};
#endif
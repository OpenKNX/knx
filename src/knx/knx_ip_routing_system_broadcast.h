#pragma once

#include "knx_ip_frame.h"
#include "cemi_frame.h"
#ifdef USE_IP

// 03_02_06 4.1.3: an IP System Broadcast Frame carries a plain cEMI L_Data.ind behind the KNXnet/IP
// header, exactly like a routing indication, and differs only in the service type and in the conditions
// the cEMI has to meet. It is always sent on the system setup multicast address and is never wrapped in
// a SECURE_WRAPPER.
class KnxIpRoutingSystemBroadcast : public KnxIpFrame
{
  public:
    KnxIpRoutingSystemBroadcast(uint8_t* data, uint16_t length);
    KnxIpRoutingSystemBroadcast(const CemiFrame& frame); // by const ref: no per-frame CemiFrame copy
    CemiFrame& frame();
    // The cEMI conditions of 03_02_06 4.1.3 that a receiver has to check before accepting the frame.
    bool cemiIsSystemBroadcast();
  private:
    CemiFrame _frame;
};
#endif

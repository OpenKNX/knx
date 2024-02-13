#pragma once

#include "knx_ip_frame.h"
#include "ip_host_protocol_address_information.h"
#ifdef USE_IP
class KnxIpSearchRequestExtended : public KnxIpFrame
{
  public:
    KnxIpSearchRequestExtended(uint8_t* data, uint16_t length);
    IpHostProtocolAddressInformation& hpai();
    bool srpByProgMode = false;
    bool srpByMacAddr = false;
    bool srpByService = false;
    bool srpRequestDIBs = false;
    uint8_t *srpMacAddr = nullptr;
  private:
    IpHostProtocolAddressInformation _hpai;
};
#endif
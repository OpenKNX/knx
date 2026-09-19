#include "knx_ip_routing_system_broadcast.h"
#include <cstring>

#ifdef USE_IP
CemiFrame& KnxIpRoutingSystemBroadcast::frame()
{
    return _frame;
}

KnxIpRoutingSystemBroadcast::KnxIpRoutingSystemBroadcast(uint8_t* data,
    uint16_t length) : KnxIpFrame(data, length), _frame(data + headerLength(), length - headerLength())
{
}

KnxIpRoutingSystemBroadcast::KnxIpRoutingSystemBroadcast(const CemiFrame& frame)
    : KnxIpFrame(frame.totalLenght() + LEN_KNXIP_HEADER), _frame(_data + headerLength(), frame.totalLenght())
{
    serviceTypeIdentifier(RoutingSystemBroadcast);
    memcpy(_data + LEN_KNXIP_HEADER, frame.data(), frame.totalLenght());
}

// 03_02_06 4.1.3: message code L_Data.ind, SB bit of control field 1 zero (which is what designates a
// system broadcast on an open medium), destination address type "group" and destination address 0.
// A receiver shall ignore a frame that does not meet all of them.
bool KnxIpRoutingSystemBroadcast::cemiIsSystemBroadcast()
{
    return _frame.messageCode() == L_data_ind
           && _frame.systemBroadcast() == SysBroadcast
           && _frame.addressType() == GroupAddress
           && _frame.destinationAddress() == 0;
}
#endif

#include "knx_ip_state_response.h"
#ifdef USE_IP

KnxIpStateResponse::KnxIpStateResponse(uint8_t channelId, uint8_t errorCode)
    : KnxIpFrame(LEN_KNXIP_HEADER + 2)
{
    serviceTypeIdentifier(ConnectionStateResponse);
    _data[LEN_KNXIP_HEADER] = channelId;
    _data[LEN_KNXIP_HEADER + 1] = errorCode;
}
#endif

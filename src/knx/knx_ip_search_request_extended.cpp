#include "knx_ip_search_request_extended.h"
#ifdef USE_IP
KnxIpSearchRequestExtended::KnxIpSearchRequestExtended(uint8_t* data, uint16_t length)
    : KnxIpFrame(data, length), _hpai(data + LEN_KNXIP_HEADER)
{
    if(length == LEN_KNXIP_HEADER + LEN_IPHPAI) return; //we dont have SRPs

    int currentPos = LEN_KNXIP_HEADER + LEN_IPHPAI;
    while(currentPos < length)
    {
        switch(data[currentPos+1])
        {
            case 0x01:
                srpByProgMode = true;
                break;

            case 0x02:
                srpByMacAddr = true;
                srpMacAddr = data + 2;
                break;

            case 0x03:
                srpByService = true;
                break;

            case 0x04:
                srpRequestDIBs = true;
                break;
        }
        currentPos += data[currentPos];
    };
}

IpHostProtocolAddressInformation& KnxIpSearchRequestExtended::hpai()
{
    return _hpai;
}
#endif
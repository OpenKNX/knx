#include "knx_ip_config_dib.h"

#ifdef USE_IP
KnxIpConfigDIB::KnxIpConfigDIB(uint8_t* data, bool isCurrent) : KnxIpDIB(data)
{
    _isCurrent = isCurrent;
}

uint32_t KnxIpConfigDIB::address()
{
    uint32_t addr = 0;
    popInt(addr, _data + 2);
    return addr;
}

void KnxIpConfigDIB::address(uint32_t addr)
{
    pushInt(addr, _data + 2);
}

uint32_t KnxIpConfigDIB::subnet()
{
    uint32_t addr = 0;
    popInt(addr, _data + 6);
    return addr;
}

void KnxIpConfigDIB::subnet(uint32_t addr)
{
    pushInt(addr, _data + 6);
}

uint32_t KnxIpConfigDIB::gateway()
{
    uint32_t addr = 0;
    popInt(addr, _data + 10);
    return addr;
}

void KnxIpConfigDIB::gateway(uint32_t addr)
{
    pushInt(addr, _data + 10);
}

uint32_t KnxIpConfigDIB::dhcp()
{
    if(!_isCurrent) return 0;
    uint32_t addr = 0;
    popInt(addr, _data + 14);
    return addr;
}

void KnxIpConfigDIB::dhcp(uint32_t addr)
{
    if(!_isCurrent) return;
    pushInt(addr, _data + 14);
}

// info1/info2 are the two trailing fields; their offsets differ between the two DIB layouts (03_08_02 Core
// Figures 16/17):
//   IP_CONFIG      (0x03, 16 B): ...gateway[10..13], IP Capabilities[14], IP assignment method[15]
//   IP_CUR_CONFIG  (0x04, 20 B): ...gateway[10..13], DHCP Server[14..17], Current IP assignment method[18], Reserved[19]
// Hence current -> 18/19, non-current -> 14/15. (These two branches were previously swapped, which wrote the
// 0x04 method into the DHCP-server field and read/wrote the 0x03 fields out of the 16-byte DIB.)
uint8_t KnxIpConfigDIB::info1()
{
    if(_isCurrent)
        return _data[18];
    else
        return _data[14];
}

void KnxIpConfigDIB::info1(uint8_t addr)
{
    if(_isCurrent)
        _data[18] = addr;
    else
        _data[14] = addr;
}

uint8_t KnxIpConfigDIB::info2()
{
    if(_isCurrent)
        return _data[19];
    else
        return _data[15];
}

void KnxIpConfigDIB::info2(uint8_t addr)
{
    if(_isCurrent)
        _data[19] = addr;
    else
        _data[15] = addr;
}

#endif
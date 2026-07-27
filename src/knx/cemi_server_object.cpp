#include "config.h"
#ifdef USE_CEMI_SERVER

#include <cstring>
#include "cemi_server_object.h"
#include "bits.h"
#include "data_property.h"

CemiServerObject::CemiServerObject()
{
    Property* properties[] =
    {
        new DataProperty( PID_OBJECT_TYPE, false, PDT_UNSIGNED_INT, 1, ReadLv3 | WriteLv0, (uint16_t)OT_CEMI_SERVER ),
        new DataProperty( PID_MEDIUM_TYPE, false, PDT_BITSET16, 1, ReadLv3 | WriteLv0, (uint16_t)0),
        new DataProperty( PID_COMM_MODE, false, PDT_ENUM8, 1, ReadLv3 | WriteLv0, (uint16_t)0),
        new DataProperty( PID_COMM_MODES_SUPPORTED, false, PDT_BITSET16, 1, ReadLv3 | WriteLv0, (uint16_t)0x100),
        new DataProperty( PID_MEDIUM_AVAILABILITY, false, PDT_BITSET16, 1, ReadLv3 | WriteLv0, (uint16_t)0),
#ifdef OPENKNX_HW_BUSMON
        // PID_ADD_INFO_TYPES (03_05_01 §4.7.4, p.112): array of the additional-info types this cEMI server
        // sends to the client. Our busmon L_Busmon.ind carries 0x03 (bus monitor status) + 0x06 (extended
        // relative timestamp), so declare both. maxElements=2, filled after initializeProperties() below.
        new DataProperty( PID_ADD_INFO_TYPES, false, PDT_ENUM8, 2, ReadLv3 | WriteLv0 ),
        // PID_TIME_BASE (03_05_01 §4.7.5, p.112): ns per tick of the free-running counter used for the
        // EXTENDED relative timestamp (AddInfo type 0x06) in our L_Busmon.ind. We latch micros(), so
        // 1 tick = 1 us = 1000 ns. Lets the cEMI client (ETS) render true inter-frame times. Busmon-only.
        new DataProperty( PID_TIME_BASE, false, PDT_UNSIGNED_INT, 1, ReadLv3 | WriteLv0, (uint16_t)1000)
#else
        // cEMI additional info types supported by this cEMI server: only 0x02 (RF Control Octet and Serial Number or DoA)
        new DataProperty( PID_ADD_INFO_TYPES, false, PDT_ENUM8, 1, ReadLv3 | WriteLv0, (uint8_t)0x02)
#endif
    };
    initializeProperties(sizeof(properties), properties);

#ifdef OPENKNX_HW_BUSMON
    // Fill the (empty-constructed) supported-AI-types array: {0x03 bus monitor status, 0x06 ext timestamp}.
    const uint8_t supportedAiTypes[2] = {0x03, 0x06};
    property(PID_ADD_INFO_TYPES)->write((uint16_t)1, (uint8_t)2, supportedAiTypes);
#endif
}

void CemiServerObject::setMediumTypeAsSupported(DptMedium dptMedium)
{
    uint16_t mediaTypesSupported;
    property(PID_MEDIUM_TYPE)->read(mediaTypesSupported);

    switch(dptMedium)
    {
    case DptMedium::KNX_IP:
        mediaTypesSupported |= 1 << 1;
        break;
    case DptMedium::KNX_RF:
        mediaTypesSupported |= 1 << 4;
        break;
    case DptMedium::KNX_TP1:
        mediaTypesSupported |= 1 << 5;
        break;
    case DptMedium::KNX_PL110:
        mediaTypesSupported |= 1 << 2;
        break;
    }

    property(PID_MEDIUM_TYPE)->write(mediaTypesSupported);
    // We also set the medium as available too
    property(PID_MEDIUM_AVAILABILITY)->write(mediaTypesSupported);
}

void CemiServerObject::clearSupportedMediaTypes()
{
    property(PID_MEDIUM_TYPE)->write((uint16_t) 0);
    // We also set the medium as not available too
    property(PID_MEDIUM_AVAILABILITY)->write((uint16_t) 0);
}

#endif


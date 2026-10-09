#pragma once

#include "config.h"

#ifndef KNX_SERVICE_FAMILY_CORE
#define KNX_SERVICE_FAMILY_CORE 1
#endif

#ifndef KNX_SERVICE_FAMILY_DEVICE_MANAGEMENT
// 2 = cEMI T_Data_Individual/Connected on the device management connection (03_08_03 4.2.5 p.23).
#ifdef KNX_CEMI_TRANSPORT_LAYER
#define KNX_SERVICE_FAMILY_DEVICE_MANAGEMENT 2
#else
#define KNX_SERVICE_FAMILY_DEVICE_MANAGEMENT 1
#endif
#endif

#ifndef KNX_SERVICE_FAMILY_TUNNELING
#define KNX_SERVICE_FAMILY_TUNNELING 1
#endif

#ifndef KNX_SERVICE_FAMILY_ROUTING
// 2 = IP system broadcast routing (08_TSSK 2.4.30); RP2040 only until the ESP32 routing socket is verified on hardware.
#if defined(ARDUINO_ARCH_RP2040) && (defined(KNX_IP_LAN) || defined(KNX_IP_WIFI))
#define KNX_SERVICE_FAMILY_ROUTING 2
#else
#define KNX_SERVICE_FAMILY_ROUTING 1
#endif
#endif

// Supported Service Families DIB (03_08_02 7.5.4.3): 2 header octets plus one entry per family.
#define LEN_SERVICE_FAMILIES 2 // octets per entry: family id + version

// Families in SEARCH_RESPONSE and DESCRIPTION_RESPONSE: Core; Device Management and Tunnelling only with
// KNX_TUNNELING (both served by IpTunnelServer); Routing only on a router (0x091A).
#ifdef KNX_TUNNELING
#define KNX_SERVICE_FAMILY_COUNT_TUNNEL 2
#else
#define KNX_SERVICE_FAMILY_COUNT_TUNNEL 0
#endif
#ifdef KNX_IS_ROUTER
#define KNX_SERVICE_FAMILY_COUNT_ROUTING 1
#else
#define KNX_SERVICE_FAMILY_COUNT_ROUTING 0
#endif
#define LEN_SUPP_SVC_DIB (2 + (1 + KNX_SERVICE_FAMILY_COUNT_TUNNEL + KNX_SERVICE_FAMILY_COUNT_ROUTING) * LEN_SERVICE_FAMILIES)

// SEARCH_RESPONSE_EXTENDED lists Device Management also without KNX_TUNNELING, so its length differs there.
#ifdef KNX_IS_ROUTER
#ifdef KNX_TUNNELING
#define LEN_SUPP_SVC_DIB_EXT (2 + 4 * LEN_SERVICE_FAMILIES)
#else
#define LEN_SUPP_SVC_DIB_EXT (2 + 3 * LEN_SERVICE_FAMILIES)
#endif
#else
#ifdef KNX_TUNNELING
#define LEN_SUPP_SVC_DIB_EXT (2 + 3 * LEN_SERVICE_FAMILIES)
#else
#define LEN_SUPP_SVC_DIB_EXT (2 + 2 * LEN_SERVICE_FAMILIES)
#endif
#endif

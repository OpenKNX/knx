#pragma once

#ifndef NO_KNX_CONFIG

#ifdef ARDUINO_ARCH_SAMD
#define SPI_SS_PIN 10
#define GPIO_GDO2_PIN 9
#define GPIO_GDO0_PIN 7
#else                    // Linux Platform (Raspberry Pi)
#define SPI_SS_PIN 8     // GPIO 8  (SPI_CE0_N) -> WiringPi: 10 -> Pin number on header: 24
#define GPIO_GDO2_PIN 25 // GPIO 25 (GPIO_GEN6) -> WiringPi: 6  -> Pin number on header: 22
#define GPIO_GDO0_PIN 24 // GPIO 24 (GPIO_GEN5) -> WiringPi: 5  -> Pin number on header: 18
#endif

// ── MASK_VERSION reference — KNX Device Descriptor Type 0 ("mask version") ─────────────────────
// Authority: KNX Std 03_05_01 Resources v01.10.01, §4.1.2 (Table 7, p.25-26). 16-bit format:
//   MMMM TTTT VVVV SSSS  =  Medium | Firmware(Mask) Type | Version | Subcode
//   Medium nibble MMMM:  0=TP1  1=PL110  2=RF  3=TP0  4=PL132  5=KNX-IP
//
// This stack builds the "System B" family (firmware type 7, version B) + the KNXnet/IP router:
//   0x07B0   TP1,      System B           normal TP device. + -D KNX_TUNNELING => IP-INTERFACE: it stays
//                                         0x07B0 (NOT a coupler) so it never advertises ROUTING and the
//                                         HW busmonitor stays spec-conform. See KNX_IS_* / KNX_DEVICE_TYPE below.
//   0x57B0   KNX-IP,   System B           pure KNXnet/IP device (no TP-UART)
//   0x091A   IP+TP1,   KNXnet/IP Router   coupler; advertises ROUTING; TP-UART is the SECONDARY DLL,
//                                         the primary is IP (per Table 7: "IP (prim) / TP1 (sec)")
//   0x27B0   RF,       System B           } NOT in Table 7 of this spec version (only the medium-nibble
//   0x2920   TP1/RF,   media coupler      } pattern) -- thelsing-stack convention, kept for completeness
// Full approved catalogue (BCU1 0x001x, BCU2 0x002x, System 300, BIM M112 0x070x, USB ifaces, Coupler
// 1.x 0x091x, IR, PL110 System B 0x17B0, Media-Coupler PL-TP 0x1900, ...) is Table 7 in the cited spec;
// not all are buildable by this stack.
//
// Normally set via -D MASK_VERSION=0x.. in platformio; or uncomment exactly one:
//#define MASK_VERSION 0x07B0
//#define MASK_VERSION 0x27B0
//#define MASK_VERSION 0x57B0
//#define MASK_VERSION 0x091A
//#define MASK_VERSION 0x2920

// ── Capabilities + device type/role — decoded ONCE per MASK_VERSION ────────────────────────────
// The mask (a raw KNX Device Descriptor Type 0 value) is decoded HERE, once. Downstream code uses the
// semantic names below and NEVER a raw `MASK_VERSION == 0x..`, so nobody has to keep mask values in mind:
//   capability :  #ifdef KNX_HAS_TP / KNX_HAS_IP / KNX_HAS_RF      (device has that physical layer)
//   role       :  #ifdef KNX_IS_INTERFACE   (or _ROUTER / _TP / _IPDEVICE / _RF / _TPRF)
//   name       :  KNX_DEVICE_TYPE           (human-readable, e.g. for a "KNX-Type" log line)
//   group obj  :  #if KNX_HAS_GROUPOBJECTS  (0/1, overridable — see below)
//   TP access  :  KNX_TP_DLL              (the TP-UART DLL, primary on 0x07B0 / secondary on the router)
// USE_TP/USE_IP/USE_RF stay too — they are the knx stack's own upstream primitives; KNX_HAS_* alias them
// so OUR code reads consistently without forking upstream.
#if MASK_VERSION == 0x07B0
#define USE_TP
#ifdef KNX_TUNNELING
// 0x07B0 + KNX_TUNNELING = an IP Interface (Bau07B0IP): a TP device running a KNXnet/IP tunnelling server.
// Routing is NOT enabled (the ROUTING DIB stays gated to 0x091A), so it stays an interface, not a coupler
// -> the HW busmonitor stays spec-conform. The mask alone can NOT tell this from a plain TP device (both
// are 0x07B0) -- KNX_TUNNELING is the distinguishing marker.
#define USE_IP
#define KNX_IS_INTERFACE
#define KNX_DEVICE_TYPE "IP-Interface"
#else
#define KNX_IS_TP
#define KNX_DEVICE_TYPE "TP"
#endif

#elif MASK_VERSION == 0x27B0
#define USE_RF
#define KNX_IS_RF
#define KNX_DEVICE_TYPE "RF"

#elif MASK_VERSION == 0x57B0
#define USE_IP
#define KNX_IS_IPDEVICE
#define KNX_DEVICE_TYPE "IP"

#elif MASK_VERSION == 0x091A
#define USE_TP
#define USE_IP
#define KNX_IS_ROUTER
#define KNX_DEVICE_TYPE "Router"

#elif MASK_VERSION == 0x2920
#define USE_TP
#define USE_RF
#define KNX_IS_TPRF
#define KNX_DEVICE_TYPE "TP/RF"

#else
#define KNX_DEVICE_TYPE "?"
#endif

// KNX_HAS_* — capability aliases of the upstream USE_* primitives, so downstream code reads consistently.
#ifdef USE_TP
#define KNX_HAS_TP
#endif
#ifdef USE_IP
#define KNX_HAS_IP
#endif
#ifdef USE_RF
#define KNX_HAS_RF
#endif

// KNX_HAS_GROUPOBJECTS (0/1) — a device has group objects unless it is a coupler (coupler masks match
// 0x_9__). This is only the DEFAULT: a product may override it via -D KNX_HAS_GROUPOBJECTS=1 / =0 (e.g. a
// System-B based router that still exposes KOs, or a future spec allowing coupler KOs). So the
// "coupler => no group objects" inference is a sensible default, not a hard-wired rule.
#ifndef KNX_HAS_GROUPOBJECTS
#if (MASK_VERSION & 0x0900) != 0x0900
#define KNX_HAS_GROUPOBJECTS 1
#else
#define KNX_HAS_GROUPOBJECTS 0
#endif
#endif

// KNX_UNCONFIGURED_ADDRESS — the factory-default individual address of an unprogrammed device:
// 15.15.0 (0xFF00) for a coupler/router, 15.15.255 (0xFFFF) for a normal device.
#ifdef KNX_IS_ROUTER
#define KNX_UNCONFIGURED_ADDRESS 0xFF00
#else
#define KNX_UNCONFIGURED_ADDRESS 0xFFFF
#endif

// KNX_TP_DLL — "the TP-UART data link layer", regardless of entity index. Expanded only in translation
// units that see the knx facade (it references knx.bau()); undefined on an IP-only device (no TP).
#if defined(KNX_IS_ROUTER)
#define KNX_TP_DLL (knx.bau().getSecondaryDataLinkLayer())
#elif defined(KNX_HAS_TP)
#define KNX_TP_DLL (knx.bau().getDataLinkLayer())
#endif

// cEMI options
//#define USE_USB
//#define USE_CEMI_SERVER
#if defined(USE_USB) || defined(KNX_TUNNELING)
#define USE_CEMI_SERVER
#endif

// KNX Data Secure Options
// Define via a compiler -D flag if required
// #define USE_DATASECURE

// option to have GroupObjects (KO in German) use 8 bytes mangement information RAM instead of 19 bytes
// see knx-demo-small-go for example
// this option might be also set via compiler flag -DSMALL_GROUPOBJECT if required
//#define SMALL_GROUPOBJECT

// Some defines to reduce footprint
// Do not perform conversion from KNXValue(const char*) to other types, it mainly avoids the expensive strtod
//#define KNX_NO_STRTOx_CONVERSION
// Do not print messages
//#define KNX_NO_PRINT
// Do not use SPI (Arduino variants)
//#define KNX_NO_SPI
// Do not use the default UART (Arduino variants), it must be defined by ArduinoPlatform::knxUart
// (combined with other flags (HWSERIAL_NONE for stm32) - avoid allocation of RX/TX buffers for all serial lines)
//#define KNX_NO_DEFAULT_UART 

#endif

#if !defined(MASK_VERSION)
#error MASK_VERSION must be defined! See config.h for possible values!
#endif


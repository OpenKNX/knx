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
// LIMIT: overriding this to 1 on a coupler mask does not by itself deliver group objects -- only
// BauSystemBDevice owns a group object table, so the facade accessor would then fail to compile. The
// override is honoured everywhere the flag is read; it cannot conjure the table.
#ifndef KNX_HAS_GROUPOBJECTS
#if (MASK_VERSION & 0x0900) != 0x0900
#define KNX_HAS_GROUPOBJECTS 1
#else
#define KNX_HAS_GROUPOBJECTS 0
#endif
#endif

// KNX_TUNNELING_DEVMGMT — KNXnet/IP device-management connections served alongside the data tunnels.
// Sizes the tunnels[] array together with KNX_TUNNELING.
#ifndef KNX_TUNNELING_DEVMGMT
#define KNX_TUNNELING_DEVMGMT 1
#endif

// KNX_CEMI_TRANSPORT_STRICT — refuse a Device-Management connect while ANY transport connection is open,
// including the requesting client's own. Default OFF: only a FOREIGN peer is refused.
//
// 08_TSSH 8.3.2 p.158 (fn 60202) drives the refusal with a peer the test controller sends in
// (`L_Data.ind 1.1.255 -> 15.15.255 Connect` as a ROUTING_INDICATION), so the connection holding the layer
// is NOT the client that afterwards opens Device Management over its own unicast endpoint. 03_08_03 2.6.1.2
// p.18 prescribes no refusal at all -- it only describes the implicit switch to cEMI Transport Layer mode.
// Counting the client's own connection made ETS lock itself out: it reads the group-address tables over a
// T_Connect through its tunnel, then asks for Device Management and is refused until the 6 s connection
// timeout of 03_03_04 5.1 releases the connection it never closed -- then retries and blocks itself again.
// Set this only for a certification run that demands the stricter reading.
// #define KNX_CEMI_TRANSPORT_STRICT

// KNX_TUNNEL_RESEND — server-side repetition of an unconfirmed TUNNELLING_REQUEST. ON by default, because
// 03_08_04 2.6.1 p.9 makes it mandatory, not optional: "If a TUNNELLING_REQUEST frame is not confirmed
// within the TUNNELLING_REQUEST_TIMEOUT time of one (1) second then the frame SHALL be repeated once with
// the same sequence counter value by the sending KNXnet/IP device" -- and a tunnelling server is a sending
// device the moment it forwards bus traffic to its client. Without it an unacknowledged telegram vanishes
// silently and the client believes it has seen the whole bus.
//
// Cost: KNX_TUNNEL_RESEND_DEPTH * KNX_TUNNEL_RESEND_BUF per connection (~15 kB at depth 3 over 16 tunnels
// plus device management). A product that cannot afford that should lower the DEPTH to 1 -- which still
// satisfies the clause, since it demands exactly ONE repetition -- rather than switch the feature off.
// KNX_NO_TUNNEL_RESEND exists for a build that genuinely cannot carry even that; it is then knowingly
// non-conformant on 2.6.1.
#if !defined(KNX_TUNNEL_RESEND) && !defined(KNX_NO_TUNNEL_RESEND)
#define KNX_TUNNEL_RESEND
#endif

// KNX_TUNNEL_RESEND_BUF — largest server->client tunnelling datagram a FIFO slot must hold, incl. extended
// frames (memory read/write, long property responses during programming): KNXnet/IP hdr 6 + connection hdr
// 4 + cEMI (<= ~9 + maxAPDU 254) ~= 273. Undersizing this silently drops large frames and re-breaks device
// reading/programming.
#ifndef KNX_TUNNEL_RESEND_BUF
#define KNX_TUNNEL_RESEND_BUF 280
#endif

// KNX_TUNNEL_RESEND_DEPTH — FIFO slots per connection (03_08_04 2.6.1 p.9). Default 1: that is exactly what
// the clause demands ("repeated ONCE"), at ~5 kB over 16 tunnels plus device management, so the mandatory
// behaviour costs a small product the least it can. Raise it where the RAM is there -- the device's
// connection-oriented layer is window-1 and a client acks in milliseconds, so 2-3 is margin for bursts, and
// only a depth > 1 makes the overflow policy meaningful (a connection-oriented frame evicts the oldest
// queued GROUP frame instead of tearing the download down). A stuck client that overruns it is disconnected.
#ifndef KNX_TUNNEL_RESEND_DEPTH
#define KNX_TUNNEL_RESEND_DEPTH 1
#endif

// KNX_BUSMON_CONNECTIONS — how many KNXnet/IP busmonitor connections the tunnel server serves at once.
// 03_08_04 2.2.4 p.8 asks for ONE per KNX subnetwork, which is the default. The capture is a read-only
// fan-out of one bus stream, so extra readers cost nothing on the bus -- but each slot carries a
// KnxIpTunnelConnection, and with KNX_TUNNEL_RESEND that is ~840 octets of send FIFO the busmonitor never
// uses. Raise only with that in mind, and only once it is settled with the KNX Association.
#ifndef KNX_BUSMON_CONNECTIONS
#define KNX_BUSMON_CONNECTIONS 1
#endif

// KNX_UNCONFIGURED_ADDRESS — the factory-default individual address of an unprogrammed device:
// 15.15.0 (0xFF00) for a coupler/router, 15.15.255 (0xFFFF) for a normal device.
// Must match DeviceObject::_ownAddress (device_object.h), which is the value actually loaded -- 0x2920 is
// a coupler there too, so keying this on KNX_IS_ROUTER alone made the two disagree for that mask.
#if defined(KNX_IS_ROUTER) || (MASK_VERSION == 0x2920)
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

// Guards for flag combinations that do not build, or build into something inconsistent.
#if defined(KNX_TUNNELING) && !defined(USE_IP)
    #error "KNX_TUNNELING needs an IP-capable mask (07B0, 57B0 or 091A) -- it pulls in the KNXnet/IP tunnel server"
#endif
#if defined(KNX_TUNNELING) && (MASK_VERSION == 0x57B0)
    #error "MASK_VERSION 0x57B0 with KNX_TUNNELING has no BAU: the facade selects Bau57B0, which cannot serve a tunnel"
#endif
#if defined(OPENKNX_HW_BUSMON) && !defined(KNX_TUNNELING)
    #error "OPENKNX_HW_BUSMON needs KNX_TUNNELING: the busmonitor is served over a KNXnet/IP tunnelling connection, and both the forward path and the connect handler are compiled only with it"
#endif
#if (KNX_SERVICE_FAMILY_CORE >= 2)
    #error "KNX_SERVICE_FAMILY_CORE >= 2 (Core v2 / extended search) does not build: knx_ip_search_response_extended.cpp has never been compiled on any mask -- see finding 4.5 before raising it"
#endif
#if defined(KNX_TUNNELING) && (KNX_TUNNELING < 1)
    #error "KNX_TUNNELING must be >= 1 -- omit the flag to build without tunnelling; 0 compiles a tunnel server that advertises the service and refuses every connection, and declares a zero-length address buffer"
#endif
#if defined(USE_CEMI_SERVER) && !defined(KNX_TUNNELING)
    #error "USE_CEMI_SERVER without KNX_TUNNELING is not wired: every BAU declares its cEMI/tunnel server under KNX_TUNNELING, so the USE_USB arm references a member that does not exist"
#endif
#if defined(KNX_TUNNELING_STRICT_TOPOLOGY) && !defined(KNX_IS_ROUTER)
    #error "KNX_TUNNELING_STRICT_TOPOLOGY is a coupler feature: on a device it withholds every tunnelled unicast from TP and confirms it as sent"
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


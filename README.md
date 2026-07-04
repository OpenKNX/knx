# knx

This is a fork of the [thelsing/knx](https://github.com/thelsing/knx) stack from [Thomas Kunze](https://github.com/thelsing) for and by the [OpenKNX Team](https://github.com/OpenKNX).

This projects provides a knx-device stack for arduino (ESP32 and RP2040).
It implements most of System-B specification and can be configured with ETS.
The necessary knxprod-files can be generated with the [Kaenx-Creator](https://github.com/OpenKNX/Kaenx-Creator) tool.

## Contents

- [Usage](#usage)
- [Changelog](#changelog)
- [Architecture overview](#architecture-overview)
  - [BAU family](#bau-family)
  - [cEMI, tunneling and routing](#cemi-tunneling-and-routing)
  - [Entry point](#entry-point)
- [Build flags / configuration switches](#build-flags--configuration-switches)
  - [Medium and mask selection](#medium-and-mask-selection)
  - [Tunneling](#tunneling)
  - [Routing](#routing)
  - [KNXnet/IP service families](#knxnetip-service-families)
  - [Memory / flash](#memory--flash)
  - [Platform / driver (RP2040 DMA)](#platform--driver-rp2040-dma)
  - [Diagnostics and footprint reduction](#diagnostics-and-footprint-reduction)

## Usage
See the [examples](examples/) for basic usage options.


## Changelog

### v1dev (replace this with version and date when releasing to v1)

- Fix: `uniqueSerialNumber()` returned no unique id on RP2350. Use `pico_get_unique_board_id()`, this  is compatible to previous implementation:
  - For RP2040 it will return the same serial as `flash_get_unique_id()` in old implementation
  - For RP2350 it uses an OTP-based unique chip ID
- deactivate by default not KNX-Standard compatible 'Tunnel-Optimization" which reduces traffic on TP (can be used with compiler option KNX_TUNNELING_NO_TUNNEL_PA_ON_TP and KNX_TUNNELING_STRICT_TOPOLOGY)
- refactor KNX IP Tunneling
- the Device Management Connection is now handled independently of Tunnel Connections and do not consume a Tunnel PA anymore
  new compiler option KNX_TUNNELING_DEVMGMT (default = 1) to set the number of available Device Management Connections
- new Compiler Option KNX_ROUTING_BC_DC: Unicast packets from the device itself is sent to both interfaces (IP and TP in case of 0x091A)
- change default PID_MAX_APDU_LENGTH_ROUTER from 220 to 254
- fix broken ConfigReq Responses
- fix programming application when FlashTablesInvalid for 0x091A
- add GroupObject::valueCompareTime() to send a GroupObject only when value changed or after some time without sending

### v2.3.1 - 2026-03-04

- Hotfix: DPT16 was not correctly handled for uninitialized KOs

### v2.3.0 - 2026-01-28
- Fix Define for 'DPT_FlowRate_m3/h'
- Enhance multicast initialization logging
- Allow write to hidden KO for DPT of size > 1 Byte
- Add function paramString to access string parameters
- Update TPUart dependency to version 1.0.4


### v2.2.2 - 2025-10-21
- Fix: DPT subgroup 0 handling

### v2.2.1 - 2025-08-22
- Fix: Distinguish between tunnel and TP PAs when reading PID_ADDITIONAL_INDIVIDUAL_ADDRESSES. This resulted in a failed PA assignment for 0x091A devices with KNX_TUNNELING
- Fix: set repeat correctly in DataLinkLayer when sending to other mediums as TP (https://github.com/OpenKNX/knx/issues/40)
- Fix: `dataConReceived` is not suppressed anymore. This prevented sending device reset telegrams.
- Fix: Update TPUart lib to 1.0.2
- Fix: Add individual address handling in TpUartDataLinkLayer
- Fix: [Unload application was not permanent](https://github.com/thelsing/knx/issues/144)
- Extend documentation for KO-state

### V2.2.0 - 2025-07-04
- Fix [#30](https://github.com/OpenKNX/knx/pull/30): Unexpected behaviour of `GroupObject` on failed conversion to DPT
  - `GroupObject::value[No]SendCompare(..)` resulted in value 0 (and returned change based on this value)
  - `GroupObject::valueNoSend(..)` updated state from unitialized to OK, without updating the value
  - `GroupObject::value(..)` wrote to GA without setting the KO value
- Extension [#30](https://github.com/OpenKNX/knx/pull/30): Return successful conversion to DPT on values update operations in `GroupObject` (changed result-type of some methods from `void` to `bool`) 
- only set pinMode of Prog button pin if valid (PROG_BUTTON_PIN >= 0)
- Strings are now \0 terminated in group objects (#25)
- change defines in the rp2040 plattform for LAN / WLAN usage to KNX_IP_LAN or KNX_IP_WIFI, remove KNX_IP_GENERIC
- better Routing and Tunneling support
- add DPT 27.001
- increase device object api version to 2 (invalidation of knx flash data stored by older versions)
- add #pragma once to Arduino plattform to allow derived plattforms
- change esp32 plattform to use KNX_NETIF
- remove examples to deprecated plattforms, update remaining examples
- use tpuart library (https://github.com/OpenKNX/tpuart)

### V2.1.2 - 2024-12-09
- adds unicast auto ack

### V2.1.1 - 2024-09-16
- fix minor bug in TP-Uart Driver (RX queue out of boundary)

### V2.1.0 - 2024-07-03
- complete rework of the TPUart DataLinkLayer with support interrupt-based handling and optimized queue handling
- added DMA support for RP2040 platform
- fix some issues with continous integration causing github actions to fail
- added rp2040 plattform to knx-demo example
- added bool GroupObject::valueCompare method for only sending the value when it has changed 

### V2.0.0 - 2024-02-13
- first OpenKNX version


## Architecture overview

This is an *overview only* — see the source in `src/knx/` and `src/` for details.

The stack is organized as a classic KNX layer stack. A telegram travels top-down on
send and bottom-up on receive:

```
        Application code (sketch / OpenKNX modules)
                        |
                    knx_facade.h            <- KnxFacade<Platform, Bau> "knx" global; knx.loop()
                        |  knx.loop() -> _bau.loop()
        +-----------------------------------------------+
        |                 BAU  (Bus Access Unit)        |   bau*.h / bau*.cpp
        |   selected at compile time by MASK_VERSION    |
        |                                               |
        |   Application Layer      application_layer.*  |   (+ secure_application_layer.* if USE_DATASECURE)
        |          |                                    |
        |   Transport Layer        transport_layer.*    |
        |          |                                    |
        |   Network Layer          network_layer_*.*    |   device vs. coupler variant
        |          |                                    |
        |   Data Link Layer        data_link_layer.*    |   base class + medium variants:
        |     TP  tpuart_data_link_layer.*  (USE_TP)    |
        |     IP  ip_data_link_layer.*      (USE_IP)    |
        |     RF  rf_data_link_layer.*      (USE_RF)    |
        +-----------------------------------------------+
                        |
                    Platform / driver       platform.* + *_platform.* (esp32, rp2040, samd, stm32, linux, cc1310)
                        |
                  physical medium (TP-UART / Ethernet+UDP multicast / CC1101 RF)
```

### BAU family

The `Bus Access Unit` ties the layers together and is chosen at compile time via
`MASK_VERSION`. All concrete BAUs derive from `BusAccessUnit` (`bau.h`) through
`BauSystemB` (`bau_systemB.h`):

| Mask | Class | Role | Media |
| --- | --- | --- | --- |
| `0x07B0` | `Bau07B0` | Device | TP |
| `0x27B0` | `Bau27B0` | Device | RF |
| `0x57B0` | `Bau57B0` | Device | IP |
| `0x091A` | `Bau091A` | Coupler | IP + TP |
| `0x2920` | `Bau2920` | Coupler | TP + RF |

Device BAUs (`bau_systemB_device.h`) own an `ApplicationLayer`, a `TransportLayer`,
a `NetworkLayerDevice` and one medium `DataLinkLayer`. Coupler BAUs
(`bau_systemB_coupler.h`) use a `NetworkLayerCoupler` with a *primary* and a
*secondary* `DataLinkLayer` and route between them.

### cEMI, tunneling and routing

- **cEMI server** (`cemi_server.*`) is compiled in via `USE_CEMI_SERVER` and provides
  the local cEMI interface used by USB (`USE_USB`) and by IP tunneling.
- **IP tunneling** (`ip_tunnel_server.*`, guarded by `KNX_TUNNELING`) sits on top of the
  IP data link layer and manages tunnel + device-management connections.
- **Routing** on couplers is handled by `NetworkLayerCoupler` (`network_layer_coupler.*`),
  which forwards frames between the primary and secondary interfaces; IP routing
  itself is multicast handled in `ip_data_link_layer.*`.

### Entry point

`knx_facade.h` provides the templated `KnxFacade<Platform, Bau>` and (unless
`KNX_NO_AUTOMATIC_GLOBAL_INSTANCE` is set) a ready-made global `knx` instance for the
current platform + mask. The sketch calls `knx.loop()` every cycle, which drives
`_bau.loop()` and from there all layers. The `Platform` object is injected into the BAU
at construction and abstracts UART/network/flash access.

> Note: the exact set of layers/objects instantiated depends on `MASK_VERSION` and the
> `USE_*` switches below; the diagram shows the general shape, not every conditional member.

## Build flags / configuration switches

Compile-time switches (set with `-D` compiler flags, or in `src/knx/config.h`). All
information below is taken directly from the source; a *default* is only stated where
the code actually provides one via `#ifndef X` / `#define X <val>`. Everything else is
"unset by default" (the feature is off / the `#else` branch applies unless you define
it).

### Medium and mask selection

| Flag | Default | Effect |
| --- | --- | --- |
| `MASK_VERSION` | unset — build error if missing (`config.h`) | Selects the device/coupler profile and derives the enabled media. `0x07B0`=TP device, `0x27B0`=RF device, `0x57B0`=IP device, `0x091A`=IP/TP coupler, `0x2920`=TP/RF coupler. Compared throughout via `#if MASK_VERSION == ...` (`config.h`, `knx_facade.h`, many BAU/IP files). |
| `USE_TP` | auto-set by `MASK_VERSION` (`config.h`) | Compiles the TP-UART data link layer (`tpuart_data_link_layer.cpp`). |
| `USE_IP` | auto-set by `MASK_VERSION` (`config.h`) | Compiles the IP data link layer and all KNXnet/IP frame classes (`ip_data_link_layer.*`, `knx_ip_*`). |
| `USE_RF` | auto-set by `MASK_VERSION` (`config.h`) | Compiles the RF data link layer and CC1101 physical layer (`rf_data_link_layer.*`, `rf_physical_layer_cc1101.*`). |
| `USE_USB` | unset by default (`config.h:53`) | Enables the USB cEMI tunnel interface (`usb_tunnel_interface.cpp`); also forces `USE_CEMI_SERVER`. |
| `USE_CEMI_SERVER` | unset by default; auto-defined when `USE_USB` or `KNX_TUNNELING` is set (`config.h:55`) | Compiles the local cEMI server (`cemi_server.cpp`) and cEMI server object. |
| `USE_DATASECURE` | unset by default (`config.h:61`) | Enables KNX Data Secure: the secure application layer and security interface object (`secure_application_layer.cpp`, `security_interface_object.cpp`). |
| `SMALL_GROUPOBJECT` | unset by default (`config.h:66`) | GroupObjects use 8 bytes of management RAM instead of 19. |

### Tunneling

| Flag | Default | Effect |
| --- | --- | --- |
| `KNX_TUNNELING` | unset by default | Master switch for KNXnet/IP tunneling. Used both as an `#ifdef` guard (compiles `ip_tunnel_server.cpp`, tunnel handling in `bau091A.cpp`, `data_link_layer.cpp`, `ip_data_link_layer.cpp`) **and as a numeric count** of tunnel channels: `tunnels[KNX_TUNNELING + KNX_TUNNELING_DEVMGMT]` (`ip_tunnel_server.h:46`). Also pulls in `USE_CEMI_SERVER` (`config.h:55`). |
| `KNX_TUNNELING_DEVMGMT` | `1` when `KNX_TUNNELING` is set (`ip_tunnel_server.h:5-7`) | Number of Device-Management connections, handled independently of tunnel channels; adds to the tunnel array size. |
| `KNX_TUNNELING_NO_TUNNEL_PA_ON_TP` | unset by default (`data_link_layer.cpp:92`) | When set, unicast frames from a tunnel whose destination equals a tunnel PA are **not** forwarded to the TP medium (`isTunnelingPA()` check in `dataRequestFromTunnel`). Part of the "tunnel optimization" that reduces TP traffic. |
| `KNX_TUNNELING_STRICT_TOPOLOGY` | unset by default (`data_link_layer.cpp:88`) | When set, unicast frames from a tunnel whose destination is a *routed* PA (not on the secondary/TP line) are **not** forwarded to TP (`isRoutedPA()` check in `dataRequestFromTunnel`). |

> The two `KNX_TUNNELING_*` optimizations above are the non-KNX-standard "Tunnel-Optimization"
> that is disabled by default (see Changelog v1dev); they must be explicitly enabled.

### Routing

| Flag | Default | Effect |
| --- | --- | --- |
| `KNX_ROUTING_BC_DC` | unset by default (`network_layer_coupler.cpp:380`) | Changes coupler routing of locally-originated unicast: when set, the frame is sent to **both** interfaces (primary IP and secondary TP), with no-repeat on the secondary if the real destination is primary. When unset, it is sent only to the correct single interface. |

Note: there is no plain `KNX_ROUTING` on/off macro in the stack. Routing service support
is expressed via the `KNX_SERVICE_FAMILY_ROUTING` version constant (see below) and is
enabled for coupler masks (`#if MASK_VERSION == 0x091A`).

### KNXnet/IP service families

Defined in `service_families.h`; each is a service-family *version* advertised in
SEARCH/DESCRIPTION responses (used e.g. as `serviceVersion(...)`).

| Flag | Default | Effect |
| --- | --- | --- |
| `KNX_SERVICE_FAMILY_CORE` | `1` (`service_families.h:2`) | KNXnet/IP Core version. When `>= 2`, the extended search-request/response and tunneling-info DIB code is compiled (`ip_data_link_layer.cpp:127/147`, `knx_ip_search_*_extended.*`, `knx_ip_tunneling_info_dib.*`). |
| `KNX_SERVICE_FAMILY_DEVICE_MANAGEMENT` | `1` (`service_families.h:6`) | Device-Management service-family version. |
| `KNX_SERVICE_FAMILY_TUNNELING` | `1` (`service_families.h:10`) | Tunneling service-family version (advertised only when `KNX_TUNNELING` is set). |
| `KNX_SERVICE_FAMILY_ROUTING` | `1` (`service_families.h:14`) | Routing service-family version (advertised for coupler mask `0x091A`). |

### Memory / flash

| Flag | Default | Effect |
| --- | --- | --- |
| `KNX_FLASH_SIZE` | `1024` (`memory.h:13`, `platform.h:11`); `0` when `KNX_FLASH_CALLBACK` is set (`platform.h:18`) | Size in bytes of the persistent parameter/state storage. A missing value triggers a `#pragma warning`. On RP2040/SAMD it must be a multiple of the flash sector size (4096 / 1024) or the build errors out. |
| `KNX_FLASH_OFFSET` | `0x180000` (1.5 MiB) on RP2040 (`rp2040_arduino_platform.h:12`); unset elsewhere | Byte offset of the KNX storage area inside flash. On RP2040/SAMD it must be a multiple of the sector size. Missing value triggers a `#pragma warning`. |
| `KNX_FLASH_CALLBACK` | unset by default (`platform.h:9`) | Delegates flash read/size/write/commit to user-provided callbacks instead of the platform's built-in flash driver (`platform.cpp`, `platform.h`). |
| `USE_RP2040_EEPROM_EMULATION` / `USE_RP2040_LARGE_EEPROM_EMULATION` | unset by default (`rp2040_arduino_platform.h`) | RP2040-only: store parameters via EEPROM emulation instead of direct XIP flash access. The "large" variant allocates a `KNX_FLASH_SIZE` RAM buffer. |


### Platform / driver (RP2040 DMA)

| Flag | Default | Effect |
| --- | --- | --- |
| `USE_KNX_DMA_UART` | unset — `#else` branch selects `uart0` (`rp2040_arduino_platform.h:35`) | On RP2040: `== 1` routes the TP-UART DMA path to `uart1` (`UART1_IRQ`, `DREQ_UART1_RX`); otherwise `uart0`. |
| `USE_KNX_DMA_IRQ` | unset — `#else` branch selects `DMA_IRQ_0` (`rp2040_arduino_platform.h:45`) | On RP2040: `== 1` uses `DMA_IRQ_1` for the UART RX DMA; otherwise `DMA_IRQ_0`. |
| `KNX_IP_LAN` / `KNX_IP_WIFI` | unset by default (`rp2040_arduino_platform.h:21`) | RP2040 network interface selection: `KNX_IP_LAN` uses W5500 Ethernet (`KNX_NETIF = Eth`, needs arduino-pico ≥ 3.7.0); otherwise WiFi (`KNX_NETIF = WiFi`). |

### Diagnostics and footprint reduction

| Flag | Default | Effect |
| --- | --- | --- |
| `KNX_ACTIVITYCALLBACK` | unset by default (`bits.h:97`, `knx_facade.h:74/429`) | Enables an activity callback invoked on send/receive so the application can e.g. blink an activity LED. Defines the direction/type bit constants `KNX_ACTIVITYCALLBACK_DIR_*`, `_IPUNICAST`, `_NET` (`bits.h:98-102`) and is honored in the data link layers (`data_link_layer.cpp`, `ip_data_link_layer.cpp`, `tpuart_data_link_layer.cpp`). |
| `KNX_NO_AUTOMATIC_GLOBAL_INSTANCE` | unset by default (`knx_facade.h:18/485`, `knx_facade.cpp:5`) | Suppresses the predefined global `knx` object so the application can instantiate `KnxFacade<>` itself. |
| `KNX_NO_PRINT` | unset by default (`config.h:72`) | Stubs out all `print`/`println`/`printHex` (`bits.h:66-95`) — removes logging strings to save flash. |
| `KNX_NO_SPI` | unset by default (`config.h:74`) | Arduino platforms: do not use SPI (`arduino_platform.*`), e.g. when no RF transceiver is present. |
| `KNX_NO_DEFAULT_UART` | unset by default (`config.h:77`) | Arduino platforms: do not allocate the default UART; the UART must be provided via `ArduinoPlatform::knxUart`. Avoids RX/TX buffer allocation for unused serial lines. |
| `KNX_NO_STRTOx_CONVERSION` | unset by default (`config.h:70`) | Skips `KNXValue(const char*)` → numeric conversion (avoids the expensive `strtod`) in `knx_value.cpp`. |
| `NO_KNX_CONFIG` | unset by default (`config.h:3`) | Skips the whole default `config.h` body so the project can supply every define itself. |
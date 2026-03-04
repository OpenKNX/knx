# knx

This is a fork of the thelsing/knx stack from Thomas Kunze for and by the OpenKNX Team.

This projects provides a knx-device stack for arduino (ESP32 and RP2040).
It implements most of System-B specification and can be configured with ETS.
The necessary knxprod-files can be generated with the [Kaenx-Creator](https://github.com/OpenKNX/Kaenx-Creator) tool.


## Usage
See the examples for basic usage options


## Changelog

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
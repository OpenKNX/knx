# Changelog

## ec/v2.4.0-beta.1 - 2026-08-09

A large feature/robustness release on top of the 2.3.1 base, serving both coupler/router (`0x091A`) and
the new IP-interface (`0x07B0`) products: a KNX IP Interface, HW busmonitor, tunnelling reliability +
conformance, an FTC client and broad memory-safety fixes. All additions follow the KNX specification
(03_08 KNXnet/IP Core/Tunnelling/Management, 03_06 cEMI, 03_06_03 busmonitor).
**BETA:** field-tested on the OAM-IP-Interface and OAM-IP-Router (RP2040 + ESP32).
The additions are grouped below; the upstream v1dev entries follow.

### KNX IP Interface (new)
- `Bau07B0IP`: a KNXnet/IP tunnelling interface on a TP1 line (mask `0x07B0` + `KNX_TUNNELING`) -- a BAU that combines the 07B0 device layer (property/memory/group objects, so ETS can download and use KOs) with an IP data-link layer, cEMI server and tunnel server. Non-routing (never advertises ROUTING), so the HW busmonitor stays spec-conform.
- Tunnel hooks for single-interface KNXnet/IP devices (TP at entity index 0): bus-RX->tunnel, own-TX->tunnel and the `KNX_TUNNELING_NO_TUNNEL_PA_ON_TP` gate work without a coupler topology; the interface can answer a tunnel client addressed to itself (self-programming).
- `ftcPacingRate` hook for delivery-rate send pacing.

### HW busmonitor (`OPENKNX_HW_BUSMON`)
- Real hardware busmonitor over an ETS Busmonitor tunnel (NCN passive mode); raw LPDUs wrapped as cEMI `L_Busmon.ind`, runs in parallel with the link layer, no TX/ACK while monitoring.
- `bcu mon` local start/stop toggle with local/ETS dual-owner coordination; a local monitor closes data/config tunnels (exclusive, like ETS); ETS busmon takes over active data tunnels; console echo for `bcu mon`.
- L2-ACK + extended timestamp transferred (03_06_03 `L_Busmon.ind` conformance); honest status octet (lost + frame-error bits); standalone L2 acknowledges and FCS-errored frames passed through raw; `busMonitorFrame` bounded vs `MAX_LPDU`.

### Tunnelling (reliability / conformance)
- Per-tunnel FIFO for reliable server->client requests (never drops CO bursts); session history + read-only introspection; connect/disconnect history grown 10 -> 32.
- KNXnet/IP conformance hardening; dedup device-config requests + reject sub-spec datagrams; L_Data routing guarded on ChannelId (dead config-channel fallback dropped); first free unique IA assigned from the pool (not `addresses[slot]`); frame size computed only when the resend queue is enabled.
- Opt-in additional-IA defence via L2-ACK (`KNX_TUNNEL_IA_DEFENCE`).

### File-Transfer client (`OPENKNX_FTC_CLIENT`, formerly `OPENKNX_FTC` — the old name is gone, rename in your `ini` and `main.cpp`)
- Connectionless File-Transfer client role in the stack; device diagnosis (property-write, memory-read) + connection-oriented scan (reaches old BCU1/BCU2 masks); client-side `A_ADC_Read` (remote bus-voltage); responder source-PA passed to the FunctionPropertyState callback.

### IP / DESCRIPTION_RESPONSE
- IP Current Config DIB (0x04) added to `DESCRIPTION_RESPONSE` (current IP/subnet/gateway/assignment-method without a device-mgmt connection); IP-config DIB info1/info2 offsets corrected for the 0x04 layout; `PID_CURRENT_IP_ASSIGNMENT_METHOD` / `PID_IP_CAPABILITIES` allow 1 element.

### Robustness / memory-safety
- `CemiFrame::valid()` OOB guard; truncated `M_PropRead`/`M_PropWrite` dropped before dereference; inbound routing cEMI validated before forwarding to TP; negative `L_Data.con` on a send-limit drop; all expired tunnel slots reaped (not just the first) + dangling `addresses` pointer fixed; LC-config property pointer guarded (not the always-non-zero default); TpUart `sendFrame` guarded against malloc failure; queue-full log rate-limited (no watchdog-starving print flood).
- Property reads, memory writes and DPT encodes bounded against OOB; FunctionProperty(Ext)/Memory response indications guarded against short-frame over-read; full 254-octet APDU allowed (NPDU length `uint16` + FTC send guard 251); cEMI frame buffer sized for a full 255-byte APDU; real `M_PropWrite` failure code reported; `PropertyValue` read counts elements in the high nibble.

### Transport / performance
- Originator `T_Connect` sets `_connectedTsap` to the peer PA (not 0); IP->TP frames serialized on the stack (removes a per-frame malloc/free on the hot path); dead cEMI copy on the IP tunnel fan-out dropped; `CemiFrame` taken by const reference in the IP encoders; malloc'd cEMI RX buffer `free()`d instead of `delete`d.

### Docs
- README: index, architecture overview + build-flags table.

### Upstream base (v1dev)

- Pin TPUart to ec/v1.2.0-beta.1
- Fix: memory leak of `TPUart::Frame` on discarded TP frames. The frame was not deleted when the transmit queue was full or when the BCU was not connected / in busmonitor mode
- Fix: memory leak in the cEMI server on a negative `M_PropRead` response. The buffer allocated by `propertyValueRead()` was only freed on the positive path, so reading an unknown PID (e.g. during an ETS property scan) leaked it
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

## v2.3.1 - 2026-03-04

- Hotfix: DPT16 was not correctly handled for uninitialized KOs

## v2.3.0 - 2026-01-28
- Fix Define for 'DPT_FlowRate_m3/h'
- Enhance multicast initialization logging
- Allow write to hidden KO for DPT of size > 1 Byte
- Add function paramString to access string parameters
- Update TPUart dependency to version 1.0.4


## v2.2.2 - 2025-10-21
- Fix: DPT subgroup 0 handling

## v2.2.1 - 2025-08-22
- Fix: Distinguish between tunnel and TP PAs when reading PID_ADDITIONAL_INDIVIDUAL_ADDRESSES. This resulted in a failed PA assignment for 0x091A devices with KNX_TUNNELING
- Fix: set repeat correctly in DataLinkLayer when sending to other mediums as TP (https://github.com/OpenKNX/knx/issues/40)
- Fix: `dataConReceived` is not suppressed anymore. This prevented sending device reset telegrams.
- Fix: Update TPUart lib to 1.0.2
- Fix: Add individual address handling in TpUartDataLinkLayer
- Fix: [Unload application was not permanent](https://github.com/thelsing/knx/issues/144)
- Extend documentation for KO-state

## V2.2.0 - 2025-07-04
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

## V2.1.2 - 2024-12-09
- adds unicast auto ack

## V2.1.1 - 2024-09-16
- fix minor bug in TP-Uart Driver (RX queue out of boundary)

## V2.1.0 - 2024-07-03
- complete rework of the TPUart DataLinkLayer with support interrupt-based handling and optimized queue handling
- added DMA support for RP2040 platform
- fix some issues with continous integration causing github actions to fail
- added rp2040 plattform to knx-demo example
- added bool GroupObject::valueCompare method for only sending the value when it has changed 

## V2.0.0 - 2024-02-13
- first OpenKNX version

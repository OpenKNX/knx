# Changelog


## unreleased

Memory-safety and conformance work on the management path, plus the removal of the download counter.
Every bound below was re-derived from the response builder and the frame buffer, not from the handler
that reads the data. Built on OAM-IP-Interface (RP2040, RP2350, ESP32), OAM-IP-Router (RP2040, ESP32)
and OAM-RaumController (57B0).

### Reachable over the bus
* Fix: a property that exists but is not `PDT_FUNCTION` is answered without return code and without data (03_03_07 3.4.7.3). `functionPropertyStateIndication` started with `handled = true`, so such a request was answered with `resultLength` still at its 255 initialiser: `CemiFrame(3 + 255)` truncated through the `uint8_t` parameter to `apduLength` 2 while `memcpy` wrote 255 octets from `buffer+13`, four octets past the 264-octet buffer and into the frame's own data pointer, which `sendTelegram` writes through on its next statement. Reachable with `A_FunctionPropertyState_Read` on object 0 property 1, over TP and through a tunnel, without programming mode and without authentication
* Fix: `functionPropertyExtStateIndication` starts `resultLength` at 1, because its error paths write only `resultData[0]` and 03_03_07 3.4.8.3 says such a response carries no data field
* Fix: the response builders are bound to the frame buffer -- `propertyValueRead` 249, `propertyValueExtRead` 245, `functionPropertyStateResponse` 251, its extended twin 248, and `systemNetworkParameterReadResponse` 250, which had no bound at all: a 250-octet `test_info` in programming mode wrapped `frame(260)` to `frame(4)` and overran by six octets
* Fix: `CemiFrame(apduLength)` takes `uint16_t`; a request above 255 wrapped to a short frame that the caller then filled to its own length. An oversized request now leaves `octetCount` at 0 and `sendTelegram` drops it before touching any field
* Fix: busmonitor carriers stay out of the cEMI path on every build -- a standalone acknowledge, a raw poll frame and a truncated frame are not full LPDUs, and the rejection sat inside the busmonitor build gate, so a build without it converted them and read past the buffer

### Group objects
* Fix: the association-table lookup terminates. The binary search used a closed interval, so `high = i - 1` wrapped to `0xFFFF` whenever the searched ASAP was below the table's first entry and the loop never ended -- a watchdog boot loop, not repairable over the bus, triggered by an ordinary project with group object 1 unassigned and group object 2 assigned
* Fix: `entryCount()` no longer believes an erased segment, which reported `0xFFFF` entries and read up to 262 KiB past the table
* Fix: the unsorted path no longer re-arms the binary search it just rejected, which made a group object silently never send
* Fix: an unassigned ASAP is dropped instead of sent -- `groupValueSend` cast `translateAsap`'s `-1` to `0xFFFF`, which the address table maps to 0, so a group value write went out as a broadcast for every group object ETS left without a group address
* Fix: the confirm is attributed to the right object: the ASAP slot is claimed once the telegram is going out, and cleared when nothing goes out

### Datapoint types
* Fix: negative values encode on the signed types. `signed8/16/32ToPayload` were fed `(uint64_t)value`, and for a `DoubleType` that is a cast of a negative double to an unsigned integer, which is undefined and saturates to zero on ARM -- a negative float on a DPT 6, 8 or 13 group object went on the bus as `0x00`. RP2040 and RP2350 were affected, the ESP32 arrived at the right value by accident

### Device management
* Fix: the ETS writability probe is answered. ETS probes a property with a 7-byte `M_PropWrite` request carrying no data before it writes; the branch handling `PID_DEVICE_ADDR` and `PID_SUBNET_ADDR` required a data octet, so the probe fell through to the generic branch, where both properties are declared write-protected and the answer was `Read_Only`. ETS reported the individual-address download as a memory write failure
* Change: `PID_DOWNLOAD_COUNTER` is removed. No KNX tool reads it, and on every product but the IP interface it reported 0 after each restart because the value lived in RAM and an ETS download ends in a restart -- a counter that decreases is worse than none, and 03_05_01 5.3.2.2 defines the fallback to a full download when the property is unavailable. The implementation also incremented a `uint16_t` without a ceiling, which 03_05_01 4.2.30.2 forbids. 03_05_01 4.2.30.1 makes the property conditional, so a device without a download counter is conformant. Removing it changes the persisted stream by zero octets
* Fix: `Frame::cemiData()` can fail its allocation, and both callers check the result

### Tunnelling
* Fix: a connect that is turned away is recorded. Three of the four reject paths wrote a history entry; the fourth -- no slot free, or a reserved slot busy and configured to decline -- sent the error response and left no trace. `detail` now carries the code the client received, 0x24 for no free connection and 0x25 for no unique individual address. Repeated identical refusals still fold into one entry
* Feature: `TunnelEvent` reports `slot` and `resSlot`, and the connection stores the slot the reservation table held for its client at connect time. Recomputing it afterwards cannot work: the reservation is matched against the control HPAI while `IpAddress` is the data HPAI, and the two need not carry the same address. Device-management and busmonitor connections report 0xFF, they sit outside the reservable pool
* Feature: `reservedTunnelsCtrl()` and `reservedTunnelsIp()` read back the ETS reservation table for diagnostics, with the same length check the connect path uses. The returned memory belongs to the property and is freed on the next ETS write, so both are for the KNX loop only


## ec/v2.5.0-beta.1 - 2026-08-29

Conformance and hardening release on top of `ec/v2.4.0-beta.1` (`cec1b35` .. `eef0ade`). Most entries
come from tracing the KNXnet/IP and device-management paths against the specification with a test
client, so nearly every fix names a value or timing an ETS or a tunnel client actually observes.
Field-tested on OAM-IP-Interface and OAM-IP-Router (RP2040 + ESP32).

### Breaking
* Breaking: `OPENKNX_FTC` is now `OPENKNX_FTC_CLIENT`. A product that sets the old name loses the client role silently, so rename it in the ini
* Change: the FunctionProperty confirm cases are handled unconditionally, no longer only under the FTC flag

### Discovery and description (a non-router must not look like a router)
* Fix: ROUTING is advertised only on a router — `DESCRIPTION_RESPONSE`, `SEARCH_RESPONSE` and `SEARCH_RESPONSE_EXTENDED` announced the routing service family and the routing multicast on an interface as well, which is what makes an interface fail the HW-busmonitor rules of 03_08_04 §2.2.4
* Fix: a non-routing device reads routing multicast as `0` and still joins the discovery multicast, so it stays findable without claiming routing
* Fix: the extended search no longer advertises the TP1 medium as permanently unavailable
* Fix: `SEARCH_REQUEST`/`SEARCH_REQUEST_EXTENDED` carrying a TCP HPAI is discarded instead of answered over UDP
* Fix: the extended-search SRP parser is bound-checked and the `requestedDIB` read guard corrected from `>` to `>=`
* Fix: the IP Current Config DIB (`0x04`) is part of `DESCRIPTION_RESPONSE`, with corrected info1/info2 offsets
* Fix: the KNX-Addresses DIB writes the individual address as 2 octets
* Fix: `PID_CURRENT_IP_ASSIGNMENT_METHOD` and `PID_IP_CAPABILITIES` accept 1 element
* Fix: the `setTunnelingInfo` address buffer is hoisted out of the `else` block, where it went out of scope before use

### Tunnelling
* Fix: the interface no longer L2-acknowledges foreign group telegrams on behalf of a tunnel client -- `isSentToTunnel()` reports true for EVERY group address while any tunnel is open, so `Bau07B0IP::isAckRequired` acknowledged group telegrams the device is not addressed by, and the TP1 repetition a receiver that missed the frame depends on never happened (the scene telegrams that prompted this were later traced to a REG2 powered from the KNX supply, not to the acknowledge; the acknowledge is wrong on its own terms)
* Fix: the interface acknowledges group telegrams only for broadcast and its own address table; acknowledging on behalf is coupler behaviour and stays in `Bau091A`
* Fix: `L_Data.con` carries the real TP result and is emitted once per request — a client could see a positive confirmation for a frame the bus never took
* Fix: a self-addressed tunnel probe (`src == dest`) gets its `L_Data.con`
* Fix: a truncated TUNNEL `CONNECT_REQUEST` is rejected with `E_CONNECTION_TYPE` instead of being parsed
* Fix: requests on an unopened channel are rejected, and a connection error response echoes the requested channel id
* Fix: `CONNECTIONSTATE_RESPONSE`, `DISCONNECT_RESPONSE` and `CONNECT_RESPONSE` rejects go back to the stored or resolved control endpoint, not to wherever the last datagram came from
* Fix: NAT route-back for a `0.0.0.0` data HPAI, per field rather than for the whole endpoint
* Fix: the config channel resends at 10 s / 3 attempts, data stays at 1 s / 1 attempt (03_08_04 H-4.2.11); both used the data timing before
* Fix: the connectionstate heartbeat reports `E_KNX_CONNECTION` while the bus is down instead of claiming a healthy link
* Fix: the session is recorded before a reserved-slot takeover resets it, so the history no longer loses the entry
* Fix: the KNXnet/IP header total length is validated before anything is read from it
* Change: drop the dead `IpTunnelServer::dataRequestToTunnel` (no caller; requests run through `dataRequestToChannelId`) and note the missing Extended-CRI (tunnelling v2 requested-IA) path

### cEMI and device management
* Feature: local Transport Layer over cEMI — `T_Data_Individual` and `T_Data_Connected` message codes `0x4A`/`0x41` are served instead of dropped (AN118, 03_08_04 H-4.3.5/H-4.3.7)
* Feature: `PID_DOWNLOAD_COUNTER` (30) on the Device Object — read-only change token, +1 per download session (armed by a read, spent by the first table load), kept in RAM so the NVM layout and apiVersion are unchanged; a product may persist it in one of its OpenKNX modules
* Fix: `M_PropRead` skips the client-address patch on a `start_index == 0` count read, which returned a patched value where the count belongs
* Fix: `M_PropWrite` guards its address patch against a request with no data
* Fix: `M_PropRead`/`M_PropWrite` frames that are truncated are dropped instead of parsed
* Fix: `M_PropWrite` reports the real failure code
* Fix: `A_PropertyExtDescription_Read` parses the 8-octet APDU correctly, and the response returns the resolved PID and index
* Fix: property writes are bounded against the received payload length; property value reads count elements in the high nibble (`|=` instead of `&=`)
* Fix: the FunctionProperty handlers null-check the property and drop a zero-length PDU
* Fix: the `A_Restart` master-reset erase codes now actually erase instead of answering `0x00` and doing nothing — ResetLinks/ResetAP/ResetParam/ResetIA and both factory resets clear the matching tables, individual address and IP configuration via the tested unload path (03_05_02 3.7.1.2)

### Management APDU bounds
* Fix: `A_MemoryExtended_Read` clamps its response length to the CemiFrame buffer — the previous version could write past it
* Fix: management memory reads are bounded against the NVM size
* Fix: short APDUs are rejected before any length-derived read in the management handlers, including `A_Restart` MasterReset
* Fix: a zero-octet group APDU is dropped before the length underflow
* Fix: `LE_ADDITIONAL_LOAD_CONTROLS` reads are bounded by the payload length, and a short write is dropped before the out-of-bounds read
* Fix: the full variable-length `test_info` is forwarded and the system-broadcast reads are guarded
* Fix: the group-object ASAP is bounded before `GroupObjectTableObject::get()`, and `AssociationTableObject::entryCount()` is guarded until the table is loaded
* Fix: `telegramLengthtTP` subtracts the cEMI additional-info length

### Transport layer
* Fix: an undefined transport control PDU is rejected instead of acted on
* Fix: in the `CONNECTING` state an E20 event closes and then sends A5, per 03_03_04 style 3

### Busmonitor
* Fix: the status octet carries bit-error, truncated and lost, and an rx-frame-buffer overflow counts into the lost-frame status
* Fix: the busmon channel id stays unique against the data channels and the `L_Busmon.ind` sequence is reset on connect
* Fix: refused tunnel connects are recorded
* Fix: busmon-only carrier handling is gated under `OPENKNX_HW_BUSMON`

### Coupler
* Fix: hop count 7 is decremented on closed-media routing (post-AN189)
* Fix: `functionRouteTableControl` is guarded against a short PDU
* Change: the stray system-broadcast `println` is compile-guarded with `KNX_LOG_COUPLER`
* Feature: KNXnet/IP telegram counters (`PID_QUEUE_OVERFLOW_TO_IP`/`_TO_KNX` 72/73, `PID_MSG_TRANSMIT_TO_IP`/`_TO_KNX` 74/75, 03_08_03 2.5.23-2.5.26) on a routing device — saturating, counted on the send paths (every emitted datagram incl. ACKs), read-only and only under `KNX_IS_ROUTER`, so the interface advertises none

### Datapoint types
* Fix: DPT 6 decodes as signed (V8)
* Fix: DPT 225/239 scaling decode rounds the same way the encoder does, so encode-decode round-trips
* Fix: the DPT Locale decode no longer returns a dangling pointer and NUL-terminates
* Fix: DPT 231 (Locale) has a `dataLength()` entry
* Fix: the value accessors read the matching `KNXValue` union member

### Diagnostics and portability
* Feature: `OPENKNX_CON_DIAG` counts confirmation generation on the tunnel path (indications, acks, header drops); off by default
* Feature: `KNX_LOG_TUNNELING` names the failing check on an invalid frame and prints property errors in readable form
* Feature: device type, role and capabilities are decoded once per `MASK_VERSION` instead of at each call site
* Fix: `0x07B0` plus `KNX_TUNNELING` is compile-guarded on SAMD and STM32, which have no IP platform
* Fix: the 711/713 unhandled-APDU confirm log is suppressed on FTC console-only builds
* Doc: the unimplemented requester primitives and the KNX-Secure confirm are annotated in the source
* Change: TPUart dependency pinned to `ec/1.2.0-beta.1`

### KNXnet/IP telegram counters (03_08_03 2.5.23-2.5.26)
* Feature: the four counters exist for the first time — `PID_QUEUE_OVERFLOW_TO_IP/KNX` (72/73) and `PID_MSG_TRANSMIT_TO_IP/KNX` (74/75) were enum values no `.cpp` ever used, so a routing device could not answer them at all
* Feature: `->IP` counts every KNXnet/IP datagram the stack emits — tunnelling, core, device management, ACKs — as 2.5.25 requires, not just routed group telegrams; every `IpTunnelServer` send goes through one helper, so no path escapes the count
* Feature: `->KNX` counts frames the TP link accepted; a rejected transmit queue counts as a loss towards KNX, the IP send limit as a loss towards IP
* Feature: `routedToIp`/`routedToKnx` and `filteredToIp`/`filteredToKnx` record the coupler's routing decision per direction — not in the spec, for the console and the display
* Change: the four spec counters saturate instead of wrapping, as 2.5.23 demands
* Change: the properties are gated on `KNX_IS_ROUTER`, so the parameter object of a tunnelling-only device stays unchanged
* Fix: the saturating increment no longer uses `v++` on a `volatile`, which C++20 deprecates
* Fix: the counter header is included next to the other includes instead of inside `#ifdef ARDUINO_ARCH_RP2040` -- an ESP32 target without `KNX_TUNNELING`, so a plain TP device, lost the declaration while the members stayed and did not compile

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

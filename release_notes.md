# Release Notes

## 2.3.1
### Breaking Changes
- none

### Feature
- none

### Bug
- Hotfix: DPT16 was not correctly handled for uninitialized KOs.

## 2.3.0
### Breaking Changes
- none

### Feature
- Enhance multicast initialization logging.
- Allow write to hidden KO for DPT of size greater than 1 byte.
- Add function paramString to access string parameters.
- Update TPUart dependency to version 1.0.4.

### Bug
- Fix define for DPT_FlowRate_m3/h.

## 2.2.2
### Breaking Changes
- none

### Feature
- none

### Bug
- Fix DPT subgroup 0 handling.

## 2.2.1
### Breaking Changes
- none

### Feature
- Extend documentation for KO state.

### Bug
- Distinguish between tunnel and TP PAs when reading PID_ADDITIONAL_INDIVIDUAL_ADDRESSES.
- Set repeat correctly in DataLinkLayer when sending to media other than TP.
- dataConReceived is not suppressed anymore.
- Update TPUart lib to 1.0.2.
- Add individual address handling in TpUartDataLinkLayer.
- Unload application was not permanent.

## 2.2.0
### Breaking Changes
- Increase device object API version to 2 (invalidates older stored KNX flash data).

### Feature
- Return successful conversion to DPT on value update operations in GroupObject.
- Strings are now null terminated in group objects.
- Change defines in RP2040 platform for LAN/WLAN usage to KNX_IP_LAN or KNX_IP_WIFI.
- Better routing and tunneling support.
- Add DPT 27.001.
- Add pragma once for Arduino platform to allow derived platforms.
- Change ESP32 platform to use KNX_NETIF.
- Remove examples for deprecated platforms and update remaining examples.
- Use TPUart library from OpenKNX.

### Bug
- Fix unexpected GroupObject behavior on failed conversion to DPT.
- Only set pinMode of PROG button pin if valid.

## 2.1.2
### Breaking Changes
- none

### Feature
- Add unicast auto ACK.

### Bug
- none

## 2.1.1
### Breaking Changes
- none

### Feature
- none

### Bug
- Fix minor bug in TP-Uart driver (RX queue out of boundary).

## 2.1.0
### Breaking Changes
- none

### Feature
- Complete rework of TPUart DataLinkLayer with interrupt-based handling and optimized queues.
- Add DMA support for RP2040 platform.
- Add RP2040 platform to knx-demo example.
- Add bool GroupObject::valueCompare for sending only when value changed.

### Bug
- Fix issues in continuous integration causing GitHub actions to fail.

## 2.0.0
### Breaking Changes
- none

### Feature
- First OpenKNX version.

### Bug
- none

## 1.0.0
### Breaking Changes
- Change default PID_MAX_APDU_LENGTH_ROUTER from 220 to 254.

### Feature
- Deactivate by default not KNX-standard compatible tunnel optimization.
- Refactor KNX IP tunneling.
- Device Management Connection is now handled independently from tunnel connections and does not consume a tunnel PA anymore.
- New compiler option KNX_TUNNELING_DEVMGMT (default 1) to set number of available Device Management Connections.
- New compiler option KNX_ROUTING_BC_DC for unicast packets from the device itself to both interfaces.
- Add GroupObject::valueCompareTime() to send only when value changed or after timeout.

### Bug
- Fix broken ConfigReq responses.
- Fix programming application when FlashTablesInvalid for 0x091A.

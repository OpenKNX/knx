#pragma once

#include "config.h"
#ifdef USE_CEMI_SERVER

#include <stdint.h>
#include "knx_types.h"
#include "usb_tunnel_interface.h"

class BauSystemB;
class DataLinkLayer;
class CemiFrame;
class IpTunnelServer;
class TransportLayer;

/**
 * This is an implementation of the cEMI server as specified in @cite knx:3/6/3.
 * Overview on page 57.
 * It provides methods for the BusAccessUnit to do different things and translates this 
 * call to an cEMI frame and calls the correct method of the data link layer. 
 * It also takes calls from data link layer, decodes the submitted cEMI frames and calls the corresponding
 * methods of the BusAccessUnit class.
 */
class CemiServer
{
  public:
    /**
     * The constructor.
     * @param bau methods are called here depending of the content of the APDU
     */
#ifndef KNX_TUNNELING
    CemiServer(BauSystemB& bau);
#else
    CemiServer(BauSystemB& bau, IpTunnelServer& ipTunnelServer);
#endif

    void dataLinkLayer(DataLinkLayer& layer);
    DataLinkLayer* dataLinkLayer() const { return _dataLinkLayer; }
#ifdef KNX_TUNNELING
    void dataLinkLayerPrimary(DataLinkLayer& layer);
#endif
#ifdef KNX_CEMI_TRANSPORT_LAYER
    // Wire the device transport layer for the local T_Data_Individual/Connected services (03_06_03 §4.1.6).
    void transportLayer(TransportLayer& layer);
    // Track whether a device management connection is open (03_08_03 2.6.1.2 p.18). Idempotent.
    void cemiTransportMode(bool active);
    /** @brief True while the device transport layer already carries a connection. */
    bool transportLayerBusy() const;
#endif

    // from data link layer
    // Only L_Data service
    void dataIndicationToTunnel(CemiFrame& frame);
    void dataConfirmationToTunnel(CemiFrame& frame);

    // From tunnel interface
    void frameReceived(CemiFrame& frame, uint8_t channelId);

    uint16_t clientAddress() const;
    void clientAddress(uint16_t value);

#ifdef KNX_TUNNELING
    /** @brief Report an evented property with M_PropInfo.ind on every device management connection.
     *  03_08_03 4.2.5 p.23; frame layout per 08_TSSH 4.2.12 p.41. Reads element 1, count 1. */
    void propertyInfoIndication(uint16_t objectType, uint8_t objectInstance, uint8_t propertyId,
                                const uint8_t* data, uint8_t length);
#endif

    void loop();
    
  private:
    uint16_t _clientAddress = 0;
    uint8_t _frameNumber = 0;

    void handleLData(CemiFrame& frame);
    void handleMPropRead(CemiFrame& frame, uint8_t channelId);
    void handleMPropWrite(CemiFrame& frame, uint8_t channelId);
    void handleMReset(CemiFrame& frame, uint8_t channelId);
#ifdef KNX_CEMI_TRANSPORT_LAYER
    void handleLocalTransport(CemiFrame& frame, uint8_t channelId, bool connected);
#endif

    DataLinkLayer* _dataLinkLayer = nullptr;
#ifdef KNX_TUNNELING
    DataLinkLayer* _dataLinkLayerPrimary = nullptr;
    IpTunnelServer& _ipTunnelServer;
#endif
#ifdef KNX_CEMI_TRANSPORT_LAYER
    TransportLayer* _transportLayer = nullptr;
#endif
    BauSystemB& _bau;
#ifdef USE_USB
    UsbTunnelInterface _usbTunnelInterface;
#endif
};

#endif
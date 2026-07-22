#pragma once

#include "config.h"
// A 0x07B0 device that additionally offers a KNXnet/IP Tunnelling front-end (an "IP Interface"
// with its own group objects). Selected instead of the plain Bau07B0 when KNX_TUNNELING is set.
// Not a KNXnet/IP Routing device: it never advertises the ROUTING service family (mask != 0x091A)
// and never forwards group telegrams onto IP -- group communication stays on TP.
#if MASK_VERSION == 0x07B0 && defined(KNX_TUNNELING)

#include "bau_systemB_device.h"
#include "ip_parameter_object.h"
#include "ip_data_link_layer.h"
#include "tpuart_data_link_layer.h"
#include "cemi_server.h"
#include "cemi_server_object.h"
#include "ip_tunnel_server.h"

class Bau07B0IP : public BauSystemBDevice, public ITpUartCallBacks, public DataLinkLayerCallbacks
{
  public:
    Bau07B0IP(Platform& platform);
    void loop() override;
    bool enabled() override;
    void enabled(bool value) override;

    // TP is the bus link (group comm); IP is present only for discovery + tunnelling.
    // getDataLinkLayer() is the device-mask accessor (OGM-Common 0x07B0 paths); the Primary/Secondary
    // pair mirrors the coupler naming so router-derived code (main loop1, NetworkModule) links unchanged.
    TpUartDataLinkLayer* getDataLinkLayer();
    TpUartDataLinkLayer* getSecondaryDataLinkLayer();
    IpDataLinkLayer* getPrimaryDataLinkLayer();
#ifdef OPENKNX_FTC
    // FTC fast-transfer flow control: read the TP transmit FIFO depth off the TP link.
    uint16_t ftcTxQueueSize() override;
#endif
  protected:
    InterfaceObject* getInterfaceObject(uint8_t idx);
    InterfaceObject* getInterfaceObject(ObjectType objectType, uint16_t objectInstance);

    // For TP1 only
    TPAckType isAckRequired(uint16_t address, bool isGrpAddr) override;

    void doMasterReset(EraseCode eraseCode, uint8_t channel) override;

  private:
    IpParameterObject _ipParameters;
    TpUartDataLinkLayer _tpLayer;  // bus link (index 0) -- carries the device's group objects
    IpDataLinkLayer _ipLayer;      // KNXnet/IP endpoint: discovery + tunnel pump, no routing
    CemiServer _cemiServer;
    CemiServerObject _cemiServerObject;
    IpTunnelServer _ipTunnelServer;
};
#endif

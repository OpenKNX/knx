#pragma once

#include "config.h"
#if MASK_VERSION == 0x091A

#include "bau_systemB_coupler.h"
#include "router_object.h"
#include "ip_parameter_object.h"
#include "ip_data_link_layer.h"
#include "tpuart_data_link_layer.h"
#include "cemi_server_object.h"
#include "ip_tunnel_server.h"

class Bau091A : public BauSystemBCoupler, public ITpUartCallBacks, public DataLinkLayerCallbacks
{
  public:
    Bau091A(Platform& platform);
    void loop() override;
    bool enabled() override;
    void enabled(bool value) override;
    bool configured() override;

    IpDataLinkLayer* getPrimaryDataLinkLayer();
    TpUartDataLinkLayer* getSecondaryDataLinkLayer();
#ifdef OPENKNX_FTC
    // FTC fast-transfer flow control: read the TP transmit FIFO depth off the secondary (TP) link.
    uint16_t ftcTxQueueSize() override;
#endif
  protected:
    InterfaceObject* getInterfaceObject(uint8_t idx);
    InterfaceObject* getInterfaceObject(ObjectType objectType, uint16_t objectInstance);

    // For TP1 only
    TPAckType isAckRequired(uint16_t address, bool isGrpAddr) override;

    void doMasterReset(EraseCode eraseCode, uint8_t channel) override;
  private:
    RouterObject _routerObj;
    IpParameterObject _ipParameters;
    IpDataLinkLayer _dlLayerPrimary;
    TpUartDataLinkLayer _dlLayerSecondary;
#ifdef USE_CEMI_SERVER
    CemiServer _cemiServer;
    CemiServerObject _cemiServerObject;
#endif
#ifdef KNX_TUNNELING
    IpTunnelServer _ipTunnelServer;
#endif
};
#endif

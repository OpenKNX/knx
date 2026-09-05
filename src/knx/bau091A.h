#pragma once

#include "config.h"
#if MASK_VERSION == 0x091A

#include "bau_systemB_coupler.h"
#include "router_object.h"
#include "ip_parameter_object.h"
#include "knx_ip_counters.h"
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

    /** @brief KNXnet/IP telegram counters (03_08_03) for diagnostics and the OAM console. */
    KnxIpCounters& getCounters() { return _counters; }

    /** @brief Router object, for reading the filter table in diagnostics. */
    RouterObject& getRouterObject() { return _routerObj; }

#ifdef OPENKNX_ROUTE_TRACE
    /** @brief Routing decisions of the coupler network layer. */
    RouteTrace& getRouteTrace() { return _netLayer.routeTrace(); }
#endif

    IpDataLinkLayer* getPrimaryDataLinkLayer();
    TpUartDataLinkLayer* getSecondaryDataLinkLayer();

#ifdef KNX_TUNNELING
    IpTunnelServer& getIpTunnelServer() { return _ipTunnelServer; }
#endif
#ifdef OPENKNX_FTC_CLIENT
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
    KnxIpCounters _counters; // declared first: _ipParameters takes its address
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

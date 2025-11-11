#pragma once

#include "config.h"
#ifdef USE_IP

#include <stdint.h>
#include "data_link_layer.h"
#include "ip_parameter_object.h"
#include "service_families.h"
#include "ip_tunnel_server.h"

class IpDataLinkLayer : public DataLinkLayer
{
    using DataLinkLayer::_deviceObject;

  public:
    IpDataLinkLayer(DeviceObject& devObj, IpParameterObject& ipParam,
                    NetworkLayerEntity& netLayerEntity,
                    Platform& platform, BusAccessUnit& busAccessUnit,
#ifdef KNX_TUNNELING
                    IpTunnelServer& ipTunnelServer,
#endif
                    DataLinkLayerCallbacks* dllcb = nullptr);

    void loop();
    void enabled(bool value);
    bool enabled() const;
    DptMedium mediumType() const override;


  private:
    bool _enabled = false;
    uint8_t _frameCount[10] = {0,0,0,0,0,0,0,0,0,0};
    uint8_t _frameCountBase = 0;
    uint32_t _frameCountTimeBase = 0;
    bool sendFrame(CemiFrame& frame);

#if KNX_SERVICE_FAMILY_CORE >= 2
    void loopHandleSearchRequestExtended(uint8_t* buffer, uint16_t length);
#endif
    bool sendBytes(uint8_t* buffer, uint16_t length);
    bool isSendLimitReached();

    IpParameterObject& _ipParameters;
    DataLinkLayerCallbacks* _dllcb;
};
#endif
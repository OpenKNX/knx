#pragma once

#include <stdint.h>
#include "knx_types.h"
#include "npdu.h"
#include "transport_layer.h"
#include "network_layer_entity.h"

class DeviceObject;
class APDU;

// Hop count of an ANSWER: the sender's own parameter, never the received 7 (03_03_03 2.4.1 p.11;
// 2.4.2.4.2 p.13 for a coupler answering for itself; tested by 08_03_03 3.1-3.4 pp.5-9). Receive side
// only - the coupler's forwarding path still decrements. KNX_ECHO_UNLIMITED_ROUTING restores the echo.
#ifdef KNX_ECHO_UNLIMITED_ROUTING
    #define KNX_ANSWER_HOPTYPE(npdu) ((npdu).hopCount() == 7 ? UnlimitedRouting : NetworkLayerParameter)
#else
    #define KNX_ANSWER_HOPTYPE(npdu) (NetworkLayerParameter)
#endif

class NetworkLayer
{
    friend class NetworkLayerEntity;

  public:
    NetworkLayer(DeviceObject& deviceObj, TransportLayer& layer);

    uint8_t hopCount() const;
    bool isApciSystemBroadcast(APDU& apdu);

    // from transport layer
    virtual void dataIndividualRequest(AckType ack, uint16_t destination, HopCountType hopType, Priority priority, TPDU& tpdu) = 0;
    virtual void dataGroupRequest(AckType ack, uint16_t destination, HopCountType hopType, Priority priority, TPDU& tpdu) = 0;
    virtual void dataBroadcastRequest(AckType ack, HopCountType hopType, Priority priority, TPDU& tpdu) = 0;
    virtual void dataSystemBroadcastRequest(AckType ack, HopCountType hopType, Priority priority, TPDU& tpdu) = 0;

  protected:
    DeviceObject& _deviceObj;
    TransportLayer& _transportLayer;

    // from entities
    virtual void dataIndication(AckType ack, AddressType addType, uint16_t destination, FrameFormat format, NPDU& npdu,
                                Priority priority, uint16_t source, uint8_t srcIfIdx) = 0;
    virtual void dataConfirm(AckType ack, AddressType addressType, uint16_t destination, FrameFormat format, Priority priority,
                             uint16_t source, NPDU& npdu, bool status, uint8_t srcIfIdx) = 0;
    virtual void broadcastIndication(AckType ack, FrameFormat format, NPDU& npdu,
                                     Priority priority, uint16_t source, uint8_t srcIfIdx) = 0;
    virtual void broadcastConfirm(AckType ack, FrameFormat format, Priority priority, uint16_t source, NPDU& npdu, bool status, uint8_t srcIfIdx) = 0;
    virtual void systemBroadcastIndication(AckType ack, FrameFormat format, NPDU& npdu,
                                           Priority priority, uint16_t source, uint8_t srcIfIdx) = 0;
    virtual void systemBroadcastConfirm(AckType ack, FrameFormat format, Priority priority, uint16_t source, NPDU& npdu, bool status, uint8_t srcIfIdx) = 0;

  private:
};

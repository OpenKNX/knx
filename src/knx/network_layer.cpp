#include "network_layer.h"
#include "device_object.h"
#include "data_link_layer.h"
#include "tpdu.h"
#include "cemi_frame.h"
#include "bits.h"
#include "apdu.h"

NetworkLayer::NetworkLayer(DeviceObject &deviceObj, TransportLayer& layer) :
    _deviceObj(deviceObj),
    _transportLayer(layer)
{
}

uint8_t NetworkLayer::hopCount() const
{
    // Read where it is used, not latched in the constructor: that runs before readMemory() restores
    // PID_ROUTING_COUNT and before ETS can write it.
    return _deviceObj.defaultHopCount();
}

bool NetworkLayer::isApciSystemBroadcast(APDU& apdu)
{
    switch (apdu.type())
    {
        // Application Layer Services on System Broadcast communication mode
        case SystemNetworkParameterRead:
        case SystemNetworkParameterResponse:
        case SystemNetworkParameterWrite:
        // Open media specific Application Layer Services on System Broadcast communication mode
        case DomainAddressSerialNumberRead:
        case DomainAddressSerialNumberResponse:
        case DomainAddressSerialNumberWrite:
        case DomainAddressRead:
        case DomainAddressSelectiveRead:
        case DomainAddressResponse:
        case DomainAddressWrite:
            return true;
        default:
            return false;
    }
    return false;
}


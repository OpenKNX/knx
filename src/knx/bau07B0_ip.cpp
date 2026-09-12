#include "config.h"
#if MASK_VERSION == 0x07B0 && defined(KNX_TUNNELING)

#include "bau07B0_ip.h"
#include "bits.h"
#include <string.h>
#include <stdio.h>

using namespace std;

// Both data link layers bind the device's single network interface (index 0). Only the TP layer is
// registered as that interface's active DLL, so the network layer sends group/individual telegrams to
// TP only. The IP layer runs purely as the KNXnet/IP endpoint (discovery + tunnel receive pump) and
// has its inbound RoutingIndication handling disabled -> no group communication over IP, no routing.
Bau07B0IP::Bau07B0IP(Platform& platform)
    : BauSystemBDevice(platform),
      _ipParameters(_deviceObj, platform, &_counters),
      _tpLayer(_deviceObj, _netLayer.getInterface(), platform, *this, _ipTunnelServer, (ITpUartCallBacks&) *this, (DataLinkLayerCallbacks*) this),
      _ipLayer(_deviceObj, _ipParameters, _netLayer.getInterface(), _platform, *this, _ipTunnelServer, (DataLinkLayerCallbacks*) this),
      DataLinkLayerCallbacks(),
      _cemiServer(*this, _ipTunnelServer),
      _ipTunnelServer(_deviceObj, _ipParameters, platform, _cemiServer)
{
    // Same counters as the router keeps; the interface uses them for the console/display only.
    _ipLayer.setCounters(&_counters);
    _tpLayer.setCounters(&_counters);
    _ipTunnelServer.setCounters(&_counters);

    // TP is the bus link: the network layer sends the device's group objects onto TP.
    _netLayer.getInterface().dataLinkLayer(_tpLayer);

    // Interface, not a router: never receive group communication over IP multicast.
    _ipLayer.enableRoutingIndications(false);
    // Single-interface device: the TP link sits at entity index 0, so it must opt in to forwarding
    // received bus frames to the tunnel (the index==1 default only fits the coupler topology).
    _tpLayer.forwardToTunnel(true);

    _cemiServerObject.setMediumTypeAsSupported(DptMedium::KNX_TP1);
    _cemiServer.dataLinkLayer(_tpLayer);          // tunnelled L_Data is put onto the TP bus
    _cemiServer.dataLinkLayerPrimary(_ipLayer);
#ifdef KNX_CEMI_TRANSPORT_LAYER
    _cemiServer.transportLayer(_transLayer);      // serve the local cEMI Transport Layer services (03_06_03 §4.1.6)
#endif
    _tpLayer.cemiServer(_cemiServer);
    _ipLayer.cemiServer(_cemiServer);
    _memory.addSaveRestore(&_cemiServerObject);
    _memory.addSaveRestore(&_ipParameters);

    uint8_t count = 1;
    uint16_t suppCommModes = 0x0100;
    _cemiServerObject.writeProperty(PID_COMM_MODES_SUPPORTED, 1, (uint8_t*)&suppCommModes, count); // Bit 0 = "LinkLayer supported"

#ifdef OPENKNX_HW_BUSMON
    // Let a KNX-Busmonitor tunnel put the TP chip into HW monitor mode.
    _ipTunnelServer.setHwBusMonitorDll(&_tpLayer);
#endif

    // Set Mask Version in Device Object depending on the BAU
    _deviceObj.maskVersion(0x07B0);

    // Set which interface objects are available in the device object
    // This differs from BAU to BAU with different medium types.
    // See PID_IO_LIST
    Property* prop = _deviceObj.property(PID_IO_LIST);
    prop->write(1, (uint16_t) OT_DEVICE);
    prop->write(2, (uint16_t) OT_ADDR_TABLE);
    prop->write(3, (uint16_t) OT_ASSOC_TABLE);
    prop->write(4, (uint16_t) OT_GRP_OBJ_TABLE);
    prop->write(5, (uint16_t) OT_APPLICATION_PROG);
    prop->write(6, (uint16_t) OT_IP_PARAMETER);
#if defined(USE_DATASECURE) && defined(USE_CEMI_SERVER)
    prop->write(7, (uint16_t) OT_SECURITY);
    prop->write(8, (uint16_t) OT_CEMI_SERVER);
#elif defined(USE_DATASECURE)
    prop->write(7, (uint16_t) OT_SECURITY);
#elif defined(USE_CEMI_SERVER)
    prop->write(7, (uint16_t) OT_CEMI_SERVER);
#endif
}

InterfaceObject* Bau07B0IP::getInterfaceObject(uint8_t idx)
{
    switch (idx)
    {
        case 0:
            return &_deviceObj;
        case 1:
            return &_addrTable;
        case 2:
            return &_assocTable;
        case 3:
            return &_groupObjTable;
        case 4:
            return &_appProgram;
        case 5: // would be app_program 2
            return nullptr;
        case 6:
            return &_ipParameters;
#if defined(USE_DATASECURE) && defined(USE_CEMI_SERVER)
        case 7:
            return &_secIfObj;
        case 8:
            return &_cemiServerObject;
#elif defined(USE_CEMI_SERVER)
        case 7:
            return &_cemiServerObject;
#elif defined(USE_DATASECURE)
        case 7:
            return &_secIfObj;
#endif
        default:
            return nullptr;
    }
}

InterfaceObject* Bau07B0IP::getInterfaceObject(ObjectType objectType, uint16_t objectInstance)
{
    (void) objectInstance;

    switch (objectType)
    {
        case OT_DEVICE:
            return &_deviceObj;
        case OT_ADDR_TABLE:
            return &_addrTable;
        case OT_ASSOC_TABLE:
            return &_assocTable;
        case OT_GRP_OBJ_TABLE:
            return &_groupObjTable;
        case OT_APPLICATION_PROG:
            return &_appProgram;
        case OT_IP_PARAMETER:
            return &_ipParameters;
#ifdef USE_DATASECURE
        case OT_SECURITY:
            return &_secIfObj;
#endif
#ifdef USE_CEMI_SERVER
        case OT_CEMI_SERVER:
            return &_cemiServerObject;
#endif
        default:
            return nullptr;
    }
}

void Bau07B0IP::doMasterReset(EraseCode eraseCode, uint8_t channel)
{
    // Common SystemB objects
    BauSystemB::doMasterReset(eraseCode, channel);

    _ipParameters.masterReset(eraseCode, channel);
}

bool Bau07B0IP::enabled()
{
    return _tpLayer.enabled() && _ipLayer.enabled();
}

void Bau07B0IP::enabled(bool value)
{
    _tpLayer.enabled(value);
    _ipLayer.enabled(value);
}

// Rebuild the KNXnet/IP endpoint after the IP interface changed, so the device keeps answering
// SEARCH_REQUEST for its whole operational life.
bool Bau07B0IP::networkChanged(bool afterOutage)
{
    return _ipLayer.networkChanged(afterOutage);
}

void Bau07B0IP::loop()
{
    _ipLayer.loop();   // KNXnet/IP endpoint: pumps discovery + tunnel
    _tpLayer.loop();   // bus
    BauSystemBDevice::loop();
    _ipTunnelServer.loop();
    updateDeviceState();
}

// 03_08_03 3.5.2 p.20: the KNX-fault bit is set once telegrams cannot be transmitted on the KNX subnetwork
// for five seconds and cleared when communication resumes. busOperational() is the driver's debounced
// "chip link up and bus voltage present" answer, so this only adds the five-second qualification on top.
// The IP-fault bit is fed from the network module, which owns the link state; both share the change flag.
// Non-blocking: two comparisons and, on an edge, one queued datagram per open device management connection.
void Bau07B0IP::updateDeviceState()
{
    const bool busDown = !_tpLayer.getTPUart().busOperational();
    const uint32_t now = millis();

    if (!busDown)
        _knxFaultSince = 0;
    else if (_knxFaultSince == 0)
        _knxFaultSince = now ? now : 1; // 0 marks "no fault pending", so never store it as a timestamp

    const bool knxFault = (_knxFaultSince != 0) && (now - _knxFaultSince >= 5000);
    _ipParameters.deviceStateBit(IpParameterObject::DeviceStateKnxFault, knxFault);

    if (!_deviceStateInit)
    {
        // Seed the state at start-up without reporting it: there is no client to inform yet, and the bus
        // is briefly "not operational" until the driver has the chip connected.
        _deviceStateInit = true;
        _ipParameters.takeDeviceStateChanged();
        return;
    }

    if (_ipParameters.takeDeviceStateChanged())
    {
        const uint8_t state = _ipParameters.deviceState();
        _cemiServer.propertyInfoIndication(OT_IP_PARAMETER, 1, PID_KNXNETIP_DEVICE_STATE, &state, 1);
    }
}

TPAckType Bau07B0IP::isAckRequired(uint16_t address, bool isGrpAddr)
{
    if (isGrpAddr)
    {
        // ACK for broadcasts
        if (address == 0)
            return TPAckType::AckReqAck;
        // is group address in group address table? ACK if yes.
        if (_addrTable.contains(address))
            return TPAckType::AckReqAck;
        // No L2-ACK on behalf of a tunnel client: isSentToTunnel() is true for EVERY group address while any
        // tunnel is open, so the interface would acknowledge foreign group telegrams and suppress the TP1
        // repetition a receiver that missed the frame depends on. ACKing on behalf is coupler behaviour (091A).
        return TPAckType::AckReqNone;
    }

    // Also ACK for our own individual address
    if (address == _deviceObj.individualAddress())
        return TPAckType::AckReqAck;
#ifdef KNX_TUNNEL_IA_DEFENCE
    // 03_08_04 2.2.2 p.7 asks the server to defend its additional individual addresses so an address-in-use
    // check sees them as occupied. 03_05_02 2.22.3 p.51 settles what that means on the wire: an L2
    // acknowledge means occupied, its absence means "not occupied at this moment". (2.19 p.35 prints the
    // opposite for the same step; 2.3 p.14 runs the identical sequence and reads "not occupied", so p.35 is
    // an error in the standard -- do not build on that line.) Only the HW ACK bit is set, nothing is
    // transmitted: an earlier attempt fabricated a T_Disconnect here and the un-acked TX became a
    // Protocol-Error on the NCN.
    // SCOPE: isTunnelAddress() matches the addresses of OPEN connections, so this defends those and NOT the
    // configured-but-unconnected ones the clause is really about. Closing that half needs the pool cached
    // off the TP hot path, plus a T_Disconnect for the connection-oriented check (03_05_02 2.19).
    if (_ipTunnelServer.isTunnelAddress(address))
        return TPAckType::AckReqAck;
#else
    // ACK for an individual address assigned to a currently-open tunnel connection
    if (_ipTunnelServer.isSentToTunnel(address, isGrpAddr))
        return TPAckType::AckReqAck;
#endif

    return TPAckType::AckReqNone;
}

TpUartDataLinkLayer* Bau07B0IP::getDataLinkLayer() {
    return (TpUartDataLinkLayer*)&_tpLayer;
}

TpUartDataLinkLayer* Bau07B0IP::getSecondaryDataLinkLayer() {
    return (TpUartDataLinkLayer*)&_tpLayer;
}

IpDataLinkLayer* Bau07B0IP::getPrimaryDataLinkLayer() {
    return (IpDataLinkLayer*)&_ipLayer;
}

#ifdef OPENKNX_FTC_CLIENT
uint16_t Bau07B0IP::ftcTxQueueSize() {
    return (uint16_t)_tpLayer.getTPUart().getTransmitter().queueSize();
}
#endif
#endif

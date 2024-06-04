#include "data_link_layer.h"

#include "bits.h"
#include "platform.h"
#include "device_object.h"
#include "cemi_server.h"
#include "cemi_frame.h"


void DataLinkLayerCallbacks::activity(uint8_t info)
{
    if(_activityCallback)
        _activityCallback(info);
}

void DataLinkLayerCallbacks::setActivityCallback(ActivityCallback activityCallback)
{
    _activityCallback = activityCallback;
}

DataLinkLayer::DataLinkLayer(DeviceObject& devObj, NetworkLayerEntity& netLayerEntity, Platform& platform, BusAccessUnit& busAccessUnit) :
    _deviceObject(devObj), _networkLayerEntity(netLayerEntity), _platform(platform), _bau(busAccessUnit)
{
#ifdef KNX_ACTIVITYCALLBACK
    _netIndex = netLayerEntity.getEntityIndex();
#endif
}

#ifdef USE_CEMI_SERVER

void DataLinkLayer::cemiServer(CemiServer& cemiServer)
{
    _cemiServer = &cemiServer;
}

#ifdef KNX_TUNNELING
void DataLinkLayer::dataRequestToTunnel(CemiFrame& frame)
{
    println("default dataRequestToTunnel");
}

void DataLinkLayer::dataConfirmationToTunnel(CemiFrame& frame)
{
    println("default dataConfirmationToTunnel");
}

void DataLinkLayer::dataIndicationToTunnel(CemiFrame& frame)
{
    println("default dataIndicationToTunnel");
}

bool DataLinkLayer::isTunnelAddress(uint16_t addr)
{
    println("default IsTunnelAddress");
    return false;
}
#endif

void DataLinkLayer::dataRequestFromTunnel(CemiFrame& frame)
{
    _cemiServer->dataConfirmationToTunnel(frame);

    frame.messageCode(L_data_ind);
    
    // Send to local stack ( => cemiServer for potential other tunnel and network layer for routing)
    frameReceived(frame);

#ifdef KNX_TUNNELING
    // TunnelOpti
    // Optimize performance when receiving unicast data over tunnel wich is not meant to be used on the physical TP line
    // dont send to knx when 
    // a) frame is individual adressed AND

    // b) destionation == PA of Tunnel-Server  OR
    // c) destination == PA of a Tunnel OR (TODO)
    // d) destination is not the TP/secondary line/segment but IP/primary (TODO)

    if(frame.addressType() == AddressType::IndividualAddress && (
        frame.destinationAddress() == _deviceObject.individualAddress() ||
        false ||
        false ))
    {
        return;
    }
#endif
    
    // Send to KNX medium
    sendFrame(frame);
}
#endif

void DataLinkLayer::dataRequest(AckType ack, AddressType addrType, uint16_t destinationAddr, uint16_t sourceAddr, FrameFormat format, Priority priority, NPDU& npdu)
{
    // Normal data requests and broadcasts will always be transmitted as (domain) broadcast with domain address for open media (e.g. RF medium) 
    // The domain address "simulates" a closed medium (such as TP) on an open medium (such as RF or PL)
    // See 3.2.5 p.22
    sendTelegram(npdu, ack, destinationAddr, addrType, sourceAddr, format, priority, Broadcast);
}

void DataLinkLayer::systemBroadcastRequest(AckType ack, FrameFormat format, Priority priority, NPDU& npdu, uint16_t sourceAddr)
{
    // System Broadcast requests will always be transmitted as broadcast with KNX serial number for open media (e.g. RF medium) 
    // See 3.2.5 p.22
    sendTelegram(npdu, ack, 0, GroupAddress, sourceAddr, format, priority, SysBroadcast);
}

void DataLinkLayer::dataConReceived(CemiFrame& frame, bool success)
{
    MessageCode backupMsgCode = frame.messageCode();
    frame.messageCode(L_data_con);
    frame.confirm(success ? ConfirmNoError : ConfirmError);
    AckType ack = frame.ack();
    AddressType addrType = frame.addressType();
    uint16_t destination = frame.destinationAddress();
    uint16_t source = frame.sourceAddress();
    FrameFormat type = frame.frameType();
    Priority priority = frame.priority();
    NPDU& npdu = frame.npdu();
    SystemBroadcast systemBroadcast = frame.systemBroadcast();

#ifdef USE_CEMI_SERVER
    // if the confirmation was caused by a tunnel request then
    // do not send it to the local stack
    if (frame.sourceAddress() == _cemiServer->clientAddress())
    {
        // Stop processing here and do NOT send it the local network layer
        return;
    }
#endif    

    if (addrType == GroupAddress && destination == 0)
            if (systemBroadcast == SysBroadcast)
                _networkLayerEntity.systemBroadcastConfirm(ack, type, priority, source, npdu, success);
            else
                _networkLayerEntity.broadcastConfirm(ack, type, priority, source, npdu, success);
    else
        _networkLayerEntity.dataConfirm(ack, addrType, destination, type, priority, source, npdu, success);

    frame.messageCode(backupMsgCode);
}

void DataLinkLayer::frameReceived(CemiFrame& frame)
{
    AckType ack = frame.ack();
    AddressType addrType = frame.addressType();
    uint16_t destination = frame.destinationAddress();
    uint16_t source = frame.sourceAddress();
    FrameFormat type = frame.frameType();
    Priority priority = frame.priority();
    NPDU& npdu = frame.npdu();
    uint16_t ownAddr = _deviceObject.individualAddress();
    SystemBroadcast systemBroadcast = frame.systemBroadcast();

#ifdef USE_CEMI_SERVER
    // Do not send our own message back to the tunnel
#ifdef KNX_TUNNELING
    //we dont need to check it here
    // send inbound frames to the tunnel if we are the secondary (TP) interface
    if( _networkLayerEntity.getEntityIndex() == 1)
        _cemiServer->dataIndicationToTunnel(frame);
#else
    if (frame.sourceAddress() != _cemiServer->clientAddress())
    {
        _cemiServer->dataIndicationToTunnel(frame);
    }
#endif
#endif

    // print("Frame received destination: ");
    // print(destination, 16);
    // println();
    // print("frameReceived: frame valid? :");
    // println(npdu.frame().valid() ? "true" : "false");
    if (source == ownAddr)
        _deviceObject.individualAddressDuplication(true);

    if (addrType == GroupAddress && destination == 0)
    {
        if (systemBroadcast == SysBroadcast)
            _networkLayerEntity.systemBroadcastIndication(ack, type, npdu, priority, source);
        else 
            _networkLayerEntity.broadcastIndication(ack, type, npdu, priority, source);
    }
    else
    {
        _networkLayerEntity.dataIndication(ack, addrType, destination, type, npdu, priority, source);
    }
}

bool DataLinkLayer::sendTelegram(NPDU & npdu, AckType ack, uint16_t destinationAddr, AddressType addrType, uint16_t sourceAddr, FrameFormat format, Priority priority, SystemBroadcast systemBroadcast, bool doNotRepeat)
{
    CemiFrame& frame = npdu.frame();
    // print("Send telegram frame valid ?: ");
    // println(frame.valid()?"true":"false");
    frame.messageCode(L_data_ind);
    frame.destinationAddress(destinationAddr);
    frame.sourceAddress(sourceAddr);
    frame.addressType(addrType);
    frame.priority(priority);
    frame.repetition(doNotRepeat?NoRepitiion:RepetitionAllowed);
    frame.systemBroadcast(systemBroadcast);

    if (npdu.octetCount() <= 15)
        frame.frameType(StandardFrame);
    else
        frame.frameType(format);


    if (!frame.valid())
    {
        println("invalid frame");
        return false;
    }

//    if (frame.npdu().octetCount() > 0)
//    {
//        _print("<- DLL ");
//        frame.apdu().printPDU();
//    }

    bool sendTheFrame = true;
    bool success = true;

#ifdef KNX_TUNNELING
    // TunnelOpti
    // Optimize performance when sending unicast data over tunnel wich is not meant to be used on the physical TP line
    // dont send to knx when 
    // a) we are the secondary interface (e.g. TP) AND
    // b) destination == PA of a Tunnel (TODO)

    if(_networkLayerEntity.getEntityIndex() == 1)    // don't send to tp if we are the secondary (TP) interface AND the destination is a tunnel
    {
        if(isTunnelingPA(destinationAddr))
            sendTheFrame = false;
    }
#endif

    // The data link layer might be an open media link layer
    // and will setup rfSerialOrDoA, rfInfo and rfLfn that we also 
    // have to send through the cEMI server tunnel
    // Thus, reuse the modified cEMI frame as "frame" is only passed by reference here!
    if(sendTheFrame)
        success = sendFrame(frame);

#ifdef USE_CEMI_SERVER
    CemiFrame tmpFrame(frame.data(), frame.totalLenght());
    // We can just copy the pointer for rfSerialOrDoA as sendFrame() sets
    // a pointer to const uint8_t data in either device object (serial) or
    // RF medium object (domain address)

#ifdef USE_RF
    tmpFrame.rfSerialOrDoA(frame.rfSerialOrDoA()); 
    tmpFrame.rfInfo(frame.rfInfo());
    tmpFrame.rfLfn(frame.rfLfn());
#endif
    tmpFrame.confirm(ConfirmNoError);

    if(_networkLayerEntity.getEntityIndex() == 1)    // only send to tunnel if we are the secondary (TP) interface
        _cemiServer->dataIndicationToTunnel(tmpFrame);
#endif

    return success;
}

uint8_t* DataLinkLayer::frameData(CemiFrame& frame)
{
    return frame._data;
}

#ifdef KNX_TUNNELING
bool DataLinkLayer::isTunnelingPA(uint16_t pa)
{
    uint8_t num = 0;
    uint32_t len = 0;
    uint8_t** data = nullptr;
    _bau.propertyValueRead(OT_IP_PARAMETER, 0, PID_ADDITIONAL_INDIVIDUAL_ADDRESSES, num, 1, data, len);
    // ObjectType objectType, uint8_t objectInstance, uint8_t propertyId, uint8_t& numberOfElements, uint16_t startIndex, uint8_t** data, uint32_t& length); PID_ADDITIONAL_INDIVIDUAL_ADDRESSES
    return false;
}

bool DataLinkLayer::isRoutedPA(uint16_t pa)
{
    // from network-layer-coupler
    /*
    if ((_deviceObj.individualAddress() & 0x00FF) == 0x00)
    {
        // Device is a router
        // Check if line coupler or backbone coupler
        if ((_deviceObj.individualAddress() & 0x0F00) == 0x0)
        {
            // Device is a backbone coupler -> individual address: x.0.0
            _couplerType = BackboneCoupler;
        }
        else
        {
            // Device is a line coupler -> individual address: x.y.0
            _couplerType = LineCoupler;
        }
    }
    */

   /*
   bool NetworkLayerCoupler::isRoutedIndividualAddress(uint16_t individualAddress, uint8_t srcIfIndex)
{
    // TODO: improve: we have to be notified about anything that might affect routing decision
    // Ugly: we could ALWAYS evaluate coupler type for every received frame
    if (_currentAddress != _deviceObj.individualAddress())
    {
        evaluateCouplerType();
    }

    // See KNX spec.: Network Layer (03/03/03) and AN161 (Coupler model 2.0)
    /
        * C  hop count value contained in the N-protocol header
        * D  low order octet of the Destination Address, i.e. Device Address part
        * G  Group Address
        * SD low nibble of high order octet plus low order octet, i.e. Line Address + Device Address
        * Z  high nibble of high order octet of the Destination Address, i.e. Area Address
        * ZS high order octet of the Destination Address, i.e. hierarchy information part: Area Address + Line Address
    /
    uint16_t ownSNA = _deviceObj.individualAddress() & 0xFF00; // Own subnetwork address (area + line)
    uint16_t ownAA = _deviceObj.individualAddress() & 0xF000;  // Own area address
    uint16_t ZS = individualAddress & 0xFF00;                        // destination subnetwork address (area + line)
    uint16_t Z = individualAddress & 0xF000;                         // destination area address


    if (_couplerType == LineCoupler)
    {
        // Main line to sub line routing
        if (srcIfIndex == kPrimaryIfIndex)
        {
            if (ZS != ownSNA)
            {
                // IGNORE_TOTALLY
                return false;
            }
            return true;
        }
        else if (srcIfIndex == kSecondaryIfIndex) // Sub line to main line routing
        {
            if (ZS != ownSNA)
            {
                // ROUTE_XXX
                return true;
            }
            else
            {
                return false;
            }
        }
        else
        {
            //not from primiary not from sec if, should not happen
            return false;
        }
    }
    else if (_couplerType == BackboneCoupler)
    {
        // Backbone line to main line routing
        if (srcIfIndex == kPrimaryIfIndex)
        {
            if (Z != ownAA)
            {
                return false;
            }

            return true;
        }
        else  if (srcIfIndex == kSecondaryIfIndex)         // Main line to backbone line routing
        {
            if (Z != ownAA)
            {
                return true;
            }
            else
            {
                return false;
            }
        }
        else
        {
            //not from primiary not from sec if, should not happen
            return false;
        }
    }
    else
    {
        //unknown coupler type, should not happen
        return false;
    }
}
   */
  return false;
}
#endif


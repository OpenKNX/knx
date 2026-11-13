#include "config.h"
#ifdef USE_TP
#pragma GCC optimize("O3")

#include "address_table_object.h"
#include "bits.h"
#include "cemi_frame.h"
#include "device_object.h"
#include "platform.h"
#include "tpuart_data_link_layer.h"

TPUart::DataLinkLayer* TpUartDataLinkLayer::tpuart()
{
    if (_tpuart == nullptr && _platform.interface() != nullptr)
    {
        _tpuart = new TPUart::DataLinkLayer(*_platform.interface());
        _tpuart->registerFrameCallback(std::bind(&TpUartDataLinkLayer::processRxFrame, this, std::placeholders::_1));
        _tpuart->registerMessage(std::bind(&TpUartDataLinkLayer::printMessage, this, std::placeholders::_1, std::placeholders::_2));
        // called from the tick (timer context): must stay short, non-blocking and without heap
        _tpuart->registerCheckAcknowledge(std::bind(&TpUartDataLinkLayer::checkAcknowledge, this, std::placeholders::_1, std::placeholders::_2));
    }

    return _tpuart;
}

TPUart::DataLinkLayer& TpUartDataLinkLayer::getTPUart()
{
    TPUart::DataLinkLayer* tpuart = this->tpuart();
    if (tpuart == nullptr)
    {
        println("TPUart interface is not set on the platform");
        _platform.fatalError();
    }

    return *tpuart;
}

TpUartDataLinkLayer::~TpUartDataLinkLayer()
{
    delete _tpuart;
}

void TpUartDataLinkLayer::setRepetitions(uint8_t nack, uint8_t busy)
{
    if (_tpuart)
        _tpuart->setRepetitions(nack, busy);
}

bool TpUartDataLinkLayer::sendFrame(CemiFrame &cemiFrame)
{
    // when not connected or in monitoring mode, discard the frame - silently
    if (!isConnected() || isMonitoring())
    {
        dataConReceived(cemiFrame, false);
        return false;
    }

    const uint16_t length = cemiFrame.telegramLengthtTP();
    uint8_t tpData[TPUart::TPUART_BUFFER_SIZE];
    if (length > sizeof(tpData))
    {
        printMessage("Ignore frame because it is too long!", true);
        dataConReceived(cemiFrame, false);
        return false;
    }

    cemiFrame.fillTelegramTP(tpData);

    // when frame not enqueued, discard the frame with a message (the reason is reported by TPUart itself)
    if (!_tpuart->pushTransmitQueue(tpData, length))
    {
        printMessage("Ignore frame because it was not accepted by the transmit queue!", true);
        dataConReceived(cemiFrame, false);
        return false;
    }

    // printHex("  CEMI>: ", cemiFrame.data(), cemiFrame.dataLength());
    return true;
}

void TpUartDataLinkLayer::connected(bool state /* = true */)
{
    if (state)
        println("TP is connected");
    else
        println("TP is disconnected");

    _connected = state;
}

void TpUartDataLinkLayer::reset()
{
    if (_tpuart)
        _tpuart->reset();
}

void TpUartDataLinkLayer::stop(bool state)
{
    if (!_initialized)
        return;

    _tpuart->stopMode(state);
}

void TpUartDataLinkLayer::requestBusy(bool state)
{
    if (!_initialized)
        return;

    _tpuart->busyMode(state);
}

void TpUartDataLinkLayer::monitor()
{
    if (!_initialized)
        return;

    _tpuart->startMonitoring();
}

void TpUartDataLinkLayer::initialize()
{
    if (_initialized)
        return;

    TPUart::DataLinkLayer* tpuart = this->tpuart();
    if (tpuart == nullptr)
        return;

#ifdef NCN5120
    tpuart->begin(TPUart::BcuType::Ncn5120);
#else
    tpuart->begin(TPUart::BcuType::Tpuart2);
#endif
    _initialized = true;
}

void TpUartDataLinkLayer::enabled(bool value)
{
    initialize();

    stop(!value);
}

bool TpUartDataLinkLayer::enabled() const
{
    return _initialized && _tpuart->isConnected();
}

void TpUartDataLinkLayer::loop()
{
    if (!_initialized)
        return;

    const uint16_t individualAddress = _deviceObject.individualAddress();
    if (individualAddress > 0 && _individualAddress != individualAddress)
    {
        _individualAddress = individualAddress;
        _tpuart->setOwnAddress(_individualAddress);
    }

    _tpuart->loop();
}

DptMedium TpUartDataLinkLayer::mediumType() const
{
    return DptMedium::KNX_TP1;
}

void TpUartDataLinkLayer::powerControl(bool state)
{
    if (!_initialized)
        return;

    _tpuart->powerControl(state);
}

TpUartDataLinkLayer::TpUartDataLinkLayer(DeviceObject &devObj,
                                         NetworkLayerEntity &netLayerEntity,
                                         Platform &platform,
                                         BusAccessUnit &busAccessUnit,
#ifdef KNX_TUNNELING
                                        IpTunnelServer& ipTunnelServer,
#endif
                                         ITpUartCallBacks &cb,
                                         DataLinkLayerCallbacks *dllcb)
    : DataLinkLayer(devObj, netLayerEntity, platform, busAccessUnit
#ifdef KNX_TUNNELING
                                        ,ipTunnelServer
#endif
    ),
      _cb(cb),
      _dllcb(dllcb)
{
}

bool TpUartDataLinkLayer::isConnected()
{
    return _tpuart && _tpuart->isConnected();
}

bool TpUartDataLinkLayer::isStopped()
{
    return false;
}

bool TpUartDataLinkLayer::isBusy()
{
    return false;
}

bool TpUartDataLinkLayer::isMonitoring()
{
    return _tpuart && _tpuart->isBusMonitor();
}

TPUart::AckType TpUartDataLinkLayer::checkAcknowledge(unsigned short destination, bool isGroupAddress)
{
    TPAckType ack = _cb.isAckRequired(destination, isGroupAddress);

    if (ack == TPAckType::AckReqAck)
        return TPUart::AckType::Addressed;

    if (ack == TPAckType::AckReqBusy)
        return TPUart::AckType::Busy;

    if (ack == TPAckType::AckReqNack)
        return TPUart::AckType::Nack;

    return TPUart::AckType::None;
}

void TpUartDataLinkLayer::processRxFrame(TPUart::Frame &tpFrame)
{
    if (isMonitoring())
    {
        printMessage(tpFrame.printFrame().c_str(), false);
    }
    
#if MASK_VERSION != 0x091A
    if (tpFrame.isFiltered())
        return;
#endif

    // broken telegrams are delivered as well (CRC error, truncated, ...), they must not reach the stack
    if (tpFrame.isInvalid())
        return;

    uint8_t *cemiData = tpFrame.cemiData();
    if (cemiData == nullptr)
        return;

    CemiFrame cemiFrame(cemiData, tpFrame.cemiSize());

    if (tpFrame.isTransmitted())
    {
        // own telegram: positive only when the BCU confirmed it and the bus did not NACK it
        dataConReceived(cemiFrame, tpFrame.isDataCon() && !tpFrame.isNack());
        free(cemiData);
        return;
    }

    // printHex("  TP<: ", (const uint8_t *)tpFrame.data(), tpFrame.length());
    // printHex("  CEMI<: ", cemiFrame.data(), cemiFrame.dataLength());

#ifdef KNX_ACTIVITYCALLBACK
    if (_dllcb)
        _dllcb->activity((_netIndex << KNX_ACTIVITYCALLBACK_NET) | (KNX_ACTIVITYCALLBACK_DIR_RECV << KNX_ACTIVITYCALLBACK_DIR));
#endif

    frameReceived(cemiFrame);
    free(cemiData);
}

void TpUartDataLinkLayer::printMessage(const char *message, bool error)
{
    if (error)
        print("\e[0;31m");

    print(message);

    if (error)
        print("\e[0m");

    println("");
}

#endif
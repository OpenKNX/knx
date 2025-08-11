#include "config.h"
#ifdef USE_TP
#pragma GCC optimize("O3")

#include "address_table_object.h"
#include "bits.h"
#include "cemi_frame.h"
#include "device_object.h"
#include "platform.h"
#include "tpuart_data_link_layer.h"

void TpUartDataLinkLayer::setRepetitions(uint8_t nack, uint8_t busy)
{
    _tpuart.setRepetitions(nack, busy);
}

bool TpUartDataLinkLayer::sendFrame(CemiFrame &cemiFrame)
{
    uint8_t *tpData = (uint8_t *)malloc(cemiFrame.telegramLengthtTP());
    cemiFrame.fillTelegramTP(tpData);

    TPUart::Frame *tpFrame = new TPUart::Frame((char *)tpData, cemiFrame.telegramLengthtTP());

    // when not connected or in monitoring mode, discard the frame - silently
    if (!_tpuart.isConnected() || _tpuart.isMonitoring())
    {
        free(tpData);
        dataConReceived(cemiFrame, false);
        return false;
    }

    // when frame not enqueued, discard the frame with a message
    if (!_tpuart.pushTransmitQueue(tpFrame))
    {
        free(tpData);
        printMessage("Ignore frame because transmit queue is full!", true);
        dataConReceived(cemiFrame, false);
        return false;
    }

    free(tpData);
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
    _tpuart.reset();
}

void TpUartDataLinkLayer::stop(bool state)
{
    if (!_initialized)
        return;

    _tpuart.stopMode(state);
}

void TpUartDataLinkLayer::requestBusy(bool state)
{
    _tpuart.busyMode(state);
}

void TpUartDataLinkLayer::monitor()
{
    if (!_initialized)
        return;

    _tpuart.startMonitoring();
}

void TpUartDataLinkLayer::initialize()
{
    if (_initialized)
        return;

    // After an unusual device restart, perform a reset, as the TPUart may still be in an incorrect state.
    if (!_initialized && _platform.interface() != nullptr)
    {
        _tpuart.registerReceivedFrame(std::bind(&TpUartDataLinkLayer::processRxFrame, this, std::placeholders::_1));
        _tpuart.registerMessage(std::bind(&TpUartDataLinkLayer::printMessage, this, std::placeholders::_1, std::placeholders::_2));
        _tpuart.registerCheckAcknowledge(std::bind(&TpUartDataLinkLayer::checkAcknowledge, this, std::placeholders::_1, std::placeholders::_2));
#ifdef NCN5120
        _tpuart.begin(TPUart::BcuType::BCU_NCN5120, _platform.interface());
#else
        _tpuart.begin(TPUart::BcuType::BCU_TPUART2, _platform.interface());
#endif
        _initialized = true;
    }
}

void TpUartDataLinkLayer::enabled(bool value)
{
    initialize();

    stop(!value);
}

bool TpUartDataLinkLayer::enabled() const
{
    return _initialized && _tpuart.isConnected();
}

void TpUartDataLinkLayer::loop()
{
    if (!_initialized)
        return;

    const uint16_t individualAddress = _deviceObject.individualAddress();
    if (individualAddress > 0 && _individualAddress != individualAddress)
    {
        _individualAddress = individualAddress;
        _tpuart.setOwnAddress(_individualAddress);
    }

    _tpuart.process();
}

DptMedium TpUartDataLinkLayer::mediumType() const
{
    return DptMedium::KNX_TP1;
}

void TpUartDataLinkLayer::powerControl(bool state)
{
    _tpuart.powerControl(state);
}

TpUartDataLinkLayer::TpUartDataLinkLayer(DeviceObject &devObj,
                                         NetworkLayerEntity &netLayerEntity,
                                         Platform &platform,
                                         BusAccessUnit &busAccessUnit,
                                         ITpUartCallBacks &cb,
                                         DataLinkLayerCallbacks *dllcb)
    : DataLinkLayer(devObj, netLayerEntity, platform, busAccessUnit),
      _cb(cb),
      _dllcb(dllcb)
{
}

bool TpUartDataLinkLayer::isConnected()
{
    return _tpuart.isConnected();
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
    return _tpuart.isMonitoring();
}

TPUart::AcknowledgeType TpUartDataLinkLayer::checkAcknowledge(unsigned short destination, bool isGroupAddress)
{
    TPAckType ack = _cb.isAckRequired(destination, isGroupAddress);

    if (ack == TPAckType::AckReqAck)
        return TPUart::AcknowledgeType::ACK_Addressed;

    if (ack == TPAckType::AckReqBusy)
        return TPUart::AcknowledgeType::ACK_Busy;

    if (ack == TPAckType::AckReqNack)
        return TPUart::AcknowledgeType::ACK_Nack;

    return TPUart::AcknowledgeType::ACK_None;
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

    uint8_t *cemiData = (uint8_t *)tpFrame.cemiData();
    CemiFrame cemiFrame(cemiData, tpFrame.cemiSize());

    if (tpFrame.isTransmitted()) {
        dataConReceived(cemiFrame, tpFrame.isAck());
        delete cemiData;
        return;
    }

    // printHex("  TP<: ", (const uint8_t *)tpFrame.data(), tpFrame.size());
    // printHex("  CEMI<: ", cemiFrame.data(), cemiFrame.dataLength());

#ifdef KNX_ACTIVITYCALLBACK
    if (_dllcb)
        _dllcb->activity((_netIndex << KNX_ACTIVITYCALLBACK_NET) | (KNX_ACTIVITYCALLBACK_DIR_RECV << KNX_ACTIVITYCALLBACK_DIR));
#endif

    frameReceived(cemiFrame);
    delete cemiData;
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
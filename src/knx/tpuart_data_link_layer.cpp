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
    const uint16_t tpLen = cemiFrame.telegramLengthtTP();

    // Transient serialization buffer. It only has to live until TPUart::Frame below copies it into its own
    // heap buffer for the async TX queue, so a fixed stack buffer replaces the former per-frame malloc()/free()
    // on this IP->TP send hot path (one heap alloc + one free removed per frame). Sized to the max cEMI/TP
    // telegram; the guard keeps fillTelegramTP() from ever writing past it under a corrupt length.
    uint8_t tpData[0xff + APDU_LPDU_DIFF];
#ifdef KNX_FIXES_EC
    if (tpLen > sizeof(tpData)) // impossible for a valid frame -> never OOB fillTelegramTP()
    {
        dataConReceived(cemiFrame, false);
        return false;
    }
#endif
    cemiFrame.fillTelegramTP(tpData);

    TPUart::Frame *tpFrame = new TPUart::Frame((char *)tpData, tpLen);
#ifdef KNX_FIXES_EC
    if (!tpFrame) // heap exhaustion under an IP->TP routing flood -> bail instead of null-deref downstream
    {
        dataConReceived(cemiFrame, false);
        return false;
    }
#endif

    // when not connected or in monitoring mode, discard the frame - silently
    if (!_tpuart.isConnected() || _tpuart.isMonitoring())
    {
        delete tpFrame; // not queued -> free (queue didn't take ownership)
        dataConReceived(cemiFrame, false);
        return false;
    }

    // when frame not enqueued, discard the frame with a message
    if (!_tpuart.pushTransmitQueue(tpFrame))
    {
        delete tpFrame; // queue full, not taken -> free (else leak per dropped frame)
#ifdef KNX_FIXES_EC
        // Do NOT print per dropped frame: under an IP->TP flood the queue stays full and a blocking
        // USB-CDC print on every drop starves the loop() watchdog -> 16s reset. Rate-limit to ~1/1024.
        static uint16_t _qFullDrops = 0;
        if ((_qFullDrops++ & 0x3FF) == 0)
            printMessage("TP transmit queue full - dropping routed frame(s)", true);
#else
        printMessage("Ignore frame because transmit queue is full!", true);
#endif
        dataConReceived(cemiFrame, false);
        return false;
    }

    // success: queue took ownership of tpFrame -> it deletes it after TX (no delete here)
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
    _monitorConsoleLog = false; // leaving monitor mode: stop any console echo
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

void TpUartDataLinkLayer::monitorWithConsoleLog()
{
    _monitorConsoleLog = true; // echo raw frames to the console (console-initiated busmon only)
    monitor();
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
    // Busmon-only carrier (produced by the receiver only while monitoring): a full FCS-failed frame
    // (isErrored, BUG B). Forward the raw bytes to the busmon and return before any cEMI conversion.
    // Handled unconditionally (not under isMonitoring) so a monitor-teardown race cannot leak the carrier
    // into cemiData()/frameReceived, which would read past its buffer; never delivered up to the link layer.
    if (tpFrame.isErrored())
    {
#if defined(OPENKNX_HW_BUSMON) && defined(KNX_TUNNELING)
        if (_ipTunnelServer.busMonitorActive())
            _ipTunnelServer.busMonitorFrame((uint8_t*)tpFrame.data(), tpFrame.size());
#endif
        return;
    }

    if (isMonitoring())
    {
        // Echo raw frames to the console only for a console-initiated busmon (`bcu mon`);
        // an ETS-initiated busmon stays silent on the console.
        if (_monitorConsoleLog)
            printMessage(tpFrame.printFrame().c_str(), false);
#if defined(OPENKNX_HW_BUSMON) && defined(KNX_TUNNELING)
        // Forward every raw monitor-mode frame (incl. error/un-ACKed frames, FCS) to the ETS busmon tunnel.
        if (_ipTunnelServer.busMonitorActive())
            _ipTunnelServer.busMonitorFrame((uint8_t*)tpFrame.data(), tpFrame.size());
#endif
    }

#if MASK_VERSION != 0x091A
    if (tpFrame.isFiltered())
        return;
#endif

    uint8_t *cemiData = (uint8_t *)tpFrame.cemiData();
    CemiFrame cemiFrame(cemiData, tpFrame.cemiSize());

    if (tpFrame.isTransmitted()) {
        dataConReceived(cemiFrame, tpFrame.isAck());
        free(cemiData); // Frame::cemiData() returns a malloc()'d buffer -> must be free()'d, not delete'd
        return;
    }

    // printHex("  TP<: ", (const uint8_t *)tpFrame.data(), tpFrame.size());
    // printHex("  CEMI<: ", cemiFrame.data(), cemiFrame.dataLength());

#ifdef KNX_ACTIVITYCALLBACK
    if (_dllcb)
        _dllcb->activity((_netIndex << KNX_ACTIVITYCALLBACK_NET) | (KNX_ACTIVITYCALLBACK_DIR_RECV << KNX_ACTIVITYCALLBACK_DIR));
#endif

    frameReceived(cemiFrame);
    free(cemiData); // Frame::cemiData() returns a malloc()'d buffer -> must be free()'d, not delete'd
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
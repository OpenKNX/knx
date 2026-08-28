#include "config.h"
#ifdef USE_TP
#pragma GCC optimize("O3")

#include "address_table_object.h"
#include "bits.h"
#include "cemi_frame.h"
#include "device_object.h"
#include "platform.h"
#include "tpuart_data_link_layer.h"

#ifdef OPENKNX_CON_DIAG
uint16_t g_conRxRecv = 0; // isTransmitted frame reaching dataConReceived (con diag)
#endif

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
    if (tpLen > sizeof(tpData)) // impossible for a valid frame -> never OOB fillTelegramTP()
    {
        dataConReceived(cemiFrame, false);
        return false;
    }
    cemiFrame.fillTelegramTP(tpData);

    TPUart::Frame *tpFrame = new TPUart::Frame((char *)tpData, tpLen);
    if (!tpFrame) // heap exhaustion under an IP->TP routing flood -> bail instead of null-deref downstream
    {
        dataConReceived(cemiFrame, false);
        return false;
    }

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
        if (_counters != nullptr)
            _counters->incrementOverflowToKnx();
        // rate-limit the print (~1 in 1024): under an IP->TP flood the queue stays full and a blocking
        // print on every drop would starve the loop() watchdog.
        static uint16_t _qFullDrops = 0;
        if ((_qFullDrops++ & 0x3FF) == 0)
            printMessage("TP transmit queue full - dropping routed frame(s)", true);
        dataConReceived(cemiFrame, false);
        return false;
    }

    // success: queue took ownership of tpFrame -> it deletes it after TX (no delete here)
    // TODO EC PID 75 increments here at queue-accept, but 03_08_03 2.5.26 wants "successfully transmitted".
    // The exact point is the ACK-confirmed L_Data.con below (isTransmitted -> dataConReceived, gated on
    // tpFrame.isAck()). Deviation = 0 on a healthy bus, over-counts un-ACKed frames; harmless while the
    // statistics capability (PID 70 bit1) is not advertised. Move only with a cross-target audit.
    if (_counters != nullptr)
        _counters->incrementTransmitToKnx();
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
#if defined(OPENKNX_HW_BUSMON) && defined(KNX_TUNNELING)
    // Baseline the loss counter at monitor entry so pre-existing overflows don't set a spurious "lost" bit.
    _lastBusMonRxOverflow = _tpuart.getStatistics().getRxUartOverflow()
                          + _tpuart.getStatistics().getRxSearchBufferOverflow()
                          + _tpuart.getStatistics().getRxFrameBufferOverflow();
#endif
}

void TpUartDataLinkLayer::monitorWithConsoleLog()
{
    _monitorConsoleLog = true; // echo raw frames to the console (console-initiated busmon only)
    monitor();
}

// `bcu mon` toggle. Universal on every TPUart device: 1st call starts the local console busmon (raw echo),
// 2nd stops it. On the interface it additionally coexists with an ETS busmon tunnel (dual owner): the HW
// monitor stays up while EITHER the console (_localBusmon) or an ETS tunnel owns it, and the console echo
// is independent of the ETS stream.
void TpUartDataLinkLayer::toggleConsoleMonitor()
{
    bool etsOn = false;
#if defined(OPENKNX_HW_BUSMON) && defined(KNX_TUNNELING)
    etsOn = _ipTunnelServer.busMonitorActive(); // interface only: an ETS busmon tunnel co-owns the HW monitor
#endif
    if (_localBusmon)
    {
        // local owner OFF: stop the console echo; leave HW busmon only if an ETS tunnel isn't still holding it.
        _localBusmon = false;
        _monitorConsoleLog = false;
        if (!etsOn) reset();
        printMessage("BCU monitor: off", false);
    }
    else
    {
        // local owner ON: echo raw frames; start the HW busmon only if it isn't already running (ETS or fresh).
        _localBusmon = true;
        _monitorConsoleLog = true;
#if defined(OPENKNX_HW_BUSMON) && defined(KNX_TUNNELING)
        // Make the console busmon exclusive too (like the ETS busmon connect): drop any open data/config
        // tunnels so the monitor is the only connection. In HW monitor mode the chip is passive anyway, so an
        // "active" tunnel could no longer pass traffic. No-op if none are open (e.g. an ETS busmon co-owns it).
        _ipTunnelServer.closeTunnelsForBusmon();
#endif
        if (!isMonitoring()) monitor();
        printMessage("BCU monitor: on (raw)", false);
    }
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
#if defined(OPENKNX_HW_BUSMON) && defined(KNX_TUNNELING)
    // Build the cEMI busmon status octet: bit 3 "lost" when RX overflow(s) happened since the last
    // reported frame, bit 7 "frame error" for an FCS-failed frame (03_06_03 EMI §3.3.3.2).
    // 03_06_03 4.1.5.8.1 (p.97): F(7) frame error | B(6) bit error | P(5) parity error | bit4=0 | L(3) lost
    // | seq(2-0). "Bit error" = the NCN's acceptance-window/pulse-duration error (DS p.27), not TP1 parity.
    // A truncated telegram is a lost frame PIECE -> sets Lost, not Frame error.
    auto busMonStatus = [&](bool frameError, bool bitError = false, bool truncated = false) -> uint8_t {
        uint8_t status = frameError ? 0x80 : 0x00;
        if (bitError) status |= 0x40;
        if (truncated) status |= 0x08;
        uint32_t rxOvf = _tpuart.getStatistics().getRxUartOverflow()
                       + _tpuart.getStatistics().getRxSearchBufferOverflow()
                       + _tpuart.getStatistics().getRxFrameBufferOverflow()  // loop-drained rx-frame ring: the primary drop point under IP-TX backpressure
                       + _tpuart.getStatistics().getRxDiscardedBytes();      // swept bytes are a gap in the capture too, and the only one that leaves no other trace
        if (rxOvf != _lastBusMonRxOverflow)
        {
            status |= 0x08; // lost
            _lastBusMonRxOverflow = rxOvf;
        }
        return status;
    };
#endif

#if defined(OPENKNX_HW_BUSMON) && defined(KNX_TUNNELING)
    // Busmon-only carriers (produced by the receiver only while monitoring): a 1-byte standalone L2
    // acknowledge (isAckOnly) or a full FCS-failed frame (isErrored). Forward the raw bytes to the busmon
    // and return before any cEMI conversion. Handled independently of the runtime isMonitoring() state so a
    // monitor-teardown race cannot leak a 1-byte carrier into cemiData()/frameReceived, which would read
    // past its buffer; they are never delivered up to the link layer.
    if (tpFrame.isAckOnly() || tpFrame.isErrored() || tpFrame.isTruncated() || tpFrame.isRaw())
    {
        if (_ipTunnelServer.busMonitorActive())
        {
            // rawLength() = octets actually received; size() (what the length octet promised) would read
            // past the buffer for a truncated frame or an ack carrier.
            _ipTunnelServer.busMonitorFrame((uint8_t*)tpFrame.data(), tpFrame.rawLength(),
                                            busMonStatus(tpFrame.isErrored(), tpFrame.isBitErrored(),
                                                         tpFrame.isTruncated()));
        }
        return;
    }
#endif

    if (isMonitoring())
    {
        // Echo raw frames to the console only for a console-initiated busmon (`bcu mon`);
        // an ETS-initiated busmon stays silent on the console.
        if (_monitorConsoleLog)
            printMessage(tpFrame.printFrame().c_str(), false);
#if defined(OPENKNX_HW_BUSMON) && defined(KNX_TUNNELING)
        // Forward every raw monitor-mode frame (incl. un-ACKed frames, incl. FCS) to the ETS busmon tunnel.
        if (_ipTunnelServer.busMonitorActive())
        {
            _ipTunnelServer.busMonitorFrame((uint8_t*)tpFrame.data(), tpFrame.size(), busMonStatus(false, tpFrame.isBitErrored()));
            // 03_06_03 EMI/IMI §4.1.5.8.1 (p.96): a busmonitor SHALL also transfer the DLL acknowledge
            // (this is the defining difference from L_Raw.ind, §4.1.5.7.5 p.95). The receiver folds the
            // frame-trailing ACK/NACK/BUSY into this frame's flags and consumes the octet, so it never
            // reaches the monitor on its own. Re-emit it here as its own 1-byte L_Busmon.ind right AFTER
            // its telegram (ascending in the ETS trace), rebuilding the raw L2 octet from the flags
            // (ACK 0xCC / NACK 0x0C / BUSY 0xC0 -- exactly the values seen on the bus).
            if (tpFrame.isAck())
            {
                uint8_t ackByte = tpFrame.isBusy() ? 0xC0 : (tpFrame.isNack() ? 0x0C : 0xCC);
                _ipTunnelServer.busMonitorFrame(&ackByte, 1, busMonStatus(false));
            }
        }
#endif
    }

#if MASK_VERSION != 0x091A
    if (tpFrame.isFiltered())
        return;
#endif

    uint8_t *cemiData = (uint8_t *)tpFrame.cemiData();
    CemiFrame cemiFrame(cemiData, tpFrame.cemiSize());

    if (tpFrame.isTransmitted()) {
#ifdef OPENKNX_CON_DIAG
        g_conRxRecv++;
#endif
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
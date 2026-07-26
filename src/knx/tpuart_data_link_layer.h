#pragma once

#include "config.h"
#ifdef USE_TP

#include "TPUart.h"
#include "data_link_layer.h"
#include "tp_frame.h"
#include <stdint.h>
#include <functional>
#include "TPUart.h"
#ifdef ARDUINO_ARCH_ESP32
#include "TPUart/Interface/ESP32.h"
#endif
#ifdef ARDUINO_ARCH_RP2040
#include "TPUart/Interface/RP2040.h"
#endif

class ITpUartCallBacks
{
  public:
    virtual ~ITpUartCallBacks() = default;
    virtual TPAckType isAckRequired(uint16_t address, bool isGrpAddr) = 0;
};

class TpUartDataLinkLayer : public DataLinkLayer
#if defined(OPENKNX_HW_BUSMON) && defined(KNX_TUNNELING)
                          , public IHwBusMonitorDll
#endif
{
    friend class TPUart::DataLinkLayer;

    using DataLinkLayer::_deviceObject;
    using DataLinkLayer::_platform;

  public:
    TpUartDataLinkLayer(DeviceObject& devObj, NetworkLayerEntity& netLayerEntity,
                        Platform& platform, BusAccessUnit& busAccessUnit,
#ifdef KNX_TUNNELING
                        IpTunnelServer& ipTunnelServer,
#endif
                        ITpUartCallBacks& cb, DataLinkLayerCallbacks* dllcb = nullptr);

    void loop();
    void enabled(bool value);
    bool enabled() const;
    DptMedium mediumType() const override;
    void reset();
    void monitor();
    // Console-initiated busmon (`bcu mon`): same as monitor() but also echoes each raw frame to the
    // console. ETS-initiated busmon (hwBusMonEnter) stays silent on the console.
    void monitorWithConsoleLog();
    void stop(bool state);
    void requestBusy(bool state);
    // void forceAck(bool state);
    void setRepetitions(uint8_t nack, uint8_t busy);
    // Alias
    // void setFrameRepetition(uint8_t nack, uint8_t busy);
    bool isConnected();
    bool isMonitoring();
    bool isStopped();
    bool isBusy();
    void resetStats();

#if defined(OPENKNX_HW_BUSMON) && defined(KNX_TUNNELING)
    // IHwBusMonitorDll bridge: let the IP tunnel server drive the TP chip's HW monitor mode.
    void hwBusMonEnter() override { _monitorConsoleLog = false; monitor(); } // ETS busmon: no console echo
    void hwBusMonExit() override { reset(); }
    bool hwBusMonConnected() override { return isConnected(); }
#endif

    void powerControl(bool state);

    TPUart::DataLinkLayer& getTPUart()
    {
        return _tpuart;
    }

  private:
    TPUart::Interface::Abstract* _tpuartInterface = nullptr;
    TPUart::DataLinkLayer _tpuart;
    void initialize();
    TPUart::AcknowledgeType checkAcknowledge(unsigned short destination, bool isGroupAddress);
   
    volatile bool _stopped = false;
    volatile bool _connected = false;
    volatile bool _monitoring = false;
    // True only while a console-initiated busmon (`bcu mon`) is active -> echo raw frames to console.
    volatile bool _monitorConsoleLog = false;
#if defined(OPENKNX_HW_BUSMON) && defined(KNX_TUNNELING)
    // Last-seen combined RX overflow count; edge-detects loss for the busmon status "lost" bit.
    uint32_t _lastBusMonRxOverflow = 0;
#endif
    volatile bool _busy = false;
    volatile bool _initialized = false;
    uint16_t _individualAddress = 0;

    bool sendFrame(CemiFrame& frame);
    inline void connected(bool state = true);
    void processRxFrame(TPUart::Frame& tpFrame);
    void printMessage(const char *message, bool error);

    ITpUartCallBacks& _cb;
    DataLinkLayerCallbacks* _dllcb;
};
#endif

#pragma once

#include "config.h"
#include "bau.h"
#include "security_interface_object.h"
#include "application_program_object.h"
#include "application_layer.h"
#include "secure_application_layer.h"
#include "transport_layer.h"
#include "network_layer.h"
#include "data_link_layer.h"
#include "platform.h"
#include "memory.h"

class BauSystemB : protected BusAccessUnit
{
  public:
    BauSystemB(Platform& platform);
    virtual void loop() = 0;
    virtual bool configured() = 0;
    virtual bool enabled() = 0;
    virtual void enabled(bool value) = 0;

    Platform& platform();
    ApplicationProgramObject& parameters();
    DeviceObject& deviceObject();

    Memory& memory();
    void readMemory();
    void writeMemory();
    void addSaveRestore(SaveRestore* obj);

    bool restartRequest(uint16_t asap, const SecurityControl secCtrl);
#ifdef OPENKNX_FTC
    // OPENKNX_FTC: client role for KnxFileTransfer (ObjectIndex 159). Connectionless by design; the
    // reply arrives via the callback.
    bool ftcSendCommand(uint16_t asap, const SecurityControl secCtrl, uint8_t objectIndex, uint8_t propertyId, uint8_t* data, uint8_t length);
    void ftcSetResponseCallback(void (*cb)(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, uint8_t* data, uint8_t length)) { _ftcResponseCb = cb; }
    // Scan probe: connectionless DeviceDescriptor_Read (type 0 = mask version) to one PA. The reply
    // reaches the client through the callback below. A read only, exactly what a bus scanner sends.
    bool ftcSendDeviceDescriptorRead(uint16_t asap, const SecurityControl secCtrl);
    void ftcSetDeviceDescriptorCallback(void (*cb)(uint16_t pa, uint8_t descriptorType, const uint8_t* data)) { _ftcDdCb = cb; }
    // Device-info: read one interface-object property (serial, order number, app version ...). Answer
    // via the callback. A standard PropertyValue_Read, the same one ETS uses to identify a device.
    bool ftcSendPropertyValueRead(uint16_t asap, const SecurityControl secCtrl, uint8_t objectIndex, uint8_t propertyId,
                                  uint8_t count, uint16_t startIndex);
    void ftcSetPropertyCallback(void (*cb)(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, const uint8_t* data, uint8_t length)) { _ftcPropCb = cb; }
    // Device diagnosis: PropertyValue_WRITE (e.g. PID_PROG_MODE to drive the target's prog-LED, `ftc <pa> led`).
    // Fire-and-forget: the echoed PropertyValue_Response lands harmlessly on _ftcPropCb and is ignored.
    bool ftcSendPropertyValueWrite(uint16_t asap, const SecurityControl secCtrl, uint8_t objectIndex, uint8_t propertyId,
                                   uint8_t count, uint16_t startIndex, uint8_t* data, uint8_t length);
    // Device diagnosis: A_Memory_Read (ETS path to the GA + association tables, `ftc <pa> info ga`). Answer via the callback.
    bool ftcSendMemoryRead(uint16_t asap, const SecurityControl secCtrl, uint8_t number, uint16_t memoryAddress);
    void ftcSetMemoryCallback(void (*cb)(uint16_t pa, uint16_t addr, const uint8_t* data, uint8_t len)) { _ftcMemCb = cb; }
    // CO scan (`ftc scan ... ets`): ETS-parity connection-oriented probe. Reaches old BCU1/BCU2 masks that
    // answer ONLY connection-oriented (the connectionless scan misses them). Driven serially from the client.
    bool ftcScanConnect(uint16_t pa);                       // open a T_Connect to pa (self-guards: no-op if already connected)
    bool ftcScanConnected();                                // T_Connect established?
    void ftcScanReadDescriptor(const SecurityControl& sec); // CO DeviceDescriptor_Read on the open connection
    void ftcScanDisconnect();                               // ALWAYS callable -- also unwedges a stuck Connecting state
    // FTC fast-transfer flow control: current TP transmit-FIFO depth. Base has no TP link -> 0; the
    // coupler/device bau overrides it (Transmitter::queueSize()) so the pump paces against the driver.
    virtual uint16_t ftcTxQueueSize() { return 0; }
    // FTC fast-transfer pacing feedback (delivery-rate / BBR-style). Per report the client reports the
    // MEASURED delivered rate over the window (bytes the target confirmed / elapsed) plus whether the window
    // was clean (0 missing). The IP host pacer uses it: clean -> probe higher; a lossy window -> snap the send
    // rate to the measured delivered rate (the true wire ceiling, not a guess); deliveredBps==0 && !clean = a
    // report-timeout kick -> back off. Base/embedded is a no-op (the real TP FIFO back-pressures the pump).
    virtual void ftcPacingRate(uint32_t deliveredBps, bool clean) { (void)deliveredBps; (void)clean; }
#endif

    uint8_t checkmasterResetValidity(EraseCode eraseCode, uint8_t channel);

    void propertyValueRead(ObjectType objectType, uint8_t objectInstance, uint8_t propertyId,
                           uint8_t& numberOfElements, uint16_t startIndex, 
                           uint8_t **data, uint32_t &length) override;
    void propertyValueWrite(ObjectType objectType, uint8_t objectInstance, uint8_t propertyId,
                            uint8_t& numberOfElements, uint16_t startIndex,
                            uint8_t* data, uint32_t length) override;
    // Read-only access to a property's metadata (used by the cEMI server to pick the correct
    // M_PropWrite error code, e.g. read-only detection). Returns nullptr if object/property is absent.
    Property* property(ObjectType objectType, uint8_t objectInstance, uint8_t propertyId);
    void versionCheckCallback(VersionCheckCallback func);
    VersionCheckCallback versionCheckCallback();
    void beforeRestartCallback(BeforeRestartCallback func);
    BeforeRestartCallback beforeRestartCallback();
    void functionPropertyCallback(FunctionPropertyCallback func);
    FunctionPropertyCallback functionPropertyCallback();
    void functionPropertyStateCallback(FunctionPropertyCallback func);
    FunctionPropertyCallback functionPropertyStateCallback();

  protected:
    virtual ApplicationLayer& applicationLayer() = 0;
    virtual InterfaceObject* getInterfaceObject(uint8_t idx) = 0;
    virtual InterfaceObject* getInterfaceObject(ObjectType objectType, uint16_t objectInstance) = 0;

    void memoryWriteIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t number,
                               uint16_t memoryAddress, uint8_t* data) override;
    void memoryReadIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t number,
                              uint16_t memoryAddress) override;
    void memoryReadIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t number,
                              uint16_t memoryAddress, uint8_t * data);
    void memoryRouterWriteIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t number,
                              uint16_t memoryAddress, uint8_t *data);
    void memoryRouterReadIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t number,
                                    uint16_t memoryAddress, uint8_t *data);
    void memoryRoutingTableWriteIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t number,
                              uint16_t memoryAddress, uint8_t *data);
    void memoryRoutingTableReadIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t number, uint16_t memoryAddress, uint8_t *data);
    void memoryRoutingTableReadIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t number, uint16_t memoryAddress);
    //
    void memoryExtWriteIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t number,
                                  uint32_t memoryAddress, uint8_t* data) override;
    void memoryExtReadIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t number,
                                 uint32_t memoryAddress) override;
    void deviceDescriptorReadIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t descriptorType) override;
    void restartRequestIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, RestartType restartType, EraseCode eraseCode, uint8_t channel) override;
    void authorizeIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint32_t key) override;
    void userMemoryReadIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t number, uint32_t memoryAddress) override;
    void userMemoryWriteIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t number,
                                   uint32_t memoryAddress, uint8_t* memoryData) override;
    void propertyDescriptionReadIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t objectIndex,
                                           uint8_t propertyId, uint8_t propertyIndex) override;
    void propertyExtDescriptionReadIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl,
                                                   uint16_t objectType, uint16_t objectInstance, uint16_t propertyId, uint8_t descriptionType, uint16_t propertyIndex) override;
    void propertyValueWriteIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t objectIndex,
                                      uint8_t propertyId, uint8_t numberOfElements, uint16_t startIndex, uint8_t* data, uint8_t length) override;
    void propertyValueExtWriteIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, ObjectType objectType, uint8_t objectInstance,
                                         uint8_t propertyId, uint8_t numberOfElements, uint16_t startIndex, uint8_t* data, uint8_t length, bool confirmed);
    void propertyValueReadIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t objectIndex,
                                     uint8_t propertyId, uint8_t numberOfElements, uint16_t startIndex) override;
    void propertyValueExtReadIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, ObjectType objectType, uint8_t objectInstance,
                                        uint8_t propertyId, uint8_t numberOfElements, uint16_t startIndex) override;
    void functionPropertyCommandIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t objectIndex,
                                           uint8_t propertyId, uint8_t* data, uint8_t length) override;
    void functionPropertyStateIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t objectIndex,
                                         uint8_t propertyId, uint8_t* data, uint8_t length) override;
#ifdef OPENKNX_FTC
    void functionPropertyStateResponseIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, uint8_t objectIndex,
                                                 uint8_t propertyId, uint8_t* data, uint8_t length) override;
    // A DeviceDescriptor_Response we asked for came back (scan): hand the responder's PA + mask to the client.
    void deviceDescriptorReadAppLayerConfirm(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl,
                                             uint8_t descriptortype, uint8_t* deviceDescriptor) override;
    // A PropertyValue_Response we asked for came back (device info): hand PA + object/property + data on.
    void propertyValueReadAppLayerConfirm(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl,
                                          uint8_t objectIndex, uint8_t propertyId, uint8_t numberOfElements, uint16_t startIndex,
                                          uint8_t* data, uint8_t length) override;
    // A MemoryResponse we asked for came back (GA/association table walk): hand PA + address + bytes on.
    void memoryReadAppLayerConfirm(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl,
                                   uint8_t number, uint16_t memoryAddress, uint8_t* data) override;
#endif
    void functionPropertyExtCommandIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, ObjectType objectType, uint8_t objectInstance,
                                                      uint8_t propertyId, uint8_t* data, uint8_t length) override;
    void functionPropertyExtStateIndication(Priority priority, HopCountType hopType, uint16_t asap, const SecurityControl &secCtrl, ObjectType objectType, uint8_t objectInstance,
                                                    uint8_t propertyId, uint8_t* data, uint8_t length) override;
    void individualAddressReadIndication(HopCountType hopType, const SecurityControl &secCtrl) override;
    void individualAddressWriteIndication(HopCountType hopType, const SecurityControl &secCtrl, uint16_t newaddress) override;
    void individualAddressSerialNumberWriteIndication(Priority priority, HopCountType hopType, const SecurityControl &secCtrl, uint16_t newIndividualAddress,
                                                      uint8_t* knxSerialNumber) override;
    void individualAddressSerialNumberReadIndication(Priority priority, HopCountType hopType, const SecurityControl &secCtrl, uint8_t* knxSerialNumber) override;
    void systemNetworkParameterReadIndication(Priority priority, HopCountType hopType, const SecurityControl &secCtrl, uint16_t objectType,
                                              uint16_t propertyId, uint8_t* testInfo, uint16_t testinfoLength) override;
    void systemNetworkParameterReadLocalConfirm(Priority priority, HopCountType hopType, const SecurityControl &secCtrl, uint16_t objectType,
                                                uint16_t propertyId, uint8_t* testInfo, uint16_t testInfoLength, bool status) override;
    void connectConfirm(uint16_t tsap) override;

    void nextRestartState();
    virtual void doMasterReset(EraseCode eraseCode, uint8_t channel);

    enum RestartState
    {
        Idle,
        Connecting,
        Connected,
        Restarted
    };

    Memory _memory;
    DeviceObject _deviceObj;
    ApplicationProgramObject _appProgram;
    Platform& _platform;
    RestartState _restartState = Idle;
#ifdef OPENKNX_FTC
    void (*_ftcResponseCb)(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, uint8_t* data, uint8_t length) = nullptr;
    void (*_ftcDdCb)(uint16_t pa, uint8_t descriptorType, const uint8_t* data) = nullptr;
    void (*_ftcPropCb)(uint16_t pa, uint8_t objectIndex, uint8_t propertyId, const uint8_t* data, uint8_t length) = nullptr;
    void (*_ftcMemCb)(uint16_t pa, uint16_t addr, const uint8_t* data, uint8_t len) = nullptr;
#endif
    SecurityControl _restartSecurity;
    uint32_t _restartDelay = 0;
    BeforeRestartCallback _beforeRestart = 0;
    FunctionPropertyCallback _functionProperty = 0;
    FunctionPropertyCallback _functionPropertyState = 0;
};

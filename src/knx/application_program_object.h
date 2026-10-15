#pragma once

#include "table_object.h"
#include "bits.h"

class ApplicationProgramObject : public TableObject
{
  public:
    /**
     * @brief Application program with its load state machine (03_05_01 4.21).
     * @param objectType OT_APPLICATION_PROG for Application Program 1, OT_INTERFACE_PROG for Application Program 2.
     */
    ApplicationProgramObject(Memory& memory, ObjectType objectType = OT_APPLICATION_PROG);
    uint8_t* save(uint8_t* buffer) override;
    const uint8_t* restore(const uint8_t* buffer) override;
    uint16_t saveSize() override;
    uint8_t* data(uint32_t addr);
    uint8_t getByte(uint32_t addr);
    uint16_t getWord(uint32_t addr);
    uint32_t getInt(uint32_t addr);
    double getFloat(uint32_t addr, ParameterFloatEncodings encoding);
    void beforeStateChange(LoadState& newState) override;
    void loadEvent(const uint8_t* data) override;
    bool loadsWithoutMemory() const override { return _loadsWithoutMemory; }

    /** @brief Run state Terminated, i.e. the executable part is stopped (03_05_01 4.24 p.298). */
    bool applicationStopped() const { return _applicationStopped; }
    void applicationStopped(bool value) { _applicationStopped = value; }

    /** @brief Set by a BAU that really holds its executable part, and for Application Program 2; others refuse Restart and Stop.
     *  @details Table 96 p.299 makes the events optional, so a BAU without a gate must not report a state it never
     *           enters. 4.21.2.6 p.290 requires them for Application Program 2; this stack runs nothing of it, so there
     *           the state is only reported. */
    bool runControlWritable() const { return _runControlWritable; }
    void runControlWritable(bool value) { _runControlWritable = value; }

  private:
    // Application Program 2 may complete without memory: the ETS "par" procedure of MV-07B0 loads LSM 5 without a segment.
    bool _loadsWithoutMemory = false;
    // Written from the main loop, read from the receive context; byte-atomic, so volatile is enough.
    volatile bool _applicationStopped = false;
    bool _runControlWritable = false;
};

#include "application_program_object.h"
#include "bits.h"
#include "data_property.h"
#include "callback_property.h"
#include "dptconvert.h"
#include <cstring>

// 0100h is a product convention, not a spec constant -- only the filter table at 0200h-21FFh is fixed
// (03_05_01 5.2.1.2 p.311). Overridable so parameters can be placed above the filter table.
#ifndef KNX_091A_APPLICATION_PROGRAM_ADDR
    #define KNX_091A_APPLICATION_PROGRAM_ADDR 0x0100
#endif
#ifndef KNX_091A_APPLICATION_PROGRAM_SIZE
    #define KNX_091A_APPLICATION_PROGRAM_SIZE 0x0100
#endif

ApplicationProgramObject::ApplicationProgramObject(Memory& memory)
#if MASK_VERSION == 0x091A
    : TableObject(memory, KNX_091A_APPLICATION_PROGRAM_ADDR, KNX_091A_APPLICATION_PROGRAM_SIZE)
#else
    : TableObject(memory)
#endif
{
    Property* properties[] =
    {
        new DataProperty(PID_OBJECT_TYPE, false, PDT_UNSIGNED_INT, 1, ReadLv3 | WriteLv0, (uint16_t)OT_APPLICATION_PROG),
        new DataProperty(PID_PROG_VERSION, true, PDT_GENERIC_05, 1, ReadLv3 | WriteLv3),
        new CallbackProperty<ApplicationProgramObject>(this, PID_PEI_TYPE, false, PDT_UNSIGNED_CHAR, 1, ReadLv3 | WriteLv0,
            [](ApplicationProgramObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t {
                if(start == 0)
                {
                    uint16_t currentNoOfElements = 1;
                    pushWord(currentNoOfElements, data);
                    return 1;
                }

                data[0] = 0;
                return 1;
            }),
        // Run state machine, 03_05_01 4.24 p.298. Writable rather than the read-only minimum, so a
        // device can be silenced "for diagnostic purposes" (p.299) without unloading its configuration.
        new CallbackProperty<ApplicationProgramObject>(this, PID_RUN_STATE_CONTROL, true, PDT_CONTROL, 1, ReadLv3 | WriteLv3,
            [](ApplicationProgramObject* io, uint16_t start, uint8_t count, uint8_t* data) -> uint8_t {
                if (start == 0)
                {
                    uint16_t currentNoOfElements = 1;
                    pushWord(currentNoOfElements, data);
                    return 1;
                }

                // Table 95 p.299. Terminated first and independent of the load state: Table 97 p.301
                // has Stop reach it from Halted, and 08_TSSI 2.2.4 p.7 requires 03 on an unloaded object.
                if (io->applicationStopped()) { data[0] = 3; return 1; }
                if (io->loadState() != LS_LOADED) { data[0] = 0; return 1; }
                data[0] = 1;
                return 1;
            },
            [](ApplicationProgramObject* io, uint16_t start, uint8_t count, const uint8_t* data) -> uint8_t {
                // Redundant with CallbackProperty::write; kept because data[0] is read below.
                if (start == 0 || count == 0)
                    return 0;

                // Table 96 p.299. Only the state-changing events are gated: 08_TSSI 2.2.1 p.5 sends FFh
                // and still requires nr_of_elem 1 with the current state.
                switch (data[0])
                {
                    case 1: // Restart
                        if (!io->runControlWritable()) return 0;
                        io->applicationStopped(false);
                        return 1;
                    case 2: // Stop
                        if (!io->runControlWritable()) return 0;
                        io->applicationStopped(true);
                        return 1;
                    case 0: return 1;                                  // NOP
                    default: return 1;                                 // ignored, state unchanged
                }
            })
    };

    TableObject::initializeProperties(sizeof(properties), properties);
}

uint8_t* ApplicationProgramObject::save(uint8_t* buffer)
{
    uint8_t programVersion[5];
    property(PID_PROG_VERSION)->read(programVersion);
    buffer = pushByteArray(programVersion, 5, buffer);

    return TableObject::save(buffer);
}

const uint8_t* ApplicationProgramObject::restore(const uint8_t* buffer)
{
    uint8_t programVersion[5];
    buffer = popByteArray(programVersion, 5, buffer);
    property(PID_PROG_VERSION)->write(programVersion);

    return TableObject::restore(buffer);
}

uint16_t ApplicationProgramObject::saveSize()
{
    return TableObject::saveSize() + 5; // sizeof(programVersion)
}

uint8_t * ApplicationProgramObject::data(uint32_t addr)
{
    return TableObject::data() + addr;
}

uint8_t ApplicationProgramObject::getByte(uint32_t addr)
{
    return *(TableObject::data() + addr);
}

uint16_t ApplicationProgramObject::getWord(uint32_t addr)
{
    return ::getWord(TableObject::data() + addr);
}

uint32_t ApplicationProgramObject::getInt(uint32_t addr)
{
    return ::getInt(TableObject::data() + addr);
}

// Table 97 p.301: Unload leaves Terminated for Halted. On an already unloaded object no load state
// changes, so beforeStateChange() alone would let Terminated stand.
void ApplicationProgramObject::loadEvent(const uint8_t* data)
{
    if (data[0] == LE_UNLOAD)
        _applicationStopped = false;

    TableObject::loadEvent(data);
}

// 08_TSSI 2.5.5 p.26: Terminated plus an unload reads back as Halted. Without this the state would
// survive a download and hold group communication for good.
void ApplicationProgramObject::beforeStateChange(LoadState& newState)
{
    TableObject::beforeStateChange(newState);

    if (newState != LS_LOADED)
        _applicationStopped = false;
}

double ApplicationProgramObject::getFloat(uint32_t addr, ParameterFloatEncodings encoding)
{
    switch (encoding)
    {
        case Float_Enc_DPT9:
            return float16FromPayload(TableObject::data() + addr, 0);
            break;
        case Float_Enc_IEEE754Single:
            return float32FromPayload(TableObject::data() + addr, 0);
            break;
        case Float_Enc_IEEE754Double:
            return float64FromPayload(TableObject::data() + addr, 0);
            break;
        default:
            return 0;
            break;
    }
}

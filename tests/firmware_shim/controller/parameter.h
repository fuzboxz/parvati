// Desktop shim: controller/parameter.h — minimal host replacement
// (tests/firmware_parity_test).
//
// The real parameter manager (ambika_reference/controller/parameter.{h,cc})
// maps MIDI CC / NRPN addresses onto parameter IDs with PROGMEM string tables
// and pulls the UI layer. The parity oracle's stimulus scripts use only the
// controllers the firmware handles EXPLICITLY before any parameter mapping
// (CC1/2/4/64 and the all-notes-off messages), so a stub manager that maps
// NOTHING (every lookup returns 0xff == "no parameter") preserves those code
// paths exactly while avoiding the AVR/UI dependency closure.
//
// If a future script needs NRPN/CC parameter-editing parity, compile the real
// parameter.cc with the pgmspace shim instead of extending this stub.
#pragma once

#include "avrlib/base.h"

namespace ambika
{

enum
{
    PARAMETER_LEVEL_GLOBAL,
    PARAMETER_LEVEL_MULTI,
    PARAMETER_LEVEL_PART,
};

struct Parameter
{
    uint8_t min;
    uint8_t max;
    uint8_t num_instances;
    uint8_t midi_cc;
    uint8_t stride;
    uint8_t level;
    uint16_t offset;

    uint8_t Clamp (uint8_t value) const
    {
        return value < min ? min : (value > max ? max : value);
    }
    uint8_t Increment (uint8_t value, int8_t amount) const
    {
        int v = static_cast<int> (value) + amount;
        return static_cast<uint8_t> (v < min ? min : (v > max ? max : v));
    }
    uint8_t Scale (uint8_t value) const { return value; }
    uint8_t RandomValue() const { return min; }
};

class ParameterManager
{
public:
    ParameterManager() {}

    static uint8_t AddressToParameterId (uint16_t) { return 0xff; }
    static uint8_t ControlChangeToParameterId (uint8_t) { return 0xff; }
    static const Parameter& parameter (uint8_t)
    {
        static Parameter p = { 0, 255, 1, 0, 0, PARAMETER_LEVEL_PART, 0 };
        return p;
    }
    static void SetValue (const Parameter&, uint8_t, uint8_t, uint8_t, uint8_t) {}

private:
    ParameterManager (const ParameterManager&);
    void operator= (const ParameterManager&);
};

extern ParameterManager parameter_manager;

}  // namespace ambika

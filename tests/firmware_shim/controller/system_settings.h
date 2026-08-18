// Desktop shim: controller/system_settings.h — minimal host replacement
// (tests/firmware_parity_test). The firmware's system settings gate NRPN/CC
// reception; the oracle runs with reception fully enabled (midi_in_mask 0),
// matching the factory default. Only the surface part.cc touches is kept.
#pragma once

#include "avrlib/base.h"

namespace ambika
{

struct SystemSettingsData
{
    uint8_t midi_in_mask;
    uint8_t midi_out_mode;
    uint8_t show_help;
    uint8_t snap;
    uint8_t autobackup;
    uint8_t voicecard_leds;
    uint8_t swap_leds_colors;
    uint8_t padding[8];
    uint8_t checksum;
};

class SystemSettings
{
public:
    SystemSettings() {}

    static inline uint8_t rx_sysex()           { return !(data_.midi_in_mask & 1); }
    static inline uint8_t rx_program_change()   { return !(data_.midi_in_mask & 2); }
    static inline uint8_t rx_nrpn()             { return !(data_.midi_in_mask & 4); }
    static inline uint8_t rx_cc()               { return !(data_.midi_in_mask & 8); }

    static inline SystemSettingsData* mutable_data() { return &data_; }

private:
    static SystemSettingsData data_;
    SystemSettings (const SystemSettings&);
    void operator= (const SystemSettings&);
};

extern SystemSettings system_settings;

}  // namespace ambika

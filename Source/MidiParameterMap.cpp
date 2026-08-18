// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See MidiParameterMap.h.

#include "MidiParameterMap.h"

#include <algorithm>
#include <cstring>

namespace
{
// ---- Parameter levels (parameter.h:61-65) ----
enum : uint8_t
{
    LEVEL_PATCH  = 0,
    LEVEL_PART   = 1,
    LEVEL_MULTI  = 2,
    LEVEL_SYSTEM = 3,
    LEVEL_UI     = 4
};

// ---- Standard MIDI NRPN controller numbers (midi/midi.h:30-43) ----
constexpr int kDataEntryMsb   = 0x06;   // 6
constexpr int kDataEntryLsb   = 0x26;   // 38
constexpr int kDataIncrement  = 0x60;   // 96
constexpr int kDataDecrement  = 0x61;   // 97
constexpr int kNrpnMsb        = 0x63;   // 99
constexpr int kNrpnLsb        = 0x62;   // 98

constexpr uint8_t kUnmapped    = 0xff;
constexpr uint8_t kReservedNrpn = 0xfe;  // NRPN map "reserved" (part.cc treats as unmapped)

// ============================================================================
// Ported verbatim from ambika_reference/controller/parameter.cc:275-289.
// CC number (0..127) -> firmware parameter index (0xff = unmapped).
// ============================================================================
constexpr uint8_t midi_cc_map[128] = {
    // 0-15
    255, 255, 255,  22, 255,  48, 255,  42,
    255,  23, 255, 255,  14,  15,   2,   3,
    // 16-31
      0,   1,   4,   5,   6,   7,   8,   9,
     10,  11,  12,  13,  18,  19,  20,  21,
    // 32-47
    255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255,  29,  30,  31,  32,
    // 48-63
     33, 255, 255, 255,  29,  30,  31, 255,
    255, 255, 255, 255,  29,  30,  31, 255,
    // 64-79
    255, 255, 255, 255,  47, 255,  27,  17,
     28,  25,  16,  26, 255, 255,  27, 255,
    // 80-95
     28,  25, 255,  26, 255, 255,  27, 255,
     28,  25, 255,  26, 255, 255,  44,  45,
    // 96-111
    255, 255, 255, 255, 255, 255,  49,  50,
     51,  52,  53,  57, 255, 255, 255, 255,
    // 112-127
    255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255
};

// ============================================================================
// Ported verbatim from ambika_reference/controller/parameter.cc:302-340.
// NRPN address (0..255) -> firmware parameter index (0xff=unmapped, 0xfe=reserved).
// ============================================================================
constexpr uint8_t midi_nrpn_map[256] = {
    // 0-15
      0,   1,   2,   3,   4,   5,   6,   7,
      8,   9,  10,  11,  12,  13,  14,  15,
    // 16-31
     16,  17,  18,  19,  20,  21,  22,  23,
     25,  26,  27,  28,  31,  30, 255,  29,
    // 32-47
     25,  26,  27,  28,  31,  30, 255,  29,
     25,  26,  27,  28,  31,  30, 255,  29,
    // 48-63
     33,  32,  35,  36,  37,  35,  36,  37,
     35,  36,  37,  35,  36,  37,  35,  36,
    // 64-79
     37,  35,  36,  37,  35,  36,  37,  35,
     36,  37,  35,  36,  37,  35,  36,  37,
    // 80-95
     35,  36,  37,  35,  36,  37,  35,  36,
     37,  35,  36,  37,  39,  40,  41,  39,
    // 96-111
     40,  41,  39,  40,  41,  39,  40,  41,
    255, 255, 255, 255, 255, 255, 255, 255,
    // 112-127
     42,  43,  44,  45,  46,  47,  48,  49,
     50,  51,  52,  53,  54,  55,  56,  57,
    // 128-191 (reserved)
    254, 254, 254, 254, 254, 254, 254, 254,
    254, 254, 254, 254, 254, 254, 254, 254,
    254, 254, 254, 254, 254, 254, 254, 254,
    254, 254, 254, 254, 254, 254, 254, 254,
    254, 254, 254, 254, 254, 254, 254, 254,
    254, 254, 254, 254, 254, 254, 254, 254,
    254, 254, 254, 254, 254, 254, 254, 254,
    254, 254, 254, 254, 254, 254, 254, 254,
    // 192-255 (unmapped)
    255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255
};

// ============================================================================
// Ported from ambika_reference/controller/parameter.cc:352-762 (params 0..57).
// Field order = firmware Parameter (parameter.h:68-89):
//   { level, offset, unit, min, max, num_instances, stride, indexed_by, midi_cc }
// `offset` is the byte address in the combined [Patch][PartData] space
// (== the PRM_* enum value; e.g. PRM_PART_VOLUME == 112). Name resource ids
// are dropped. unit is display-only and unused in the decode. min/max are
// ported for parity (scaling uses the Parvati descriptor range instead).
//
// kNumParameters == 73 (parameter.h:120); only indices 0..57 are patch/part
// level — the CC map never references >= 58, so those are not ported.
// ============================================================================
constexpr FirmwareParameter firmware_parameters[58] = {
    // ---- oscillators (offsets 0..7) ----
    { LEVEL_PATCH,  0, 0,   0,  37, 1, 0, 0xff, 16 },  // 0  osc1_shape
    { LEVEL_PATCH,  1, 0,   0, 127, 1, 0, 0xff, 17 },  // 1  osc1_param
    { LEVEL_PATCH,  2, 0, -24,  24, 1, 0, 0xff, 14 },  // 2  osc1_range
    { LEVEL_PATCH,  3, 0, -64,  64, 1, 0, 0xff, 15 },  // 3  osc1_detune
    { LEVEL_PATCH,  4, 0,   0,  37, 1, 0, 0xff, 18 },  // 4  osc2_shape
    { LEVEL_PATCH,  5, 0,   0, 127, 1, 0, 0xff, 19 },  // 5  osc2_param
    { LEVEL_PATCH,  6, 0, -24,  24, 1, 0, 0xff, 20 },  // 6  osc2_range
    { LEVEL_PATCH,  7, 0, -64,  64, 1, 0, 0xff, 21 },  // 7  osc2_detune
    // ---- mixer (offsets 8..15) ----
    { LEVEL_PATCH,  8, 0,   0,  63, 1, 0, 0xff, 22 },  // 8  mix_balance
    { LEVEL_PATCH,  9, 0,   0,   5, 1, 0, 0xff, 23 },  // 9  mix_op
    { LEVEL_PATCH, 10, 0,   0,  63, 1, 0, 0xff, 24 },  // 10 mix_param
    { LEVEL_PATCH, 11, 0,   0,  10, 1, 0, 0xff, 25 },  // 11 mix_sub_shape
    { LEVEL_PATCH, 12, 0,   0,  63, 1, 0, 0xff, 26 },  // 12 mix_sub
    { LEVEL_PATCH, 13, 0,   0,  63, 1, 0, 0xff, 27 },  // 13 mix_noise
    { LEVEL_PATCH, 14, 0,   0,  63, 1, 0, 0xff, 12 },  // 14 mix_fuzz
    { LEVEL_PATCH, 15, 0,   0,  31, 1, 0, 0xff, 13 },  // 15 mix_crush
    // ---- filters (offsets 16..23) ----
    { LEVEL_PATCH, 16, 0,   0, 127, 1, 0, 0xff, 74 },  // 16 filter1_cutoff
    { LEVEL_PATCH, 17, 0,   0,  63, 1, 0, 0xff, 71 },  // 17 filter1_reso
    { LEVEL_PATCH, 18, 0,   0,   2, 1, 0, 0xff, 28 },  // 18 filter1_mode
    { LEVEL_PATCH, 19, 0,   0, 127, 1, 0, 0xff, 29 },  // 19 filter2_cutoff
    { LEVEL_PATCH, 20, 0,   0,  63, 1, 0, 0xff, 30 },  // 20 filter2_reso
    { LEVEL_PATCH, 21, 0,   0,   2, 1, 0, 0xff, 31 },  // 21 filter2_mode
    { LEVEL_PATCH, 22, 0,   0,  63, 1, 0, 0xff,  3 },  // 22 filter_env
    { LEVEL_PATCH, 23, 0,   0,  63, 1, 0, 0xff,  9 },  // 23 filter_lfo
    // ---- env+lfo unit selector (UI level; never CC/NRPN-addressed) ----
    { LEVEL_UI,    24, 0,   0,   2, 1, 0, 0xff, 0xff },// 24 active env-lfo (UI)
    // ---- env+lfo params (3 instances, stride 8: bytes 24/32/40, 25/33/41, ...) ----
    { LEVEL_PATCH, 24, 0,   0, 127, 3, 8, 0xff, 73 },  // 25 env_attack  (offset 24)
    { LEVEL_PATCH, 25, 0,   0, 127, 3, 8, 0xff, 75 },  // 26 env_decay   (offset 25)
    { LEVEL_PATCH, 26, 0,   0, 127, 3, 8, 0xff, 70 },  // 27 env_sustain (offset 26)
    { LEVEL_PATCH, 27, 0,   0, 127, 3, 8, 0xff, 72 },  // 28 env_release (offset 27)
    { LEVEL_PATCH, 31, 0,   0,   2, 3, 8, 0xff, 44 },  // 29 lfo_sync    (offset 31)
    { LEVEL_PATCH, 29, 0,   0, 142, 3, 8, 0xff, 45 },  // 30 lfo_rate    (offset 29)
    { LEVEL_PATCH, 28, 0,   0,  19, 3, 8, 0xff, 46 },  // 31 lfo_shape   (offset 28)
    // ---- voice LFO (offsets 49/48) ----
    { LEVEL_PATCH, 49, 0,   0, 127, 1, 0, 0xff, 47 },  // 32 voice_lfo_rate  (offset 49)
    { LEVEL_PATCH, 48, 0,   0,   3, 1, 0, 0xff, 48 },  // 33 voice_lfo_shape (offset 48)
    // ---- mod-matrix selector (UI) ----
    { LEVEL_UI,    50, 0,   0,  13, 1, 0, 0xff, 0xff },// 34 active modulation (UI)
    // ---- modulation matrix (14 instances, stride 3: bytes 50/53/..., 51/..., 52/...) ----
    { LEVEL_PATCH, 50, 0,   0,  24, 14, 3, 0xff, 0xff },// 35 mod_source
    { LEVEL_PATCH, 51, 0,   0,  18, 14, 3, 0xff, 0xff },// 36 mod_dest
    { LEVEL_PATCH, 52, 0, -63,  63, 14, 3, 0xff, 0xff },// 37 mod_amount
    // ---- modifier selector (UI) ----
    { LEVEL_UI,    92, 0,   0,   3, 1, 0, 0xff, 0xff },// 38 active modifier (UI)
    // ---- modifiers (4 instances, stride 3: bytes 92/95/..., 93/..., 94/...) ----
    { LEVEL_PATCH, 92, 0,   0,  30, 4, 3, 0xff, 0xff },// 39 mod_operand1
    { LEVEL_PATCH, 93, 0,   0,  30, 4, 3, 0xff, 0xff },// 40 mod_operand2
    { LEVEL_PATCH, 94, 0,   0,  10, 4, 3, 0xff, 0xff },// 41 mod_operator
    // ---- part editor (offsets 112..118 = PartData bytes 0..6) ----
    { LEVEL_PART, 112, 0,   0, 127, 1, 0, 0xff,  7 },  // 42 part_volume   (PartData 0)
    { LEVEL_PART, 113, 0,  -2,   2, 1, 0, 0xff, 0xff },// 43 part_octave   (PartData 1)  [not exposed]
    { LEVEL_PART, 114, 0,-127, 127, 1, 0, 0xff, 94 },  // 44 part_tuning   (PartData 2)  [not exposed]
    { LEVEL_PART, 115, 0,   0,  40, 1, 0, 0xff, 95 },  // 45 part_spread   (PartData 3)  [not exposed]
    { LEVEL_PART, 116, 0,   0,  31, 1, 0, 0xff, 0xff },// 46 part_raga     (PartData 4)  [not exposed]
    { LEVEL_PART, 117, 0,   0,   1, 1, 0, 0xff, 68 },  // 47 part_legato   (PartData 5)
    { LEVEL_PART, 118, 0,   0,  63, 1, 0, 0xff,  5 },  // 48 part_portamento(PartData 6)
    // ---- arpeggiator (PartData 7..11 = addresses 119..123) ----
    { LEVEL_PART, 119, 0,   0,   2, 1, 0, 0xff, 102 }, // 49 arp_mode
    { LEVEL_PART, 120, 0,   0,   5, 1, 0, 0xff, 103 }, // 50 arp_direction
    { LEVEL_PART, 121, 0,   1,   4, 1, 0, 0xff, 104 }, // 51 arp_octave
    { LEVEL_PART, 122, 0,   0,  21, 1, 0, 0xff, 105 }, // 52 arp_pattern
    { LEVEL_PART, 123, 0,   0,  14, 1, 0, 0xff, 106 }, // 53 arp_resolution
    // ---- sequencer lengths (PartData 12..14 = addresses 124..126) ----
    { LEVEL_PART, 124, 0,   1,  32, 1, 0, 0xff, 0xff },// 54 seq_length_1
    { LEVEL_PART, 125, 0,   1,  32, 1, 0, 0xff, 0xff },// 55 seq_length_2
    { LEVEL_PART, 126, 0,   1,  32, 1, 0, 0xff, 0xff },// 56 seq_length_3
    // ---- polyphony (PartData 15 = address 127; not exposed in Parvati) ----
    { LEVEL_PART, 127, 0,   0,   4, 1, 0, 0xff, 107 }  // 57 polyphony_mode [not exposed]
};
static_assert (sizeof (firmware_parameters) / sizeof (firmware_parameters[0]) == 58,
               "58 patch/part level parameters (0..57)");

// ---- helpers: clamp a value to the descriptor's APVTS range ----
int descriptorMin (const PatchParamDescriptor& d)
{
    return d.choices != nullptr ? 0 : d.minValue;
}
int descriptorMax (const PatchParamDescriptor& d)
{
    return d.choices != nullptr ? d.choices->size() - 1 : d.maxValue;
}
}  // namespace

// ============================================================================
void MidiParameterMap::initialise (const std::vector<PatchParamDescriptor>& descriptors,
                                   ValueSetter setter, ValueGetter getter)
{
    setter_ = std::move (setter);
    getter_ = std::move (getter);

    for (int& v : addressToDescriptorIndex_)
        v = -1;

    for (size_t i = 0; i < descriptors.size(); ++i)
    {
        const auto& d = descriptors[i];
        if (d.isOption)
            continue;  // part_select / vca_curve / filter_card: no patch byte

        int address = -1;
        if (d.isArp)
        {
            // Arp params have byteOffset=-1; map explicitly by ID (PartData 7..11).
            if      (d.paramID == "arp_mode")       address = 119;
            else if (d.paramID == "arp_direction")  address = 120;
            else if (d.paramID == "arp_octave")     address = 121;
            else if (d.paramID == "arp_pattern")    address = 122;
            else if (d.paramID == "arp_resolution") address = 123;
        }
        else if ((d.isPart || d.isSequencer) && d.byteOffset >= 0)
        {
            address = 112 + d.byteOffset;   // PartData byte -> firmware address
        }
        else if (! d.isPart && ! d.isArp && ! d.isSequencer)
        {
            address = d.byteOffset;          // patch byte
        }

        if (address >= 0 && address < 256)
            if (addressToDescriptorIndex_[address] < 0)   // first wins (addresses unique)
                addressToDescriptorIndex_[address] = static_cast<int> (i);
    }
}

const PatchParamDescriptor* MidiParameterMap::descriptorForAddress (int address) const
{
    if (address < 0 || address >= 256)
        return nullptr;
    const int idx = addressToDescriptorIndex_[address];
    if (idx < 0)
        return nullptr;
    return &getPatchParamDescriptors()[static_cast<size_t> (idx)];
}

// ============================================================================
void MidiParameterMap::applyValue (int address, int midiValue, bool scaled)
{
    const PatchParamDescriptor* d = descriptorForAddress (address);
    if (d == nullptr || ! setter_)
        return;  // address not exposed in Parvati (e.g. octave/tuning/polyphony)

    const int lo = descriptorMin (*d);
    const int hi = descriptorMax (*d);

    int value;
    if (scaled)
    {
        // CC path: 7-bit scaled to range (firmware parameter.Scale, parameter.cc:49-58).
        //   range = max-min+1; scaled = ((range * (v<<1)) >> 8) + min; clamp.
        const int range = hi - lo + 1;
        value = ((range * (midiValue << 1)) >> 8) + lo;
    }
    else
    {
        // NRPN path: the data value IS the direct parameter value (firmware
        // parameter.Clamp, part.cc:419) — just clamp to range. INT8 parameters
        // (those with a negative APVTS range: osc range/detune, mod amounts)
        // are TWO'S COMPLEMENT — the firmware's Clamp reads the byte as int8_t
        // (parameter.cc UNIT_INT8) — so a byte >= 128 is NEGATIVE. Reinterpret
        // BEFORE clamping or the clamp saturates it to +max and negative values
        // are unreachable over NRPN (an Ambika editor cannot set any detune/
        // mod amount below zero).
        value = midiValue;
        if (lo < 0 && midiValue > 127)
            value = static_cast<int> (static_cast<int8_t> (midiValue));
    }
    value = std::max (lo, std::min (hi, value));

    setter_ (d->paramID.c_str(), static_cast<float> (value));
}

void MidiParameterMap::nudgeValue (int address, int delta)
{
    const PatchParamDescriptor* d = descriptorForAddress (address);
    if (d == nullptr || ! setter_ || ! getter_)
        return;

    const int lo = descriptorMin (*d);
    const int hi = descriptorMax (*d);
    int current = static_cast<int> (getter_ (d->paramID.c_str()));
    current = std::max (lo, std::min (hi, current + delta));
    setter_ (d->paramID.c_str(), static_cast<float> (current));
}

// ============================================================================
void MidiParameterMap::handleBuffer (const juce::MidiBuffer& midi)
{
    for (const auto meta : midi)
    {
        const auto msg = meta.getMessage();
        if (! msg.isController())
            continue;

        const int controller = msg.getControllerNumber();
        const int value      = msg.getControllerValue();

        if (controller == kNrpnMsb)         // CC99: NRPN address MSB (flag)
        {
            nrpnAddressMsbFlag_ = value ? 128 : 0;
            dataEntryMsbFlag_   = 0;
        }
        else if (controller == kNrpnLsb)     // CC98: NRPN address LSB
        {
            nrpnAddress_        = value | nrpnAddressMsbFlag_;
            nrpnAddressMsbFlag_ = 0;
            dataEntryMsbFlag_   = 0;
        }
        else if (controller == kDataEntryMsb) // CC6: data-entry MSB (flag)
        {
            dataEntryMsbFlag_ = value ? 128 : 0;
        }
        else if (controller == kDataEntryLsb) // CC38: data-entry LSB -> apply NRPN
        {
            const int address = nrpnAddress_;
            if (address >= 0 && address < 256
                && midi_nrpn_map[address] != kUnmapped
                && midi_nrpn_map[address] != kReservedNrpn)
            {
                applyValue (address, value | dataEntryMsbFlag_, false /*direct*/);
            }
        }
        else if (controller == kDataIncrement)  // CC96
        {
            const int address = nrpnAddress_;
            if (address >= 0 && address < 256
                && midi_nrpn_map[address] != kUnmapped
                && midi_nrpn_map[address] != kReservedNrpn)
                nudgeValue (address, +1);
        }
        else if (controller == kDataDecrement)  // CC97
        {
            const int address = nrpnAddress_;
            if (address >= 0 && address < 256
                && midi_nrpn_map[address] != kUnmapped
                && midi_nrpn_map[address] != kReservedNrpn)
                nudgeValue (address, -1);
        }
        else
        {
            // Regular CC -> parameter (firmware part.cc:433-458).
            const int paramIndex = midi_cc_map[controller];
            if (paramIndex == kUnmapped || paramIndex >= 58)
                continue;
            const auto& p = firmware_parameters[paramIndex];

            // Instance loop (part.cc:436-444): walk the CC group to find which
            // instance this CC selects.
            int c = controller;
            int instance = 0;
            for (instance = 0; instance < p.num_instances; ++instance)
            {
                if (p.midi_cc == c)
                    break;
                c -= p.stride;
            }
            if (instance >= p.num_instances)
                continue;  // CC not in this param's group (shouldn't happen)

            const int address = p.offset + p.stride * instance;
            applyValue (address, value, true /*scaled*/);
        }
    }
}

// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.
//
// MidiParameterMap — hardware-parity MIDI CC/NRPN → APVTS parameter control.
//
// The Ambika hardware maps incoming MIDI CC (performance keyboards/DAWs) and
// NRPN (editors/librarians) onto its patch/part parameters. This module ports
// that mapping (spec F.4; firmware `controller/parameter.cc` + `controller/
// part.cc::OnControlChange`) so the plugin responds to the same controllers.
//
// ADDRESS MODEL (verified against firmware):
//   CC/NRPN address is a FLAT BYTE INDEX into the combined
//   [Patch (112 bytes)][PartData] space. Part::SetValue(address) writes
//   patch_[address] (address<112) or part_[address-112] (address>=112).
//   Hellcat's structs are byte-identical to firmware, so every decoded
//   address lands on a PatchParamDescriptor byteOffset.
//
//   - NRPN: address == NRPN LSB (| MSB flag). Value = the DIRECT parameter
//     value, clamped to range (firmware uses parameter.Clamp, part.cc:419).
//   - CC:   address = param.offset + param.stride * instance (the firmware
//     instance loop, part.cc:436-444). Value = 7-bit SCALED to range
//     (firmware parameter.Scale, parameter.cc:49-58).

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <functional>
#include <vector>

#include "ParameterLayout.h"  // PatchParamDescriptor, getPatchParamDescriptors

// One ported firmware Parameter entry (controller/parameter.h:68-89).
// Field order matches firmware: level, offset, unit, min, max, num_instances,
// stride, indexed_by, midi_cc. (The firmware Parameter has no separate `id`
// field — its 2nd member IS `offset` = the PRM_* byte address.) Name resource
// ids are dropped (unused in Hellcat). `offset` is the byte address in the
// combined [Patch][PartData] space (e.g. PRM_PART_VOLUME == 112).
struct FirmwareParameter
{
    uint8_t level;         // PARAMETER_LEVEL_PATCH / PART / MULTI / UI
    uint8_t offset;        // byte address (== the PRM_* enum value)
    uint8_t unit;          // UNIT_* (display only; unused in decode)
    int16_t min_value;     // signed (e.g. -63 for INT8 amounts); 16-bit to also hold 142 (lfo_rate max)
    int16_t max_value;
    uint8_t num_instances; // instances in a stride group (env=3, mod=14, modfr=4, else 1)
    uint8_t stride;        // byte distance between instances (env=8, mod/modfr=3, else 0)
    uint8_t indexed_by;    // UI selector that picks the active instance (display only)
    uint8_t midi_cc;       // base CC for this parameter (0xff = no CC assignment)
};

class MidiParameterMap
{
public:
    // Sets a parameter's value (routes via APVTS setValueNotifyingHost so
    // parameterChanged fires and the byte reaches the current part's voices).
    using ValueSetter = std::function<void (const char* paramID, float value)>;
    // Reads a parameter's current APVTS value (for NRPN data increment/decrement).
    using ValueGetter = std::function<float (const char* paramID)>;

    // Build the address→descriptor lookup from the descriptor table. Call once
    // after the APVTS is constructed.
    void initialise (const std::vector<PatchParamDescriptor>& descriptors,
                     ValueSetter setter, ValueGetter getter);

    // Scan a MIDI buffer for CC/NRPN and apply any mapped parameter changes.
    // Does NOT consume/remove messages (notes etc. are left for the engine).
    void handleBuffer (const juce::MidiBuffer& midi);

private:
    // Returns the descriptor for a firmware byte address, or nullptr.
    const PatchParamDescriptor* descriptorForAddress (int address) const;

    // Apply a value to the parameter at `address`. scaled=false => NRPN direct
    // value (clamped); scaled=true => CC 7-bit value (Scale'd to range).
    void applyValue (int address, int midiValue, bool scaled);

    // NRPN data increment/decrement: read current, +/-1, clamp, apply (direct).
    void nudgeValue (int address, int delta);

    ValueSetter setter_;
    ValueGetter getter_;

    // address (0..255) -> index into the descriptor vector, or -1.
    int addressToDescriptorIndex_[256] {};

    // NRPN assembly state (part.cc:392-406).
    int nrpnAddress_        = 0xff;   // current NRPN address (lsb | msbFlag)
    int nrpnAddressMsbFlag_ = 0;      // 0 or 128 (set by CC99 NRPN MSB)
    int dataEntryMsbFlag_   = 0;      // 0 or 128 (set by CC6 data-entry MSB)
};

// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// ParameterLayout — the full Ambika patch parameter surface exposed through a
// JUCE AudioProcessorValueTreeState (APVTS), plus the faithful mapping from
// each APVTS parameter to its Patch/Part struct byte.
//
// Source of truth for ranges/units: ambika_reference/controller/parameter.cc
// (the `parameters[]` table) and Source/dsp/patch.h (the Patch/Part byte
// layout). Every Ambika patch parameter (oscillators, mixer, filters, the 3
// env+lfo units, voice LFO, 14 modulation routings, 4 modifiers) plus the
// in-scope Part params (volume / legato / portamento) is exposed. Changing any
// parameter writes the correct byte(s) into every active voice's Patch/Part.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dsp/patch_sanitizer.h"   // ArpSeqField + kArpSeqDomains (byte domains)

// Describes one APVTS parameter and how it maps to a Patch/Part struct byte.
struct PatchParamDescriptor
{
    std::string paramID;          // stable APVTS parameter ID
    std::string label;            // human-readable label
    int byteOffset = 0;           // byte offset into the Patch (or Part) struct
    bool isPart = false;          // true => Part struct (set_part_data), false => Patch
    bool isSigned = false;        // true => value is int8 (two's-complement byte)
    bool isArp = false;           // true => controller-side arpeggiator param (no patch byte)
    bool isOption = false;        // true => synth option (no patch byte), routed specially (e.g. VCA curve)
    bool isSequencer = false;     // true => step-sequencer param (controller PartData; routed to the engine Sequencer)
    bool isFx = false;            // true => Parvati per-part FX param (no patch byte; routed via applyFxParameter)
    const juce::StringArray* choices = nullptr;  // non-null => AudioParameterChoice (value = index)
    int minValue = 0;             // APVTS Int range (ignored for Choice)
    int maxValue = 0;
    int defaultValue = 0;         // APVTS default (Int value or Choice index)
    bool nonAutomatable = false;  // true => created with .withAutomatable(false):
                                  // a UI action, not a sound parameter (hosts
                                  // hide it from automation lanes)
};

// The complete, ordered table of all patch/part parameters.
const std::vector<PatchParamDescriptor>& getPatchParamDescriptors();

// Build the APVTS ParameterLayout from the descriptor table.
juce::AudioProcessorValueTreeState::ParameterLayout createParvatiParameterLayout();

// FxType choice-list DISPLAY labels ("None", "Diffuser", ..., "Digital Echo",
// "Room", "Spring"), one per FxType enum value in order. The stored parameter
// value is the choice INDEX (never the text), so label renames are
// serialization-safe; exposed (external linkage) so tests can pin the labels
// and the count against the enum.
juce::StringArray makeFxTypes();

// The controller-side init patch (Part::InitPatch(DEFAULT)) as raw Patch bytes
// (112). Used to seed every Part to the audible factory patch on prepare so a
// freshly-loaded plugin does not leave Parts 1..5 on the silent voicecard
// fallback. Pointer storage is static constexpr (valid for the program lifetime).
const uint8_t* getControllerInitPatchBytes();

// Convert an APVTS raw (float) value to the faithful Patch/Part byte for a
// given descriptor (clamped to range; signed values cast to int8).
uint8_t parvatiValueToPatchByte (const PatchParamDescriptor& descriptor, float rawValue);

// Inverse of parvatiValueToPatchByte: the faithful Patch/Part byte back to the
// APVTS raw (denormalized) value for a descriptor. Used when loading Ambika
// .PRO patch files (which store the raw struct bytes).
float parvatiPatchByteToValue (const PatchParamDescriptor& descriptor, uint8_t byte);

// (The arp choice lists are built inline from makeArp*() in the descriptor
// table below; no public accessors are needed.)

// ---------------------------------------------------------------------------
// Arp/seq parameter ID -> pending-config field. The ONE source for the
// paramID dispatch that used to be hand-coded in five places (the preset
// read and write paths, applyArpParameter/applySequencerParameter,
// loadPartIntoApvts, the NRPN address map). The PartData offsets and byte
// bounds come from kArpSeqDomains (patch_sanitizer.h).
// ---------------------------------------------------------------------------
struct ArpSeqParamMapEntry
{
    const char* paramID;
    ambika::dsp::ArpSeqField field;
};

constexpr ArpSeqParamMapEntry kArpSeqParamMap[] = {
    { "arp_mode",       ambika::dsp::ArpSeqField::ArpMode },
    { "arp_direction",  ambika::dsp::ArpSeqField::ArpDirection },
    { "arp_octave",     ambika::dsp::ArpSeqField::ArpOctave },
    { "arp_pattern",    ambika::dsp::ArpSeqField::ArpPattern },
    { "arp_resolution", ambika::dsp::ArpSeqField::ArpResolution },
    { "seq_length_1",   ambika::dsp::ArpSeqField::SeqLength1 },
    { "seq_length_2",   ambika::dsp::ArpSeqField::SeqLength2 },
    { "seq_length_3",   ambika::dsp::ArpSeqField::SeqLength3 },
};

// Look up an arp/seq config field by parameter ID. Returns nullopt when the
// ID names no config field (for example the seq step parameters, which ride
// their PartData byteOffset instead).
constexpr std::optional<ambika::dsp::ArpSeqField> arpSeqFieldForID (std::string_view id)
{
    for (const ArpSeqParamMapEntry& e : kArpSeqParamMap)
        if (id == e.paramID)
            return e.field;
    return std::nullopt;
}

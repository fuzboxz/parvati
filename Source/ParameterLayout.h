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

#include <string>
#include <vector>

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
    const juce::StringArray* choices = nullptr;  // non-null => AudioParameterChoice (value = index)
    int minValue = 0;             // APVTS Int range (ignored for Choice)
    int maxValue = 0;
    int defaultValue = 0;         // APVTS default (Int value or Choice index)
};

// The complete, ordered table of all patch/part parameters.
const std::vector<PatchParamDescriptor>& getPatchParamDescriptors();

// Build the APVTS ParameterLayout from the descriptor table.
juce::AudioProcessorValueTreeState::ParameterLayout createParvatiParameterLayout();

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

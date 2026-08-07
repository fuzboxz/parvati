// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// ModDestMap — a pure-data bridge between the APVTS modulation-matrix
// parameters and the synth's visible knobs. It maps a ModulationDestination
// enum value (MOD_DST_*) to the APVTS paramID of its visible knob (if any),
// resolves the reverse lookup (paramID -> destination), and aggregates the
// per-slot signed modulation amounts. Used by the modulation ring,
// hover-highlight, and drag-and-drop features.
//
// This module changes NOTHING about the patch format or the DSP: it only reads
// the APVTS parameters that already drive the engine (mod{1..14}_dest /
// mod{1..14}_amount), so Ambika-format backward compatibility is untouched.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>  // juce::String + juce::AudioProcessorValueTreeState

#include <vector>

namespace parvati::ModDestMap
{
// A ModulationDestination enum value (ambika::dsp::MOD_DST_*), or -1 for "none".
using ModDst = int;

// Returns the MOD_DST_* destination that the given knob paramID is the base
// value of (and therefore the target of), or -1 if @p paramID is not a
// modulation destination / has no visible knob.
ModDst destForParamID (const juce::String& paramID);

// Returns the APVTS paramID of the visible knob for @p dest, or an empty string
// if that destination has no base knob (VCA, envelope ADR offsets, raw
// oscillator-pitch offsets).
juce::String paramIDForDest (ModDst dest);

// Whether @p dest is backed by a visible knob and is therefore eligible as a
// modulation-ring and drag-and-drop target.
bool hasVisibleKnob (ModDst dest);

// Sums the signed amounts (-63..+63) of every slot currently routed to @p dest.
// @p excludeSlot (0-based slot index 0..13) is omitted when non-negative — used
// while dragging a slot so its in-progress value is excluded from the ring.
int aggregateAmount (juce::AudioProcessorValueTreeState& apvts, ModDst dest, int excludeSlot = -1);

// The 0-based slot indices (0..13) currently routed to @p dest, in ascending
// order. Returns an empty vector for an invalid destination.
std::vector<int> slotsForDest (juce::AudioProcessorValueTreeState& apvts, ModDst dest);

}  // namespace parvati::ModDestMap

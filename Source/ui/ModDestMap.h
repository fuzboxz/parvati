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

#include "dsp/fx/FxTypes.h"   // FxModDestination (FX_DST_*), kNumFxMatrixSlots (FX domain)

namespace parvati::ModDestMap
{
// A ModulationDestination enum value (ambika::dsp::MOD_DST_*), an FX-dest
// offset (FX_DST_* + kFxModDstOffset), or -1 for "none".
using ModDst = int;

// ---- FX-dest domain (drag-and-drop modulation on the FX page) ----
// Synth MOD_DST_* dests occupy 0..kFxModDstOffset-1 (== MOD_DST_LAST entries);
// FX FX_DST_* dests are offset above them to a NON-OVERLAPPING range so a single
// ModDst int carries its domain (one range check routes synth vs FX). Pinned by
// a static_assert in the .cpp against MOD_DST_LAST. FX dests are
// kFxModDstOffset + FX_DST_* (== 19..33); 16 fxmod slots drive them.
constexpr int kFxModDstOffset = 19;                  // == ambika::dsp::MOD_DST_LAST
constexpr int kFxNumSlots     = kNumFxMatrixSlots;   // 16 fxmod slots (FxTypes.h)

// True if @p dest is in the FX domain (an FX_DST_* offset). Used by the editor to
// pick the slot param prefix ("mod" vs "fxmod") and by each assign handler to
// ignore the other domain's drops.
bool isFxDest (ModDst dest) noexcept;

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

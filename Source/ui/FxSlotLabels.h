// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.
//
// FxSlotLabels — declarations for the per-FX-type active-parameter count and
// semantic short parameter labels. The definitions live in FxSlotLabels.cpp
// (they are the source of truth for the live param-knob labels on an FX-slot
// card).
// This tiny dependency-free shard exposes them to FxMatrixView so the FX
// mod-matrix's destination combo can show each slot's ACTUAL parameter names
// (e.g. "FX1 Position/Size/Pitch/Freeze" for a Looping Delay) instead of the
// static "FX{N} Param K" labels — without FxMatrixView having to pull in all of
// FxSlotCard.h (ParamControl / the editor).

#pragma once

#include "dsp/fx/FxTypes.h"   // FxType
#include <juce_core/juce_core.h>   // juce::String

// Number of active (visible) generic params for an effect type (0..4). idx
// 0..activeParamCount-1 are real; idx >= activeParamCount are inactive/hidden.
int activeParamCount (FxType t) noexcept;

// Semantic short label for generic param @p idx (0..3) of effect type @p t
// ("Amount", "Position", "Pitch", ...). Returns "-" for an inactive idx.
const char* paramLabel (FxType t, int idx) noexcept;

// Meaningful-unit value readout for generic param @p idx of effect type @p t,
// given the raw stored 0..127 value (e.g. "C4", "+12.0 st", "440 Hz", "On",
// or "50%"). DISPLAY-ONLY — the stored value is unchanged; this just formats
// the knob's popup readout. Dimensionless params fall back to 0..100%.
juce::String paramValueText (FxType t, int idx, double value0to127);

// Master-section FX readouts (hoisted from FxRoutingBar.cpp so the HOST-VISIBLE
// parameter text — wired in ParameterLayout.cpp via
// AudioParameterIntAttributes::withStringFromValueFunction — shares ONE
// implementation with the UI knobs). Both are pure math on the raw 0..127 byte
// (juce_core only; callable from arbitrary host threads):
//   fxEqLowToString : fx_eq_low   0..127 (0 = off, else HP 20..1500 Hz, log)
//   fxEqDbToString  : fx_eq_mid/high 0..127 (64 = unity, ±12 dB)
juce::String fxEqLowToString (double v);
juce::String fxEqDbToString  (double v);

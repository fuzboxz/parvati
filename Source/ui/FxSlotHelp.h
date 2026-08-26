// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.
//
// FxSlotHelp — one curated help sentence for every ACTIVE generic param of
// every FX type. The definitions live in FxSlotHelp.cpp. FxSlotCard installs
// the text as the param-knob TOOLTIP (and the accessibility description) in
// refreshFromType(), so the knob help follows the loaded module. The knob
// LABEL itself comes from FxSlotLabels.h (the single source for labels).
// This shard stays dependency-light like FxSlotLabels: FxType, juce::String
// and the label table. Nothing else.

#pragma once

#include "dsp/fx/FxTypes.h"        // FxType
#include <juce_core/juce_core.h>   // juce::String

// Module help text for generic param @p idx (0-based, 0..4) of effect type
// @p t in slot @p slot (1-based, 1..3). Shape: "FX slot N <label>: <one
// sentence>." Returns an empty string for an INACTIVE param (idx at or above
// activeParamCount) and for a type without params (None, Diffuser); the
// caller then keeps the generic ParamHelp text.
juce::String fxParamHelp (FxType t, int slot, int idx);

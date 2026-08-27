// Copyright (c) 2026 805Labs Kft. / Hellcat.
//
// SynthParamLabels — meaningful-unit value readout for the SYNTH-section knob
// params (OSC/MIX/FILTER/ENV/LFO/MOD/SEQ/ARP/GLOBAL). The FX section has its
// own formatter (ui/FxSlotLabels.cpp::paramValueText); this is the synth mirror.
//
// DISPLAY-ONLY: the returned string is just the knob's centre readout (drawn by
// HellcatLookAndFeel::drawRotarySlider via slider.getTextFromValue). The stored
// APVTS value, the slider value, serialization, and the painter are all
// untouched — wiring installs it through ParamControl::setDisplayValueText.

#pragma once

#include <juce_core/juce_core.h>   // juce::String

// Meaningful-unit readout for the synth param @p paramID at denormalized value
// @p value (the slider's natural unit: 0..127 for unsigned, signed min..max for
// bipolar params). Unmatched paramIDs fall back to the raw integer (the
// pre-formatter behaviour), so this is always safe to call.
juce::String paramValueTextSynth (const juce::String& paramID, double value);

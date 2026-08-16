// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// FilterResponseDisplay — a compact LIVE magnitude-vs-frequency response curve
// for the Filter 1 section (cutoff / resonance / mode). Intended as a decoration
// under the "Filter 1" group so the filter shape is visible while editing.
//
// Decoupled contract: constructed with NORMALIZED (0..1) getters for cutoff,
// resonance, and the mode choice index (0..3 = LP/BP/HP/Notch). Self-contained:
// owns a 30 Hz refresh timer with an eps-diff gate. Read-only on the APVTS.
// Colours are read from the active ParvatiTheme via the component's LookAndFeel
// every repaint; the trace adopts a category hue (catAudio) via
// setCategoryColour().
//
// Magnitude model (local math — the runtime filter in dsp/analog_filter.cpp is
// NOT touched): a 4-pole RESONANT LADDER prototype (Moog-style) evaluated at
// ~64 log-spaced frequency points (20 Hz .. 16 kHz). The cutoff->Hz mapping
// replicates AnalogFilter::cutoffByteToHz (exponential 20..16k) so the curve is
// visually consistent with the actual filter. The ladder gives a 24 dB/oct
// skirt AND classic analog resonance behaviour: as the resonance (feedback K)
// rises, the passband gain DROPS (energy is stolen into the resonance peak near
// fc) and the peak grows. LP/BP/HP/Notch share the same ladder denominator.
// Cutoff/resonance are tracked EXACTLY (displayed = live APVTS target each
// 30 Hz tick) so the preview is accurate under automation; mode is discrete
// (snaps). fc is marked with a vertical tick.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

#include "ParvatiLookAndFeel.h"

//==============================================================================
class FilterResponseDisplay : public juce::Component,
                              private juce::Timer
{
public:
    /** Construct a filter response preview.
        @param getCutoff NORMALIZED 0..1 value of filter1_cutoff.
        @param getReso   NORMALIZED 0..1 value of filter1_resonance.
        @param getMode   NORMALIZED 0..1 value of filter1_mode (choice 0..3). */
    FilterResponseDisplay (juce::String title,
                           std::function<float()> getCutoff,
                           std::function<float()> getReso,
                           std::function<float()> getMode);

    ~FilterResponseDisplay() override;

    void setCategoryColour (const juce::Colour& c) { categoryColour_ = c; hasCategoryColour_ = true; repaint(); }
    bool hasCategoryColour() const noexcept { return hasCategoryColour_; }
    juce::Colour getCategoryColour() const noexcept { return categoryColour_; }

    void paint (juce::Graphics&) override;

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    void timerCallback() override;
    float fetch (const std::function<float()>& f) const;

    // 4-pole resonant-ladder |H|^2 for the given mode at frequency f with pole
    // fc and resonance feedback K (0 = none, ->4 = self-oscillation).
    // mode: 0=LP 1=BP 2=HP 3=Notch.
    static float magnitudeSq (float f, float fc, float K, int mode);

    juce::String title_;
    std::function<float()> getCutoff_, getReso_, getMode_;

    juce::Colour categoryColour_;
    bool hasCategoryColour_ = false;

    // DISPLAYED values, tracked EXACTLY to the live APVTS target each 30 Hz tick
    // so the preview is accurate under automation (no smoothing lag); the repaint
    // gate fires on a change vs the previous tick. Mode is discrete and snaps
    // (lastM_ is the eps-change gate for it).
    float dispC_ = -1.0f, dispR_ = -1.0f;   // -1 => first tick
    float lastM_ = -1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FilterResponseDisplay)
};

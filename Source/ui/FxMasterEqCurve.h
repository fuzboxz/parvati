// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// FxMasterEqCurve — a LIVE composite magnitude-response curve for the FX
// MASTER EQ (low-cut + mid peak + high-shelf), drawn to MATCH FxChain's RBJ
// biquads EXACTLY (same coefficient formulas + freq/gain mapping as
// FxChain::updateEqCoeffs), so the drawn curve is the actual EQ.
//
// Mirrors FxSlotVisualizer / FilterResponseDisplay: constructed with NORMALIZED
// (0..1) getters (fx_eq_low / fx_eq_mid / fx_eq_high), self-contained, owns a
// 30 Hz refresh timer with an eps-diff repaint gate, read-only on the APVTS, and
// reads its colours from the active ParvatiTheme via the component's LookAndFeel
// every repaint so theme switches are picked up automatically. The trace adopts
// a category hue via setCategoryColour() (else accentPrimary). The curve is
// FLAT (0 dB) at the defaults (low=0 / mid=64 / high=64) — a no-op EQ.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

class ParvatiLookAndFeel;   // resolved in the .cpp via getLookAndFeel()

//==============================================================================
class FxMasterEqCurve : public juce::Component,
                        private juce::Timer
{
public:
    using Getter = std::function<float()>;

    /** Construct the master-EQ response curve.
        @param getLow  NORMALIZED 0..1 value of fx_eq_low  (0 => low-cut off).
        @param getMid  NORMALIZED 0..1 value of fx_eq_mid  (64/127 => 0 dB).
        @param getHigh NORMALIZED 0..1 value of fx_eq_high (64/127 => 0 dB). */
    FxMasterEqCurve (Getter getLow, Getter getMid, Getter getHigh);

    ~FxMasterEqCurve() override;

    /** Adopt a hue for the curve TRACE (catAudio amber). When never called, the
        trace reads the live theme accentPrimary. */
    void setCategoryColour (const juce::Colour& c) { categoryColour_ = c; hasCategoryColour_ = true; repaint(); }

    void paint (juce::Graphics&) override;

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    void timerCallback() override;
    float fetch (const Getter& f) const;

    Getter getLow_, getMid_, getHigh_;

    juce::Colour categoryColour_;
    bool hasCategoryColour_ = false;

    // DISPLAYED values, tracked EXACTLY to the live APVTS target each 30 Hz tick
    // (accurate under automation); the repaint gate fires on a change vs the
    // previous tick. -1.0f => first tick (paint fetches fresh).
    float dispLow_ = -1.0f, dispMid_ = -1.0f, dispHigh_ = -1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxMasterEqCurve)
};

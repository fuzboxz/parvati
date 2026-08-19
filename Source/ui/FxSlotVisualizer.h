// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// FxSlotVisualizer — a compact LIVE per-slot effect graphic for the FX page's
// three FX-slot cards (FX1/FX2/FX3). It mirrors the visual style + decoupled
// contract of OscPreviewDisplay (waveform) and FilterResponseDisplay (filter
// curve): constructed with NORMALIZED (0..1) getters, self-contained, owns a
// 30 Hz refresh timer with an eps-diff repaint gate, read-only on the APVTS, and
// reads its colours from the active ParvatiTheme via the component's
// LookAndFeel every repaint so theme switches are picked up automatically. The
// trace adopts a category hue via setCategoryColour() (else accentSecondary,
// the FX/bypass accent token).
//
// The graphic is chosen by the slot's FxType (fx{N}_type):
//   None               (0) — dimmed grid + a passive centred outline (empty slot).
//   Diffuser..Resonator (1..10) — per-type Clouds/Warps/Rings graphics
//                               (drawDiffuser / drawPitchShifter / drawReverb /
//                                drawLoopingDelay / drawWSOLAStretch / drawSpectral /
//                                drawWavefolder / drawFrequencyShifter /
//                                drawRingModulator / drawResonator).
//
// The slot's Dry/Wet (getDryWet) scales the overall trace vividness: a fully-dry
// slot reads dimmer, a fully-wet slot reads vivid (the knob is visible here).
//
// All values are tracked EXACTLY to the live APVTS target each 30 Hz tick (no
// smoothing lag); the repaint gate fires on a change vs the previous tick.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

class ParvatiLookAndFeel;   // resolved in the .cpp via getLookAndFeel()

//==============================================================================
class FxSlotVisualizer : public juce::Component,
                         private juce::Timer
{
public:
    using Getter = std::function<float()>;

    /** Construct a per-slot FX graphic.
        @param getType   NORMALIZED 0..1 value of fx{N}_type (choice None..Resonator).
        @param getP0..4  NORMALIZED 0..1 values of fx{N}_param1..5.
        @param getDryWet NORMALIZED 0..1 value of fx{N}_drywet (0 = fully dry). */
    FxSlotVisualizer (Getter getType, Getter getP0, Getter getP1,
                      Getter getP2, Getter getP3, Getter getP4, Getter getDryWet);

    ~FxSlotVisualizer() override;

    /** Adopt a hue for the FX TRACE. When never called, the trace reads the live
        theme accentSecondary (the FX/bypass accent). */
    void setCategoryColour (const juce::Colour& c) { categoryColour_ = c; hasCategoryColour_ = true; repaint(); }
    bool hasCategoryColour() const noexcept { return hasCategoryColour_; }
    juce::Colour getCategoryColour() const noexcept { return categoryColour_; }

    void paint (juce::Graphics&) override;

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    void timerCallback() override;
    // F-ios-perf-3 (iOS hunt 2026-08-19): gate the 30 Hz poll on visibility.
    // The TabbedComponent UNPARENTS non-current pages (fires this), and an
    // AUv3 host can keep the extension process alive with the editor closed
    // — ~10 components x 30 Hz of atomic/APVTS fetches burn battery for
    // nothing then. The callbacks are change-only (cheap idle tick), so the
    // gating is about the wakeup cadence, not the tick cost.
    void visibilityChanged() override;
    float fetch (const Getter& f) const;

    // Per-type graphic drawers (all allocation-free per paint: juce::Path locals
    // live on the stack, exactly like the sibling vectorTrace recipes).
    void drawNone    (juce::Graphics& g, juce::Rectangle<float> plot,
                      ParvatiLookAndFeel* lnf,
                      juce::Colour passive, juce::Colour dimText);

    // Clouds FX-mode graphics (parametric, APVTS-only, allocation-free per paint).
    void drawDiffuser     (juce::Graphics& g, juce::Rectangle<float> plot,
                           ParvatiLookAndFeel* lnf,
                           juce::Colour trace, juce::Colour dimText,
                           float amount, float wet);
    void drawPitchShifter (juce::Graphics& g, juce::Rectangle<float> plot,
                           ParvatiLookAndFeel* lnf,
                           juce::Colour trace, juce::Colour dimText,
                           float ratio, float size, float wet);
    void drawReverb (juce::Graphics& g, juce::Rectangle<float> plot,
                           ParvatiLookAndFeel* lnf,
                           juce::Colour trace, juce::Colour accent, juce::Colour dimText,
                           float amount, float time, float tone, float diffusion, float wet);
    void drawLoopingDelay (juce::Graphics& g, juce::Rectangle<float> plot,
                           ParvatiLookAndFeel* lnf,
                           juce::Colour trace, juce::Colour accent, juce::Colour dimText,
                           float position, float size, float pitch, float freeze, float wet);
    void drawWSOLAStretch (juce::Graphics& g, juce::Rectangle<float> plot,
                           ParvatiLookAndFeel* lnf,
                           juce::Colour trace, juce::Colour dimText,
                           float pitch, float position, float size, float wet);
    void drawSpectral     (juce::Graphics& g, juce::Rectangle<float> plot,
                           ParvatiLookAndFeel* lnf,
                           juce::Colour trace, juce::Colour accent, juce::Colour dimText,
                           float pitch, float warp, float position, float blur, float wet);
    void drawWavefolder    (juce::Graphics& g, juce::Rectangle<float> plot,
                           ParvatiLookAndFeel* lnf,
                           juce::Colour trace, juce::Colour dimText,
                           float fold, float bias, float wet);
    void drawFrequencyShifter (juce::Graphics& g, juce::Rectangle<float> plot,
                           ParvatiLookAndFeel* lnf,
                           juce::Colour trace, juce::Colour accent, juce::Colour dimText,
                           float shift, float feedback, float spread, float wet);
    void drawRingModulator (juce::Graphics& g, juce::Rectangle<float> plot,
                           ParvatiLookAndFeel* lnf,
                           juce::Colour trace, juce::Colour accent, juce::Colour dimText,
                           float carrier, float shape, float amount, float wet);
    void drawResonator (juce::Graphics& g, juce::Rectangle<float> plot,
                        ParvatiLookAndFeel* lnf,
                        juce::Colour trace, juce::Colour accent, juce::Colour dimText,
                        float pitch, float decay, float bright, float timbre, float wet);

    // Wetness vividness: dry => dimmer trace, wet => vivid (0.42..1.0 alpha).
    static float wetAlpha (float wet) noexcept;

    Getter getType_, getP0_, getP1_, getP2_, getP3_, getP4_, getDryWet_;

    juce::Colour categoryColour_;
    bool hasCategoryColour_ = false;

    // DISPLAYED values, tracked EXACTLY to the live APVTS target each 30 Hz tick
    // so the preview is accurate under automation; the repaint gate fires on a
    // change vs the previous tick. -1.0f => first tick (paint fetches fresh).
    float dispType_   = -1.0f;
    float dispP0_     = -1.0f, dispP1_ = -1.0f, dispP2_ = -1.0f, dispP3_ = -1.0f, dispP4_ = -1.0f;
    float dispDryWet_ = -1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxSlotVisualizer)
};

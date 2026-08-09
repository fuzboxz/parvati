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
//   None     (0) — dimmed grid + a passive centred outline (the empty-slot state).
//   GainPan  (1) — an L<->R pan track with a position dot + a vertical gain meter.
//   Delay    (2) — a left source pulse followed by DECAYING echo taps repeating
//                  to the right (tap spacing = time, decay = feedback); a faint
//                  ghost row offset by the stereo-spread param conveys width.
//   Reverb   (3) — a decaying impulse-response tail envelope (length = size,
//                  steepness = damp) + a wet-level band + a stereo-width bracket.
//   Chorus   (4) — two wobbling delayed traces (L/R) whose sine amplitude tracks
//                  `depth` and whose spatial wobble count tracks `rate`. STATIC
//                  (frozen phase): it redraws only on a rate/depth/type change.
//
// The slot's Dry/Wet (getDryWet) scales the overall trace vividness: a fully-dry
// slot reads dimmer, a fully-wet slot reads vivid (the knob is visible here).
//
// All values are tracked EXACTLY to the live APVTS target each 30 Hz tick (no
// smoothing lag); the repaint gate fires on a change vs the previous tick. The
// Chorus graphic is STATIC (its phase is frozen) so it too only redraws on a
// rate/depth/type change.

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
        @param getType   NORMALIZED 0..1 value of fx{N}_type (choice None..Chorus).
        @param getP0..3  NORMALIZED 0..1 values of fx{N}_param1..4.
        @param getDryWet NORMALIZED 0..1 value of fx{N}_drywet (0 = fully dry). */
    FxSlotVisualizer (Getter getType, Getter getP0, Getter getP1,
                      Getter getP2, Getter getP3, Getter getDryWet);

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
    float fetch (const Getter& f) const;

    // Per-type graphic drawers (all allocation-free per paint: juce::Path locals
    // live on the stack, exactly like the sibling vectorTrace recipes).
    void drawNone    (juce::Graphics& g, juce::Rectangle<float> plot,
                      ParvatiLookAndFeel* lnf,
                      juce::Colour passive, juce::Colour dimText);
    void drawGainPan (juce::Graphics& g, juce::Rectangle<float> plot,
                      ParvatiLookAndFeel* lnf,
                      juce::Colour trace, juce::Colour dimText,
                      float gain, float pan, float wet);
    void drawDelay   (juce::Graphics& g, juce::Rectangle<float> plot,
                      ParvatiLookAndFeel* lnf,
                      juce::Colour trace, juce::Colour dimText,
                      float time, float feedback, float spread, float wet);
    void drawReverb  (juce::Graphics& g, juce::Rectangle<float> plot,
                      ParvatiLookAndFeel* lnf,
                      juce::Colour trace, juce::Colour accent, juce::Colour dimText,
                      float size, float damp, float level, float width, float wet);
    void drawChorus  (juce::Graphics& g, juce::Rectangle<float> plot,
                      juce::Colour trace,
                      float rate, float depth, float wet);

    // Wetness vividness: dry => dimmer trace, wet => vivid (0.42..1.0 alpha).
    static float wetAlpha (float wet) noexcept;

    Getter getType_, getP0_, getP1_, getP2_, getP3_, getDryWet_;

    juce::Colour categoryColour_;
    bool hasCategoryColour_ = false;

    // DISPLAYED values, tracked EXACTLY to the live APVTS target each 30 Hz tick
    // so the preview is accurate under automation; the repaint gate fires on a
    // change vs the previous tick. -1.0f => first tick (paint fetches fresh).
    float dispType_   = -1.0f;
    float dispP0_     = -1.0f, dispP1_ = -1.0f, dispP2_ = -1.0f, dispP3_ = -1.0f;
    float dispDryWet_ = -1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxSlotVisualizer)
};

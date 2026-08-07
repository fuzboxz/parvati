// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// OscPreviewDisplay — a compact LIVE vector preview of the selected oscillator
// shape, intended to sit INLINE beside the Shape dropdown in each OSC panel so
// the waveform is visible while editing. Phase 2 of docs/UI-...
//
// Decoupled contract: constructed with NORMALIZED (0..1) getters for the
// oscillator shape (a choice index 0..WAVEFORM_LAST-1) and the oscillator
// parameter (PWM/CZ-resonance/wavetable-position/...). Self-contained: owns a
// 30 Hz refresh timer with an eps-diff gate, so it only re-renders + repaints
// when the shape or (quantized) parameter actually changes. Read-only on the
// APVTS. Colours are read from the active ParvatiTheme via the component's
// LookAndFeel every repaint, so theme switches are picked up automatically; the
// trace adopts a category hue (catAudio amber) via setCategoryColour().
//
// Rendering strategy (hybrid):
//   * The 5 basic shapes (None/Saw/Square/Triangle/Sine) are drawn ANALYTICALLY
//     (exact). Square's duty cycle tracks the parameter (PWM).
//   * The remaining deterministic algorithms (CZ family, Quad Saw, FM, 8-Bit
//     Land, Dirty PWM, Filtered Noise, Wavetables, Wavequence) are SAMPLED by
//     instantiating the REAL ambika::dsp::Oscillator OFF the audio thread, feeding
//     the shape + parameter, calling Render() into a 40-sample scratch buffer,
//     and plotting one cycle. The oscillator instance is LOCAL to the render
//     (never touches the audio thread). Reset() is deliberately NOT called so the
//     global Random LFSR (used by the audio-thread vowel renderer) is never
//     mutated -> Filtered Noise uses its OWN per-instance LFSR instead.
//   * The Vowel algorithm touches the shared global Random (data race with the
//     audio thread), so it falls back to an analytic multi-formant GLYPH.
// The sampled cycle is cached keyed by (shape, quantized parameter) so it only
// re-renders on change.
//
// SMOOTHING (Phase: interpolation pass):
//   * Shape (and quantized-parameter) changes do NOT snap — the previously
//     displayed cycle is kept as `prevCycle_` and each painted sample is a lerp
//     from prev to the new target over ~66 ms (`morphProgress_` advances in the
//     30 Hz timer, ~2 ticks). Both buffers are indexed by the same phase
//     fraction so they need not share a length (analytic = 256, sampled =
//     kAudioBlockSize).
//   * The continuous oscillator parameter is tracked EXACTLY (displayed = live
//     APVTS target) so the preview is accurate under automation/modulation (no
//     smoothing lag). For the flicker-free analytic shapes (Square PWM / Vowel
//     formants) the cycle is rebuilt from the exact parameter on change; the
//     DSP-sampled shapes keep the quantized rebuild (to avoid per-tick oscillator
//     re-render flicker) and morph between cycles instead.
// The eps-diff gate is retained: it only repaints while morphing or on change.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <functional>
#include <vector>

//==============================================================================
class OscPreviewDisplay : public juce::Component,
                          private juce::Timer
{
public:
    /** Construct a waveform preview for one oscillator.
        @param getShape  NORMALIZED 0..1 value of the osc shape choice
                         (0 => WAVEFORM_NONE, 1 => WAVEFORM_WAVEQUENCE).
        @param getParam  NORMALIZED 0..1 value of the osc parameter (PWM/CZ/...). */
    OscPreviewDisplay (juce::String title,
                       std::function<float()> getShape,
                       std::function<float()> getParam);

    ~OscPreviewDisplay() override;

    /** Adopt a category hue for the waveform TRACE (catAudio amber). When never
        called, the trace reads the live theme accent. */
    void setCategoryColour (const juce::Colour& c) { categoryColour_ = c; hasCategoryColour_ = true; repaint(); }
    bool hasCategoryColour() const noexcept { return hasCategoryColour_; }
    juce::Colour getCategoryColour() const noexcept { return categoryColour_; }

    void paint (juce::Graphics&) override;

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    void timerCallback() override;
    float fetch (const std::function<float()>& f) const;

    // Fill cycle_ (normalized -1..1) with one cycle of the given shape/parameter.
    void rebuildCycle (int shapeIdx, uint8_t paramByte);
    void buildAnalytic (int shapeIdx, uint8_t paramByte);
    void buildSampled (int shapeIdx, uint8_t paramByte);

    // Stash the current cycle as the morph source, rebuild the new target from
    // the (smoothed) parameter, and (re)start the morph.
    void stashAndRebuild (int shapeIdx);

    juce::String title_;
    std::function<float()> getShape_, getParam_;

    juce::Colour categoryColour_;
    bool hasCategoryColour_ = false;

    // Cached (shape, quantized parameter) the current cycle was built for
    // (-1 / 0xff => nothing built yet -> first timer tick rebuilds).
    int cachedShape_ = -1;
    uint8_t cachedParamQ_ = 0xff;

    // The rendered one-cycle waveform (normalized -1..1), resampled to the
    // canvas columns at paint time.
    std::vector<float> cycle_;

    // Shape/param morph: prevCycle_ is the previously-displayed cycle (the morph
    // source); morphProgress_ runs 0->1 over ~66 ms (~2 ticks; 1 => morph done,
    // prev dropped). displayedParam_ is the oscillator parameter tracked EXACTLY
    // to the live APVTS target (accurate under automation); lastBuiltParamF_
    // tracks the parameter the current analytic cycle was built for (to detect
    // movement requiring a continuous rebuild).
    std::vector<float> prevCycle_;
    float morphProgress_   = 1.0f;
    float displayedParam_  = 0.0f;
    float lastBuiltParamF_ = -1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OscPreviewDisplay)
};

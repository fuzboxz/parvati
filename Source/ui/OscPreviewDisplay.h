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
// trace adopts a category hue (catAudio) via setCategoryColour().
//
// LIVE MODULATED OVERLAY (2026-08-23 parity pass): an optional LiveOscValues
// provider (wired to the engine telemetry through the editor's LiveFeedbackHub)
// reports the EFFECTIVE (modulation-applied) oscillator parameter while a
// voice sounds — the same contract FilterResponseDisplay has for cutoff /
// resonance. The preview follows the CURRENT engine state: while the effective
// byte is MOVING (>= 1 byte per tick, held ~270 ms after the last movement) the
// smoothed display target switches from the knob value to the live byte, so an
// env/LFO/matrix sweep on the osc parameter visibly rides the waveform; at
// rest the preview settles back to the knob state. There is no second curve —
// ONE waveform moves, exactly like the filter's "one curve at a time" handoff.
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
// The sampled cycle is rebuilt whenever the (quantized) parameter byte moves
// (see SMOOTHING below). Determinism: every rebuild instantiates a FRESH
// Oscillator (phase zeroed by value-initialization; the filtered-noise LFSR
// starts from its fixed zero state; Reset() is never called so the global RNG
// is never touched), so the same (shape, param byte) always renders the
// bit-identical buffer — successive rebuilds of a moving param read as a
// smooth re-shaping, never flicker.
//
// SMOOTHING (2026-08-23 — filter-preview parity, revised twice the same
// day): the displayed parameter is SMOOTHED, not snapped. Two paths share
// smoothParam01_: the KNOB path uses a FIXED-DURATION glide (kGlideT ~140 ms,
// quadratic-out ease, retargeted on every knob change — tracks a fast spin
// with <= one glide of lag and finishes EXACTLY kGlideT after the last
// change, so no byte-step can land after the animation completes); the LIVE
// overlay path keeps the FilterResponseDisplay's critically-damped
// exponential (tau 130 ms, half-byte snap) so modulated telemetry steps
// blend identically to the filter preview. The cycle rebuilds whenever the
// quantized byte of the smoothed value moves; the repaint gate is BYTE-driven
// (a tick that moves no byte paints nothing) and the component paints OPAQUE
// (no parent recomposite per repaint). A discrete SHAPE switch still uses the
// short prev->next cycle morph (~66 ms), the analogue of the filter's
// snapping mode.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <functional>
#include <vector>

#include "ModTelemetryTypes.h"   // parvati::LiveOscValues (live modulated overlay)

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

    /** Adopt a category hue for the waveform TRACE (catAudio). When never
        called, the trace reads the live theme accent. */
    void setCategoryColour (const juce::Colour& c) { categoryColour_ = c; hasCategoryColour_ = true; repaint(); }
    bool hasCategoryColour() const noexcept { return hasCategoryColour_; }
    juce::Colour getCategoryColour() const noexcept { return categoryColour_; }

    // TEST-ONLY diagnostic: incremented on every real cycle REBUILD (a shape
    // switch, or a move of the byte-quantized SMOOTHED parameter — see
    // SMOOTHING above). Lets a headless test observe "the preview reacted to
    // a param change" without touching painting. Not read by any product
    // code.
    int previewGeneration() const noexcept { return generation_; }

    // TEST-ONLY: is the 30 Hz poll timer running? (Timer is a private base;
    // getTimerInterval() is inaccessible externally.) Pins the F-ios-perf-3
    // gate semantics: stopped while not showing, running once shown.
    bool isPollRunningForTest() const noexcept { return getTimerInterval() > 0; }

    /** Re-evaluate the poll timer's run state NOW (public twin of the private
        dual-hook gate — the same backstop EnvelopeDisplay has). The EDITOR's
        status timer calls this every ~30 Hz tick for every registered osc
        preview (liveOscDisplays_), so a poll whose own hooks were starved
        (page built off-screen, then swapped in) starts within one tick. */
    void reassertPollTimer() { updatePollTimer(); }

    /** Live modulated overlay (2026-08-23 parity pass — same contract as
        FilterResponseDisplay::setLiveValuesProvider): @p p returns the
        EFFECTIVE (modulation-applied) oscillator parameter, normalized to
        0..1 of the 0..127 effective-byte domain (the same domain as the base
        param byte). While the effective byte is MOVING (>= 1 byte vs the
        previous tick) the smoothed display target follows it; a short hold
        (~270 ms) bridges modulation dips below the 1-byte rate, then the
        preview eases back to the knob value. An empty or inactive provider
        hides the overlay at zero overhead beyond one std::function call per
        poll tick. */
    void setLiveValuesProvider (std::function<parvati::LiveOscValues()> p);

    // TEST-ONLY: is the live modulated overlay now ARMED (the temporal
    // activity state after the change gate — lets a headless test observe the
    // overlay without a Graphics context, mirroring the filter display's
    // liveCurveVisibleForTest seam).
    bool liveOverlayActiveForTest() const noexcept { return dispLiveActive_; }

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
    //
    // BUG FIX (preview-update regression): BOTH hierarchy hooks are needed.
    // juce::Component is constructed HIDDEN (componentFlags(0)), and
    // addAndMakeVisible() calls setVisible(true) BEFORE parenting — so the
    // display's visibilityChanged() fires while UNPARENTED (isShowing()==false)
    // and used to stopTimer() immediately after construction. JUCE only sends
    // visibilityChanged to the component whose OWN flag changed (ancestor
    // visibility / peer creation never propagates it to descendants), so the
    // timer stayed dead forever — frozen previews from launch. JUCE DOES
    // recurse internalHierarchyChanged() through all children on every
    // hierarchy change (add/remove, and addToDesktop when the editor gets its
    // peer), so parentHierarchyChanged() is the reliable "did we become
    // showing?" hook. Both funnel into updatePollTimer().
    void visibilityChanged() override;
    void parentHierarchyChanged() override;

    void updatePollTimer();
    float fetch (const std::function<float()>& f) const;

    // Fill cycle_ (normalized -1..1) with one cycle of the given shape/parameter.
    void rebuildCycle (int shapeIdx, uint8_t paramByte);
    void buildAnalytic (int shapeIdx, uint8_t paramByte);
    void buildSampled (int shapeIdx, uint8_t paramByte);

    // Stash the current cycle as the morph source, rebuild the new target from
    // the (smoothed) parameter, and (re)start the morph. SHAPE switches only —
    // parameter motion glides through the smoothed rebuild instead.
    void stashAndRebuild (int shapeIdx);

    juce::String title_;
    std::function<float()> getShape_, getParam_;

    juce::Colour categoryColour_;
    bool hasCategoryColour_ = false;

    // Cached shape the current cycle was built for (-1 => nothing built yet ->
    // first timer tick rebuilds) + the byte it was built from (0xff => none).
    int     cachedShape_ = -1;
    uint8_t lastBuiltParamByte_ = 0xff;

    // The rendered one-cycle waveform (normalized -1..1), resampled to the
    // canvas columns at paint time.
    std::vector<float> cycle_;

    // SHAPE-switch morph (param motion now glides via smoothParam01_ instead):
    // prevCycle_ is the previously-displayed cycle (the morph source);
    // morphProgress_ runs 0->1 over ~66 ms (~2 ticks; 1 => morph done, prev
    // dropped).
    std::vector<float> prevCycle_;
    float morphProgress_   = 1.0f;

    // displayedParam_ tracks the base knob fetch EXACTLY (the eps-change gate
    // term + fallback before the first tick); smoothParam01_ is the eased
    // DISPLAY value — it converges toward the current target (base at rest,
    // live effective byte under modulation) with the filter preview's
    // critically-damped exponential (tau 130 ms) and drives the byte-quantized
    // cycle rebuild. -1 = never ticked (snap on the first tick).
    float displayedParam_ = 0.0f;
    float smoothParam01_  = -1.0f;

    // KNOB-path fixed-duration glide (see OscPreviewDisplay.cpp kGlideT):
    // glideFrom01_ -> glideTo01_ over kGlideT with a quadratic-out ease,
    // retargeted whenever the knob value moves. glideTo01_ < 0 = no active
    // glide (retarget on the next tick). Bounds every byte change INSIDE the
    // window — nothing can land after the ease completes (the "delayed
    // change after the animation finishes" bug).
    float glideFrom01_ = 0.0f;
    float glideTo01_   = -1.0f;
    float glideT_      = 0.0f;

    // ---- Live modulated overlay state (see setLiveValuesProvider) ----
    // The provider is pulled in the SAME 30 Hz tick as the base getters;
    // activity is TEMPORAL like the filter overlay: a >= 1-byte move vs the
    // previous tick (re)arms a ~270 ms hold, a settled or absent voice hides
    // the overlay at once. dispLiveParamByte_ is the quantized live byte
    // (-1 => never seen an active provider frame).
    std::function<parvati::LiveOscValues()> liveValuesProvider_;
    bool dispLiveActive_     = false;
    int  dispLiveParamByte_  = -1;
    int  liveHoldTicks_      = 0;     // temporal-gate hold budget (ticks)

    // TEST-ONLY (see previewGeneration).
    int generation_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OscPreviewDisplay)
};

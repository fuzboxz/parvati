// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.
//
// FilterResponseDisplay — a compact LIVE magnitude-vs-frequency response curve
// for the Filter 1 section (cutoff / resonance / mode). Intended as a decoration
// under the "Filter 1" group so the filter shape is visible while editing.
//
// LIVE MODULATED OVERLAY (docs/LIVE_MOD_FEEDBACK_DESIGN.md): an optional
// LiveFilterValues provider (wired to the engine telemetry through the editor's
// LiveFeedbackHub) reports the EFFECTIVE (modulation-applied) cutoff /
// resonance while a voice sounds. When the effective bytes depart from the
// base knob bytes, a SECOND curve + a bright live cutoff tick show the
// modulated state while the opaque base preview stays exactly in place — the
// static shape reads as "what is set", the moving one as "what is happening".
//
// Decoupled contract: constructed with NORMALIZED (0..1) getters for cutoff,
// resonance, the mode choice index (0..3 = LP/BP/HP/Notch) and the filter-card
// choice (0..5, normalized). Self-contained: owns a 30 Hz refresh timer with
// an eps-diff gate. Read-only on the APVTS. Colours are read from the active
// HellcatTheme via the component's LookAndFeel every repaint; the trace
// adopts a category hue (catAudio) via setCategoryColour().
//
// Magnitude model (local math — the runtime filter in dsp/analog_filter.cpp is
// NOT touched): CARD-AWARE. The six cards split into two families, and the
// drawn resonance law MIRRORS the runtime law of the active card (see
// Source/dsp/analog_filter.h; keep the two in sync):
//   * 2-pole family (SVF, Polivoks skeleton): Q = 1/(2*(1-res)). The
//     Polivoks character layer is not drawn; the skeleton is the honest
//     static estimate.
//   * 4-pole cascade ("4P"): two identical 2-pole stages, per-stage
//     q = 0.5*(1-res)^-0.616 (mirrors kSsm4PeakExp).
//   * 4-pole feedback family (SMR4 K = 4*r; Ladder K = max(0.4, 4*r);
//     IR3109 K = 3.4*r, the factory cap): H = num/((1+jw)^4 + K).
// The 4-pole cards draw lowpass only (the hardware is lowpass); the SVF card
// draws LP/BP/HP/Notch; the Polivoks card draws LP and BP only (the runtime
// clamps HP/Notch to LP). The cutoff->Hz mapping replicates
// AnalogFilter::cutoffByteToHz (exponential 20..16k). Cutoff/resonance are
// tracked EXACTLY (displayed = live APVTS target each 30 Hz tick) so the
// preview is accurate under automation; mode and card are discrete (snap).

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

#include "ModTelemetryTypes.h"   // hellcat::LiveFilterValues (live modulated overlay)
#include "HellcatLookAndFeel.h"

//==============================================================================
class FilterResponseDisplay : public juce::Component,
                              private juce::Timer
{
public:
    /** Construct a filter response preview.
        @param getCutoff NORMALIZED 0..1 value of filter1_cutoff.
        @param getReso   NORMALIZED 0..1 value of filter1_resonance.
        @param getMode   NORMALIZED 0..1 value of filter1_mode (choice 0..3).
        @param getCard   NORMALIZED 0..1 value of the filter_card choice
                         (0..5 = SMR4 / 4P / SVF / Ladder / Polivoks /
                         IR3109). Optional: a null getter draws the SMR4
                         card (index 0). */
    FilterResponseDisplay (juce::String title,
                           std::function<float()> getCutoff,
                           std::function<float()> getReso,
                           std::function<float()> getMode,
                           std::function<float()> getCard = {});

    ~FilterResponseDisplay() override;

    void setCategoryColour (const juce::Colour& c) { categoryColour_ = c; hasCategoryColour_ = true; repaint(); }
    bool hasCategoryColour() const noexcept { return hasCategoryColour_; }
    juce::Colour getCategoryColour() const noexcept { return categoryColour_; }

    // TEST-ONLY diagnostic: incremented on every REAL refresh (a cutoff/reso
    // or mode change that passed the eps gate and triggered the repaint).
    int previewGeneration() const noexcept { return generation_; }

    // TEST-ONLY: is the 30 Hz poll timer running? (Timer is a private base.)
    bool isPollRunningForTest() const noexcept { return getTimerInterval() > 0; }

    /** Re-evaluate the poll timer's run state NOW (public twin of the private
        dual-hook gate; the editor's status timer calls this every ~30 Hz tick
        so a starved poll starts within one tick of visibility — see
        EnvelopeDisplay::reassertPollTimer). Idempotent. */
    void reassertPollTimer() { updatePollTimer(); }

    /** Live modulated overlay (docs/LIVE_MOD_FEEDBACK_DESIGN.md): @p p
        returns the EFFECTIVE (modulation-applied) filter state, normalized to
        0..1 of the 0..255 byte domain (the same domain as the base curve's
        bytes). While active AND the effective bytes differ from the base knob
        bytes (>= 2 on either axis), a second curve + a bright live cutoff tick
        render over the unchanged opaque base preview. An empty or inactive
        provider (or a near-base state) hides the overlay at zero overhead
        beyond one std::function call per poll tick. */
    void setLiveValuesProvider (std::function<hellcat::LiveFilterValues()> p);

    // TEST-ONLY: is the live modulated curve now SHOWN (the painted
    // state, i.e. after the change gate — lets a headless test observe the
    // overlay without a Graphics context)?
    bool  liveCurveVisibleForTest() const noexcept { return dispLiveActive_; }

    // TEST-ONLY: the live cutoff's normalized plot x (0..1, the log-frequency
    // column of the live fc tick). Only meaningful while
    // liveCurveVisibleForTest() is true.
    float liveCutoffXForTest() const noexcept { return dispLiveCutX_; }

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
    // BUG FIX (frozen previews): BOTH hooks funnel into updatePollTimer —
    // see OscPreviewDisplay.h for the full rationale (components are born
    // hidden; visibilityChanged fires pre-parenting and never again from
    // ancestor changes; parentHierarchyChanged recurses on every hierarchy
    // change including the editor gaining its peer).
    void visibilityChanged() override;
    void parentHierarchyChanged() override;
    void updatePollTimer();
    float fetch (const std::function<float()>& f) const;

    // 4-pole resonant-ladder |H|^2 for the given mode at frequency f with pole
    // fc and resonance feedback K (0 = none, ->4 = self-oscillation).
    // mode: 0=LP 1=BP 2=HP 3=Notch.
    static float ladderMagnitudeSq (float f, float fc, float K, int mode);

    // Card-aware |H|^2 at frequency f for card `card` (0..5), pole fc,
    // clamped resonance reso (0..0.95) and drawn mode (0..3). The laws
    // mirror Source/dsp/analog_filter.h (see the file header); keep the two
    // in sync. This file stays independent of the DSP headers.
    static float magnitudeSq (float f, float fc, int card, float reso, int mode);

    juce::String title_;
    std::function<float()> getCutoff_, getReso_, getMode_, getCard_;

    juce::Colour categoryColour_;
    bool hasCategoryColour_ = false;

    // DISPLAYED values, tracked EXACTLY to the live APVTS target each 30 Hz tick
    // so the preview is accurate under automation (no smoothing lag); the repaint
    // gate fires on a change vs the previous tick. Mode and card are discrete
    // and snap (lastM_ / lastCard_ are their eps-change gates).
    float dispC_ = -1.0f, dispR_ = -1.0f;   // -1 => first tick
    float lastM_ = -1.0f;
    int   lastCard_ = -1;

    // ---- Live modulated overlay (docs/LIVE_MOD_FEEDBACK_DESIGN.md) ----
    // The provider is pulled in the SAME 30 Hz tick as the base getters; the
    // fields below are the DISPLAYED (painted) overlay state, change-gated
    // exactly like dispC_/dispR_ (a visibility flip or a >= 1-byte move
    // triggers one repaint). The bytes live in the 0..255 effective domain
    // shared with the base curve. ACTIVITY is TEMPORAL (see timerCallback):
    // liveHoldTicks_ counts down from the hold budget while the bytes sit
    // still, so a settled note hides the overlay but an LFO/env sweep keeps
    // it armed (key tracking — a static patch setting — never trips it).
    std::function<hellcat::LiveFilterValues()> liveValuesProvider_;
    bool  dispLiveActive_  = false;
    int   dispLiveCutByte_ = -1;    // -1 => never seen an active provider frame
    int   dispLiveResByte_ = -1;
    float dispLiveCutX_    = 0.0f;  // normalized log-f x of the live cutoff tick
    int   liveHoldTicks_   = 0;     // temporal-gate hold budget (ticks)

    // SMOOTHED display pair (2026-08-21 user request — "the filter movement
    // seems a bit choppy"): both axes converge toward the CURRENT display
    // target (the base knob bytes at rest, the live bytes under modulation)
    // with a critically-damped exponential per tick, so the rendered curve
    // GLIDES between states instead of snapping on every >= 1-byte telemetry
    // step at the 30 Hz cadence. -1 = never painted (snap on first tick).
    // The RAW bytes above stay exact (test seams + the temporal gate); only
    // paint() reads these.
    float smoothCut01_ = -1.0f;
    float smoothRes01_ = -1.0f;

    // TEST-ONLY (see previewGeneration).
    int generation_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FilterResponseDisplay)
};

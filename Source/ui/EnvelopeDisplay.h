// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// EnvelopeDisplay — a small live ADSR preview that reacts to a set of
// Attack/Decay/Sustain/Release value getters. Intended to sit beside a row of
// envelope knobs (e.g. on the "Envelopes / LFO" tab) so the shape is visible
// while editing. Phase 3 of docs/UI_MODERNIZATION_PLAN.md (gap D12).
//
// LIVE STAGE OVERLAY (docs/LIVE_MOD_FEEDBACK_DESIGN.md): while a key is held,
// an optional LiveEnvStage provider (wired to the engine telemetry through the
// editor's LiveFeedbackHub) drives a position marker — a dot riding the drawn
// curve plus a hairline — through Attack/Decay/Sustain/Release, so the panel
// shows WHERE the envelope actually is, not just its shape.
//
// Decoupled contract: it is constructed with four normalized (0..1) value
// getters and is otherwise self-contained (owns a 30 Hz refresh timer). The
// editor wires the getters to the APVTS in Phase 4. Colours are read from the
// active ParvatiTheme via the component's LookAndFeel every repaint, so theme
// switches are picked up automatically.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

#include "ModTelemetryTypes.h"   // parvati::LiveEnvStage (live stage overlay)
#include "ParvatiLookAndFeel.h"

//==============================================================================
class EnvelopeDisplay : public juce::Component,
                        private juce::Timer
{
public:
    /** Construct an ADSR preview for one envelope unit.
        Each getter must return a NORMALIZED value in 0.0..1.0 (the editor
        normalizes the raw 0..127 parameter range before calling). Any getter
        may be omitted; it then reads as 0.
        @param getShape  optional NORMALIZED 0..1 value of the slot's LFO shape
                         (Triangle/Square/S&H/Ramp), used when previewMode()==1. */
    EnvelopeDisplay (juce::String title,
                     std::function<float()> getAttack,
                     std::function<float()> getDecay,
                     std::function<float()> getSustain,
                     std::function<float()> getRelease,
                     std::function<float()> getShape = {});

    ~EnvelopeDisplay() override;

    /** Preview mode: 0 = ADSR envelope shape, 1 = LFO waveform shape. */
    void setPreviewMode (int mode) { previewMode_ = mode; repaint(); }
    int  getPreviewMode() const noexcept { return previewMode_; }

    // TEST-ONLY diagnostic: incremented on every REAL refresh (a param or
    // shape change that passed the eps gate and triggered the repaint). Lets
    // a headless test observe "the preview reacted" without painting.
    int previewGeneration() const noexcept { return generation_; }

    // TEST-ONLY: the pure ADSR curve shape (the exact function paint() traces):
    // normalized a/d/s/r knob values -> the level at normalized x (0..1).
    // Pins the always-visible initial 0 -> peak transient (attack floor).
    static float adsrCurveLevelForTest (float a, float d, float s, float r, float xf)
    { return adsrCurveLevel (a, d, s, r, xf); }

    // TEST-ONLY: is the 30 Hz poll timer running? (Timer is a private base.)
    bool isPollRunningForTest() const noexcept { return getTimerInterval() > 0; }

    /** Relabel the unit (e.g. "Env 1"). */
    void setTitle (const juce::String& title) { title_ = title; juce::Component::setTitle (title); repaint(); }

    /** Adopt a category hue for the waveform TRACE + fill (e.g. the Envelopes
        cyan / LFOs magenta token). When never called, the trace reads the live
        theme accent. Only the trace + its fill change; the neutral LCD grid
        backdrop keeps the theme accent so the graph still reads on any theme. */
    void setCategoryColour (const juce::Colour& c) { categoryColour_ = c; hasCategoryColour_ = true; repaint(); }
    bool hasCategoryColour() const noexcept { return hasCategoryColour_; }
    juce::Colour getCategoryColour() const noexcept { return categoryColour_; }

    /** Live stage overlay (docs/LIVE_MOD_FEEDBACK_DESIGN.md): @p p returns the
        REAL stage/progress of this envelope slot (the editor binds it to the
        engine telemetry cache). While active and not DEAD, a marker (dot on the
        curve + a vertical hairline) shows where the envelope currently is as
        the key is held. An empty provider, an inactive stage, or previewMode()
        == 1 (LFO waveform) hides the marker at zero overhead beyond one
        std::function call per poll tick. */
    void setLiveStageProvider (std::function<parvati::LiveEnvStage()> p);

    // TEST-ONLY: is the live stage marker currently SHOWN (the painted state,
    // i.e. after the change gate — lets a headless test observe the marker
    // without a Graphics context)?
    bool  liveMarkerVisibleForTest() const noexcept { return markerVisible_; }

    // TEST-ONLY: the marker's normalized plot x (0..1). Only meaningful while
    // liveMarkerVisibleForTest() is true.
    float liveMarkerXForTest() const noexcept { return markerX_; }

    void paint (juce::Graphics&) override;

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    // The pure ADSR curve (segment math incl. the attack visual floor). Static
    // + file-scope so paint() and the test hook share ONE definition.
    static float adsrCurveLevel (float a, float d, float s, float r, float xf);

    // Raw (UN-normalized) ADSR segment weights for the given knob values:
    // sustain keeps its fixed minimum, attack is floored at kMinAttackShare of
    // the other segments' sum. THE single definition of the segment geometry —
    // adsrCurveLevel (the curve) and the live stage marker both consume it, so
    // the dot always rides the exact curve the panel paints
    // (docs/LIVE_MOD_FEEDBACK_DESIGN.md, EnvelopeDisplay contract).
    static void adsrSegmentSpans (float a, float d, float s, float r,
                                  float* wA, float* wD, float* wS, float* wR);

    // Normalized plot x (0..1) of the live stage marker for @p st, computed on
    // the DISPLAYED knob values (call after the disp* fields are updated in the
    // poll tick). Returns -1 when the marker is hidden (inactive / DEAD stage).
    float markerXForStage (const parvati::LiveEnvStage& st) const;

    // juce::Timer — re-read the getters and repaint when the shape changes.
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

    // Safely read a getter (0 if it is empty); clamps to 0..1.
    float fetch (const std::function<float()>& f) const;

    juce::String title_;
    std::function<float()> getAttack_, getDecay_, getSustain_, getRelease_, getShape_;

    // 0 = ADSR envelope, 1 = LFO waveform.
    int previewMode_ = 0;

    // Optional category hue for the trace/fill (defaults off -> live accent).
    juce::Colour categoryColour_;
    bool hasCategoryColour_ = false;

    // DISPLAYED ADSR values, tracked EXACTLY to the live APVTS target each
    // 30 Hz tick so the preview is accurate under automation (no smoothing lag);
    // the repaint gate fires on a change vs the previous tick. -1 => first tick.
    // The LFO shape choice is discrete and snaps (lastShape_ is its eps-change
    // gate).
    float dispA_ = -1.0f, dispD_ = -1.0f, dispS_ = -1.0f, dispR_ = -1.0f;
    float lastShape_ = -1.0f;

    // ---- Live stage overlay (docs/LIVE_MOD_FEEDBACK_DESIGN.md) ----
    // The provider is pulled in the SAME 30 Hz tick as the knob getters; the
    // fields below are the DISPLAYED (painted) marker state, change-gated
    // exactly like dispA_..dispR_ so a stationary marker costs no repaints
    // (markerVisible_ flip or an x move past the eps gate triggers one).
    std::function<parvati::LiveEnvStage()> liveStageProvider_;
    bool  markerVisible_ = false;
    float markerX_ = 0.0f;   // normalized plot x of the marker (see markerXForStage)

    // TEST-ONLY (see previewGeneration).
    int generation_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EnvelopeDisplay)
};

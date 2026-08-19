// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// EnvelopeDisplay — a small live ADSR preview that reacts to a set of
// Attack/Decay/Sustain/Release value getters. Intended to sit beside a row of
// envelope knobs (e.g. on the "Envelopes / LFO" tab) so the shape is visible
// while editing. Phase 3 of docs/UI_MODERNIZATION_PLAN.md (gap D12).
//
// Decoupled contract: it is constructed with four normalized (0..1) value
// getters and is otherwise self-contained (owns a 30 Hz refresh timer). The
// editor wires the getters to the APVTS in Phase 4. Colours are read from the
// active ParvatiTheme via the component's LookAndFeel every repaint, so theme
// switches are picked up automatically.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

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

    /** Relabel the unit (e.g. "Env 1"). */
    void setTitle (const juce::String& title) { title_ = title; juce::Component::setTitle (title); repaint(); }

    /** Adopt a category hue for the waveform TRACE + fill (e.g. the Envelopes
        cyan / LFOs magenta token). When never called, the trace reads the live
        theme accent. Only the trace + its fill change; the neutral LCD grid
        backdrop keeps the theme accent so the graph still reads on any theme. */
    void setCategoryColour (const juce::Colour& c) { categoryColour_ = c; hasCategoryColour_ = true; repaint(); }
    bool hasCategoryColour() const noexcept { return hasCategoryColour_; }
    juce::Colour getCategoryColour() const noexcept { return categoryColour_; }

    void paint (juce::Graphics&) override;

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    // juce::Timer — re-read the getters and repaint when the shape changes.
    void timerCallback() override;
    // F-ios-perf-3 (iOS hunt 2026-08-19): gate the 30 Hz poll on visibility.
    // The TabbedComponent UNPARENTS non-current pages (fires this), and an
    // AUv3 host can keep the extension process alive with the editor closed
    // — ~10 components x 30 Hz of atomic/APVTS fetches burn battery for
    // nothing then. The callbacks are change-only (cheap idle tick), so the
    // gating is about the wakeup cadence, not the tick cost.
    void visibilityChanged() override;

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EnvelopeDisplay)
};

// Copyright (c) 2024 805LABS / Parvati.
//
// VoiceMeter — a compact 16-voice activity indicator for the Parvati UI. Each of
// the 16 engine voices (6 firmware voicecards) is shown as a cell that lights up
// in the theme accent when the voice is active, dimming to an outline dot when
// free; active cells display the MIDI note name (e.g. "C4"). A header strip
// shows "Voices" + the active-count ("5/16"). The meter is fed per-frame by the
// editor (Phase 4) via a state-provider callback and owns nothing from the
// engine directly, so it stays decoupled and headless-testable. Phase 3 of
// docs/UI_MODERNIZATION_PLAN.md (gap D14).
//
// Colours are read from the active ParvatiTheme via the component's LookAndFeel
// (a ParvatiLookAndFeel set on an ancestor), so theme switches recolour the
// meter automatically — call refresh() to force a repaint after a switch.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <vector>

struct ParvatiTheme;

//==============================================================================
// One voice's instantaneous state, supplied each frame by the editor.
struct VoiceActivity
{
    bool active = false;   // true => this voice is currently sounding
    int  note  = -1;       // MIDI note number (0..127) when active, else -1
};

//==============================================================================
class VoiceMeter : public juce::Component,
                   private juce::Timer
{
public:
    VoiceMeter();
    ~VoiceMeter() override;

    /** Set the callback the meter polls at ~30 Hz to refresh its 16 cells.
        The callback must return exactly 16 entries (one per voice); fewer are
        ignored (the last good frame is kept), more are truncated. Pass nullptr
        to clear the provider. */
    void setStateProvider (std::function<std::vector<VoiceActivity>()> provider);

    /** Force a repaint (e.g. after a theme switch). */
    void refresh() { repaint(); }

    /** Number of voices currently active (0..16), for display/accessibility. */
    int getActiveVoiceCount() const noexcept;

    void paint (juce::Graphics&) override;
    void resized() override;

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    // juce::Timer — poll the provider and repaint only when a cell changed.
    void timerCallback() override;

    // Resolve the active theme (via the ancestor ParvatiLookAndFeel), or null.
    const ParvatiTheme* currentTheme() const noexcept;

    // MIDI note number (0..127) -> "C4" style name (60 == C4). Empty if invalid.
    static juce::String midiNoteName (int note);

    // Firmware voicecard layout: vc0={0,1,2} vc1={3,4,5} vc2={6,7,8}
    // vc3={9,10,11} vc4={12,13} vc5={14,15}. True when a wider gap should
    // follow @p voiceIndex (i.e. it is the last voice of its voicecard block).
    static bool isVoicecardBoundary (int voiceIndex);

    static constexpr int kNumVoices = 16;

    // Accessibility: exposes the live active-voice count as a read-only value
    // so screen readers announce e.g. "5 of 16".
    struct VoiceCountInterface;
    friend struct VoiceCountInterface;
    int lastAnnouncedCount_ = -1;

    std::function<std::vector<VoiceActivity>()> provider_;
    std::vector<VoiceActivity> state_;                 // size kNumVoices
    juce::Rectangle<int> labelArea_;                   // "Voices" / count strip
    std::array<juce::Rectangle<int>, kNumVoices> cellRects_ {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoiceMeter)
};

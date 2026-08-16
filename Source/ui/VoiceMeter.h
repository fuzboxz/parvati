// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// VoiceMeter — a compact PART-RELATIVE voice-activity indicator for the Parvati
// UI. The editor supplies one entry per ALLOCATED voice of the current Part
// (0..kMaxCells entries; 0 = a disabled / card-less Part shows an empty strip),
// each cell lighting up in the theme accent when its voice is active, dimming
// to an outline square when free; active cells display the MIDI note name
// (e.g. "C4") when the strip is wide enough. The meter is fed per-frame by the
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

    /** Set the callback the meter polls at ~30 Hz to refresh its cells.
        PART-RELATIVE contract: the frame must contain ONE entry per allocated
        voice of the CURRENT part, in that part's pool order — 0..kMaxCells
        entries. An EMPTY frame is valid (a card-less / disabled Part) and shows
        an all-dim strip; frames larger than kMaxCells are truncated. Pass
        nullptr to clear the provider. */
    void setStateProvider (std::function<std::vector<VoiceActivity>()> provider);

    /** Force a repaint (e.g. after a theme switch). */
    void refresh() { repaint(); }

    // Poll only while on-screen: the meter lives on the Patch page's hosted
    // Global section, so a hidden top page stops its 30 Hz provider passes.
    void visibilityChanged() override;

    /** Number of voices currently active (0..cell count), for
        display/accessibility. */
    int getActiveVoiceCount() const noexcept;

    void paint (juce::Graphics&) override;
    void resized() override;

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    // juce::Timer — poll the provider and repaint only when a cell changed.
    void timerCallback() override;

    // Even single-row layout of cellCount_ cells across the strip. Called from
    // resized() and from timerCallback() when a frame CHANGES SIZE (the part's
    // allocation changed), so the meter reflows without a component resize.
    void layoutCells();

    // Resolve the active theme (via the ancestor ParvatiLookAndFeel), or null.
    const ParvatiTheme* currentTheme() const noexcept;

    // MIDI note number (0..127) -> "C4" style name (60 == C4). Empty if invalid.
    static juce::String midiNoteName (int note);

    // Display cap == the engine's per-Part slot maximum (kMaxVoicesPerPart,
    // SynthEngine.h) DOUBLED: rebuildVoiceAllocation doubles a CHAIN part's
    // voice set (base slots + partner slots), so a part can own up to
    // 2 x kMaxVoicesPerPart voices. Restated here so the component stays
    // decoupled from engine headers (it is fed via the provider callback,
    // nothing else).
    static constexpr int kMaxCells = 32;

    // Number of cells the CURRENT frame carries (0..kMaxCells). The frame size
    // IS the current part's allocated voice count.
    int cellCount_ = 0;

    struct CellState { bool active = false; int note = -1; };
    std::array<CellState, kMaxCells> state_ {};
    std::array<juce::Rectangle<int>, kMaxCells> cellRects_ {};

    // Accessibility: exposes the live active-voice count as a read-only value
    // so screen readers announce e.g. "5 of 8" (cells = the current part's
    // allocation).
    struct VoiceCountInterface;
    friend struct VoiceCountInterface;
    int lastAnnouncedCount_ = -1;

    std::function<std::vector<VoiceActivity>()> provider_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoiceMeter)
};

// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// VoicePoolView — the GLOBAL picture of the 96-voice pool, shown only on the
// Patch page. (The Global-page VoiceMeter is PART-relative by design; this is
// the one place the whole patch is visible at once.) Six compact rows — one
// per Part — each showing that Part's ALLOCATED voices as small squares
// (filled accent = active, outline = idle), a truncated part label, and a
// tiny "active/allocated" count. A top-right band shows the total allocation
// "X/96".
//
// Like VoiceMeter, the view is fed per-frame by the editor at ~30 Hz via a
// state-provider callback and owns nothing from the engine directly (decoupled
// and headless-testable), reads its colours from the ancestor
// ParvatiLookAndFeel's ParvatiTheme (call refresh() after a theme switch), and
// repaints only when a frame actually changed — including frame-SIZE changes
// (a Part's card/slot edit), which re-layout without waiting for a component
// resize.
//
// A CHAIN part can own up to 2 x 16 voices (rebuildVoiceAllocation doubles its
// set); each row DISPLAYS at most 16 squares and summarises the rest with a
// "+N" marker, but every count (per-row and the X/96 total) reflects the FULL
// allocation so the view never contradicts the status strip or the Patch
// page's "Voices Y/96" readout.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "VoiceMeter.h"   // VoiceActivity (one voice's instantaneous state)

#include <array>
#include <functional>
#include <vector>

struct ParvatiTheme;

//==============================================================================
// One Part's slice of a pool frame, supplied each poll by the editor: the
// display label (the part's name/alias or "Part N" — supplied by the EDITOR so
// the view stays engine-decoupled) and one entry per ALLOCATED voice
// (0..16). The vector's SIZE is the part's allocated voice count; an empty
// vector is a valid disabled (card-less) part.
struct VoicePoolPartFrame
{
    juce::String label;
    std::vector<VoiceActivity> voices;   // SIZE = the part's allocated count
};

// A whole frame: all 6 parts, in part order.
struct VoicePoolFrame
{
    std::array<VoicePoolPartFrame, 6> parts;
};

//==============================================================================
class VoicePoolView : public juce::Component,
                      public juce::SettableTooltipClient,
                      private juce::Timer
{
public:
    // Display caps restated from the engine (SynthEngine.h: kNumParts,
    // kMaxVoicesPerPart, kNumVoices) so the component stays decoupled from
    // engine headers — like VoiceMeter::kMaxCells, it is fed via the provider
    // callback and nothing else.
    static constexpr int kNumParts        = 6;
    static constexpr int kMaxCellsPerPart = 16;
    static constexpr int kPoolSize        = kNumParts * kMaxCellsPerPart;   // 96

    VoicePoolView();
    ~VoicePoolView() override;

    /** Set the callback polled at ~30 Hz. The frame must carry all 6 parts in
        part order; each part's voices vector is its ALLOCATION — the SIZE is
        the allocated count (0..16, or up to 32 for a CHAIN part whose set is
        doubled). At most 16 squares per row are DISPLAYED (the rest is
        summarised as "+N"), but ALL counts use the full vector. Parts beyond
        6 are dropped (defensive). Pass nullptr to clear the provider. */
    void setStateProvider (std::function<VoicePoolFrame()> provider);

    /** Test/automation hook: poll the provider once immediately and apply the
        frame (the app drives the same path at ~30 Hz via the timer). */
    void pollNow() { timerCallback(); }

    /** Force a repaint (e.g. after a theme switch). */
    void refresh() { repaint(); }

    // Poll only while on-screen: the view lives on the Patch page, so a
    // hidden top page stops its 30 Hz provider passes.
    void visibilityChanged() override;

    /** The view's fixed height budget (the Patch page sizes it to this): a
        15pt total band + six 13pt part rows with 1pt gaps + insets = 107pt.
        Public so the page and the view share one source of truth. */
    static constexpr int kHeight = 3 + 15 + 3 + kNumParts * 13 + (kNumParts - 1) * 1 + 3;
    /** Total voices currently ACTIVE across all parts (0..96), for
        display/accessibility. */
    int getActiveVoiceCount() const noexcept;

    /** Total voices currently ALLOCATED across all parts (0..96). */
    int getAllocatedVoiceCount() const noexcept;

    void paint (juce::Graphics&) override;
    void resized() override;

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    // juce::Timer — poll the provider and repaint only when something changed.
    void timerCallback() override;

    // Row layout (label column / squares / count column per part). Run from
    // resized() and from timerCallback() when a frame CHANGES SHAPE (a part's
    // allocation or label changed), so the view reflows without a resize.
    void layoutRows();

    // Resolve the active theme (via the ancestor ParvatiLookAndFeel), or null.
    const ParvatiTheme* currentTheme() const noexcept;

    struct PartRow
    {
        juce::String label;
        int  allocated   = 0;      // the part's FULL allocated count (frame size)
        int  cellCount   = 0;      // squares displayed (min(allocated, 16))
        int  activeCount = 0;      // of the ALLOCATED set, currently sounding
        std::array<bool, kMaxCellsPerPart> active {};
        juce::Rectangle<int> rowRect, labelRect, cellsRect, countRect;
    };
    std::array<PartRow, kNumParts> rows_;

    // Accessibility: exposes the live total active-voice count as a read-only
    // value so screen readers announce e.g. "5 of 96" (the pool ceiling).
    struct PoolCountInterface;
    friend struct PoolCountInterface;
    int lastAnnouncedCount_ = -1;

    std::function<VoicePoolFrame()> provider_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoicePoolView)
};

// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// SynthWorkspace — the content of the top-level SYNTH tab. A rigid, void-free
// 3-row integrated panel that hosts the EXISTING, editor-owned ParamPages
// (reparented, NOT regenerated), so every APVTS attachment and the verified
// byte-bridge survive the reorganization unchanged:
//
//   TOP row:    3 columns  [ OSCILLATORS 40% | MIXER 20% | FILTER 40% ]
//       OSCILLATORS = a direct ParamPage (BOTH "Osc 1"/"Osc 2" visible)
//       MIXER / FILTER = direct ParamPages
//   MIDDLE row: full-width CentralModBar (CentralModBar::kBarHeight) — the
//       central hub: click a GENERATOR pill (E1-3 / L1-3 / vLFO / S1-2 / ARP /
//       M1-4) to swap the bottom-left active generator editor; drag ANY pill
//       onto a destination knob to assign it (drag carries the same
//       "parvatiModSrc:<enum>" payload the rest of the editor emits).
//   BOTTOM row: LEFT 50% = the ACTIVE GENERATOR EDITOR (one generator page at
//       a time, chosen by the bar — reparented, never regenerated), RIGHT 50% =
//       the editor-owned ModMatrixView (direct-hosted, no tab bar).
//
// NO per-page juce::Viewport wrappers on the MAIN-ROW pages: each fits its
// cell, so there are ZERO param-panel scrollbars there. The BOTTOM-LEFT
// active-editor host IS a vertical-scroll Viewport, but only as a T4 safety
// net: reflowToWidth grows a fitting page to the full view height, so no
// scrollbar (and no layout change) ever appears at the tuned design size —
// the scrollbar exists solely for short host frames (small AUv3 panes), where
// content previously clipped unrecoverably.
//
// The pages stay owned by ParvatiEditor
// (generatedPages_); the workspace owns only the bar + the active-editor host,
// so reparenting never duplicates a ParamControl / APVTS attachment.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <unordered_map>

#include "CentralModBar.h"

class ModMatrixView;
class ParamPage;
class ThemeManager;

//==============================================================================
class SynthWorkspace : public juce::Component
{
public:
    explicit SynthWorkspace (ThemeManager& themeManager);

    // FX-page top-row padding parity (2026-08-20): the uniform gap taken off
    // ALL FOUR sides of the top row AND placed between its columns (the exact
    // FxWorkspace::kRowGap treatment), so the OSC/MIX/FILTER panels sit in the
    // same generous whitespace the FX cards do instead of butting the row
    // edges and each other. Exposed for the workspace-padding parity test.
    static constexpr int kRowGap = 8;
    int rowPaddingForTest() const noexcept { return kRowGap; }

    // BETWEEN-module gap (2026-08-23 harmonization): the SAME value
    // FxWorkspace::kColGap uses — the inter-module whitespace is identical on
    // both pages so switching SYNTH<->FX never reads as a different rhythm
    // (the outer margins stay kRowGap = 8 everywhere). Pinned EQUAL to
    // FxWorkspace::kColGap by tests/workspace_padding_test.cpp.
    static constexpr int kColGap = 12;
    int moduleGapForTest() const noexcept { return kColGap; }

    // Main-row columns in signal-chain order (OSC | MIX | FILTER at 40/20/40).
    // All three are direct editor-owned pages (reparented, never regenerated).
    void setMainLeft    (ParamPage* page);          // Mixer (direct)
    void setOscillators (ParamPage* page);          // Oscillators (direct; both osc panels visible)
    void setMainRight   (ParamPage* page);          // Filter (direct)

    // ---- Bottom-left: the ACTIVE GENERATOR EDITOR ----
    // Register a generator (a MOD_SRC_* enum whose catalogue entry is a
    // generator, or the bar-only Note Sequencer sentinel) -> { owning
    // ParamPage*, group names shown via setVisibleGroups }. The page stays
    // editor-owned; the workspace reparents it into the active-editor host when
    // its generator is selected (NEVER regenerated). An EMPTY @p groupNames
    // array shows ALL of the page's groups (e.g. ARP). A multi-element array
    // reveals several groups at once (e.g. the Note Sequencer reveals both its
    // "Note Pitch" and "Note Velocity" groups).
    void registerGeneratorPage (int modSrcEnum, ParamPage* page,
                                const juce::StringArray& groupNames);

    // Show the registered generator's page (reparent + setVisibleGroups) and
    // highlight its bar pill. No-op if @p modSrcEnum is not a registered
    // generator. Called from the bar's pill-click handler (generators) and once
    // at startup to set the default (Env 1).
    void setActiveGenerator (int modSrcEnum);

    // Detach the currently-active generator page from this workspace's
    // active-editor host (non-owned: removeChildComponent, never deleted) and
    // forget it. Used by the editor on a Synth<->FX toggle so the SHARED page
    // re-parents cleanly into the newly-visible FxWorkspace (a JUCE Component
    // can only have one parent).
    void releaseActiveEditor();

    // Drag-only (Perf / Util / Const) pill click — the editor registers a handler
    // that briefly highlights the mod-matrix rows currently routed FROM that
    // source (ModMatrixView::flashRowsForSource), reusing the existing timed
    // flash. Generators do NOT reach this handler (they swap the editor instead).
    void setOnDragOnlyPillClicked (std::function<void (int)> cb);

    // Fired from setActiveGenerator whenever the active generator selection moves
    // (a generator pill click). The editor uses it to track the SHARED active
    // generator so a Synth<->FX toggle reparents the right page into the
    // newly-visible FxWorkspace.
    void setOnActiveGeneratorChanged (std::function<void (int)> cb);

    // Host an editor-owned ModMatrixView as the BOTTOM-RIGHT panel (direct child,
    // non-owned — the editor retains ownership, exactly like the reparented
    // ParamPages). The view paints + lays out its own rows in its resized().
    void setModMatrixView (ModMatrixView* view);

    void resized() override;
    void paint (juce::Graphics&) override;

    // Re-apply theme colours to the main pages + the bar + the active editor
    // page + the ModMatrixView. Called by the editor on a theme switch.
    void applyThemeColors();

    // The workspace-owned CentralModBar (never null after construction).
    // Live-modulation feedback (docs/LIVE_MOD_FEEDBACK_DESIGN.md): the editor
    // uses this seam to bind the bar's telemetry provider + refresh rate —
    // the bar itself stays self-contained (no provider = no strips).
    CentralModBar* modBar() const noexcept { return modBar_.get(); }

    // The bar's no-clipping ideal width (CentralModBar::preferredWidth).
    // Diagnostic only since R3: the bar scrolls horizontally inside its own
    // Viewport, so the editor's width floor is a fixed 1024pt (tablets) and
    // the default size no longer tracks this value — it reports the
    // uncompressed ideal width (e.g. for tests).
    int barPreferredWidth() const;

    /** Show/hide the central mod-pill bar seam. Hiding COLLAPSES the bar row
        (its height goes to the top (synth/fx) row only — the bottom row keeps
        its size; see resized()); it does NOT
        tear the bar down, so re-showing is a cheap relayout. Mirrored by
        FxWorkspace so SYNTH<->FX never reflows on the difference. */
    void setModBarVisible (bool visible)
    {
        modBarVisible_ = visible;
        resized();
    }

    // MOD-BAR TOP RULE — the separator above the middle seam (see the ctor).
    // Test hook: its bounds + visibility.
    juce::Rectangle<int> barRuleBoundsForTest() const
    { return barRule_ != nullptr ? barRule_->getBounds() : juce::Rectangle<int>(); }
    bool barRuleVisibleForTest() const { return barRule_ != nullptr && barRule_->isVisible(); }

private:
    ThemeManager& themeManager_;
    // (member declarations below)


    // Main-row direct pages (Oscillators / Mixer / Filter) — all shown directly.
    ParamPage* mainOscPage_   = nullptr;    // Oscillators (direct; BOTH osc panels visible)
    ParamPage* mainLeftPage_  = nullptr;    // Mixer (direct)
    ParamPage* mainRightPage_ = nullptr;    // Filter (direct)

    // MIDDLE seam: the full-width Central Modulation Bar (workspace-owned).
    std::unique_ptr<CentralModBar> modBar_;
    // The middle seam's top separator (parvati::ChromeRule; ui/ChromeRule.h).
    // Held as unique_ptr<Component> to avoid pulling the header into every
    // consumer — SynthWorkspace.cpp owns the type via its include.
    std::unique_ptr<juce::Component> barRule_;
    bool modBarVisible_ = true;   // [MOD] header toggle state (bar shown by default)

    // BOTTOM-LEFT: the vertical-scroll host that reparents ONE generator page
    // at a time. A Viewport SAFETY NET (see the class comment): no scrollbar
    // when the page fits its cell — reflowToWidth grows the page to at least
    // the view height — and a vertical scrollbar only in short host frames,
    // where the page previously clipped unrecoverably. The reparented page
    // stays editor-owned (generatedPages_); the host owns only its layout
    // slot, so reparenting never duplicates a control/attachment.
    // TOP-row scroll host (R3): holds the three main-row pages; viewed by
    // topRowViewport_ so a short frame scrolls instead of overlapping the
    // bar/bottom rows. No scrollbar when the pages fit (design size).
    std::unique_ptr<juce::Component> topRowHost_;
    std::unique_ptr<juce::Viewport> topRowViewport_;

    std::unique_ptr<juce::Viewport> activeEditorHost_;    ParamPage* activePage_ = nullptr;   // page currently reparented into the host

    // Generator -> { page, groups-to-show } registration (built by the editor from
    // the page-generation loop; one entry per generator pill).
    struct GenEntry { ParamPage* page = nullptr; juce::StringArray groups; };
    std::unordered_map<int, GenEntry> generators_;

    // Editor-supplied handler for a drag-only (Perf/Util/Const) pill click.
    std::function<void (int)> onDragOnlyPillClicked_;

    // Editor-supplied handler fired when the active generator selection moves.
    std::function<void (int)> onActiveGenChanged_;

    // BOTTOM-RIGHT: the editor-owned ModMatrixView (direct-hosted, non-owned).
    ModMatrixView* modMatrixView_ = nullptr;

    // Reparent + setVisibleGroups + size the registered generator's page into the
    // active-editor host (the page is never regenerated). Called for generator
    // pills; the bar pill highlight is handled separately by setActiveGenerator.
    void showGenerator (int modSrcEnum);

    // Reflow the currently-active page into the host's current bounds. Called
    // from resized() (and after a generator swap) so the page follows resizes.
    void reflowActiveEditor();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SynthWorkspace)
};

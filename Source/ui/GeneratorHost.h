// Copyright (c) 2026 805Labs Kft. / Hellcat.
//
// GeneratorHostWorkspace — the shared skeleton of the SYNTH and FX tabs.
// Both workspaces mirror each other line for line:
//
//   TOP row:    the workspace's own modules (SynthWorkspace: OSC | MIX |
//               FILTER; FxWorkspace: ROUTING | FX1 | FX2 | FX3).
//   MIDDLE row: a full-width CentralModBar seam plus its top rule. Each
//               workspace owns its OWN bar instance (the same pill set; the
//               modulators come from the synth).
//   BOTTOM row: LEFT 50% = the ACTIVE GENERATOR EDITOR (a vertical-scroll
//               Viewport that reparents ONE editor-owned ParamPage at a
//               time), RIGHT 50% = the workspace's matrix view.
//
// This base owns every piece the two tabs share: the bar + rule, both
// viewport hosts, the generator-page registry and reparenting, the drag-only
// pill handler, the three-row height math, the bar-seam and bottom-row
// layout, and the background fill. Each subclass keeps ONLY its top row.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <unordered_map>

#include "CentralModBar.h"

class ParamPage;
class ThemeManager;

//==============================================================================
class GeneratorHostWorkspace : public juce::Component
{
public:
    explicit GeneratorHostWorkspace (ThemeManager& themeManager);

    // ---- Generator-page registry (the bottom-left active editor) ----
    // Register a generator (a MOD_SRC_* enum whose catalogue entry is a
    // generator, or the bar-only Note Sequencer sentinel) -> { owning
    // ParamPage*, group names shown via setVisibleGroups }. The page stays
    // editor-owned; the workspace reparents it into the active-editor host
    // when its generator is selected (NEVER regenerated). The SAME generator
    // pages are registered in BOTH workspaces (shared editor), so a SYNTH<->
    // FX toggle reparents a single active selection between the two hosts.
    void registerGeneratorPage (int modSrcEnum, ParamPage* page,
                                const juce::StringArray& groupNames);

    // Show the registered generator's page (reparent + setVisibleGroups) and
    // highlight its bar pill. No-op if @p modSrcEnum is not a registered
    // generator. Called from the bar's pill-click handler (generators) and by
    // the editor to set or restore the active generator.
    void setActiveGenerator (int modSrcEnum);

    // Detach the active generator page from this workspace's
    // active-editor host (non-owned: removeChildComponent, never deleted) and
    // forget it. Used by the editor on a SYNTH<->FX toggle so the SHARED page
    // re-parents cleanly into the newly-visible workspace (a JUCE Component
    // can only have one parent — without this the outgoing workspace's stale
    // activePage_ would skip re-adding the page on the return toggle).
    void releaseActiveEditor();

    // Drag-only (Perf / Util / Const) pill click — the editor registers a
    // handler that briefly highlights the matrix rows now routed FROM that
    // source. Generators do NOT reach this handler (they swap the editor).
    void setOnDragOnlyPillClicked (std::function<void (int)> cb);

    // Fired from setActiveGenerator whenever the active generator selection
    // moves (a generator pill click). The editor uses it to track the SHARED
    // active generator so a SYNTH<->FX toggle reparents the right page into
    // the newly-visible workspace.
    void setOnActiveGeneratorChanged (std::function<void (int)> cb);

    /** Show/hide the central mod-pill bar seam. Hiding COLLAPSES the bar row
        (its height goes to the top row — the bottom row keeps its size; see
        splitRows()); it does NOT tear the bar down, so re-showing is a cheap
        relayout. Both workspaces behave identically, so SYNTH<->FX never
        reflows on the difference. */
    void setModBarVisible (bool visible)
    {
        modBarVisible_ = visible;
        resized();
    }

    // The workspace-owned CentralModBar (never null after construction).
    // Live-modulation feedback (docs/LIVE_MOD_FEEDBACK_DESIGN.md): the editor
    // uses this seam to bind the bar's telemetry provider + refresh rate —
    // the bar itself stays self-contained (no provider = no strips).
    CentralModBar* modBar() const noexcept { return modBar_.get(); }

    // MOD-BAR TOP RULE — the separator above the middle seam (see the ctor).
    // Test hook: its bounds + visibility.
    juce::Rectangle<int> barRuleBoundsForTest() const
    { return barRule_ != nullptr ? barRule_->getBounds() : juce::Rectangle<int>(); }
    bool barRuleVisibleForTest() const { return barRule_ != nullptr && barRule_->isVisible(); }

    // Flat windowBackground fill so any integer-division remainder between
    // the rigid cells (or a short page) never bleeds the default component
    // colour. Shared by both workspaces.
    void paint (juce::Graphics&) override;

protected:
    // ---- The shared three-row split (resized() step 1) ----
    // The bottom row keeps the height it would have WITH the bar shown and is
    // capped at @p bottomRowMaxH (each subclass passes its matrix view's
    // exactly-4-rows cap); everything else — including the freed bar strip
    // when [MOD] hides the seam — goes to the TOP row, so toggling the pill
    // bar grows the top section only and the bottom section keeps its size.
    struct RowHeights { int main, bar, bottom; };
    RowHeights splitRows (const juce::Rectangle<int>& area, int bottomRowMaxH) const;

    // ---- The bar seam + its top rule (resized() step 3) ----
    // Full workspace width at the bar's top edge; both hidden with the bar.
    void layoutBarSeam (const juce::Rectangle<int>& barRow);

    // ---- The bottom row (resized() step 4) ----
    // LEFT 50% = the active-editor host (then reflowed), RIGHT 50% = the
    // matrix view (non-owned; its resized() lays out its own rows).
    void layoutBottomRow (juce::Rectangle<int> bottomRow, juce::Component* matrixView);

    ThemeManager& themeManager_;

    // MIDDLE seam: the full-width Central Modulation Bar (workspace-owned).
    std::unique_ptr<CentralModBar> modBar_;
    // The middle seam's top separator (hellcat::ChromeRule; ui/ChromeRule.h).
    // Held as unique_ptr<Component> to avoid pulling the header into every
    // consumer — GeneratorHost.cpp owns the type via its include.
    std::unique_ptr<juce::Component> barRule_;
    bool modBarVisible_ = true;   // [MOD] header toggle state (bar shown by default)

    // TOP-row scroll host (R3): holds the workspace's own modules; viewed by
    // topRowViewport_ so a short frame scrolls the row instead of overlapping
    // the bar/bottom rows. No scrollbar when the modules fit (design size).
    std::unique_ptr<juce::Component> topRowHost_;
    std::unique_ptr<juce::Viewport> topRowViewport_;

    // BOTTOM-LEFT: the vertical-scroll host that reparents ONE generator page
    // at a time. A Viewport SAFETY NET (T4): no scrollbar when the page fits
    // its cell — reflowActiveEditor grows the page to at least the view
    // height — and a vertical scrollbar only in short host frames, where the
    // page previously clipped unrecoverably. The reparented page stays
    // editor-owned; the host owns only its layout slot, so reparenting never
    // duplicates a control or an APVTS attachment.
    std::unique_ptr<juce::Viewport> activeEditorHost_;
    ParamPage* activePage_ = nullptr;   // page now reparented into the host

    // Generator -> { page, groups-to-show } registration (built by the editor
    // from the page-generation loop; one entry per generator pill).
    struct GenEntry { ParamPage* page = nullptr; juce::StringArray groups; };
    std::unordered_map<int, GenEntry> generators_;

    // Editor-supplied handler for a drag-only (Perf/Util/Const) pill click.
    std::function<void (int)> onDragOnlyPillClicked_;

    // Editor-supplied handler fired when the active generator selection moves.
    std::function<void (int)> onActiveGenChanged_;

private:
    // Reparent + setVisibleGroups + size the registered generator's page into
    // the active-editor host (the page is never regenerated). Called for
    // generator pills; the bar pill highlight is handled by setActiveGenerator.
    void showGenerator (int modSrcEnum);

    // Reflow the active page into the host's current bounds. Called
    // from resized() (and after a generator swap) so the page follows resizes.
    void reflowActiveEditor();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GeneratorHostWorkspace)
};

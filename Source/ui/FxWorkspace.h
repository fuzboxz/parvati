// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// FxWorkspace — the content of the top-level FX tab. A rigid, void-free 3-row
// integrated panel that mirrors SynthWorkspace's skeleton, hosting EDITOR-OWNED
// ParamPages (reparented, NOT regenerated) so every APVTS attachment and the
// verified byte-bridge survive unchanged:
//
//   TOP row:    4 columns [ ROUTING | FX1 | FX2 | FX3 ]. A slim FxRoutingBar
//               column (topology dropdown + global MIX + master EQ) on
//               the left, then 3 equal-width columns of self-contained
//               FxSlotCards (FX1/FX2/FX3) — each a modular card (power/bypass
//               toggle + type combo + a per-algorithm visualizer + a param knob
//               grid with the dry/wet anchored rightmost). Every column gets the
//               FULL top-row height (synth-page OSC/MIXER/FILTER parity), so the
//               knobs reach their 52px synth-parity dial. The cards are
//               borderless sibling panels (containerFill, no outline).
//   MIDDLE row: full-width CentralModBar (CentralModBar::kBarHeight) — the SAME
//               source set as the synth (modulators come from the synth). Click a
//               GENERATOR pill (E1-3 / L1-3 / vLFO / S1-2 / ARP / M1-4) to swap
//               the bottom-left active generator editor; drag ANY pill onto a
//               destination knob to assign it.
//   BOTTOM row: LEFT 50% = the ACTIVE GENERATOR EDITOR (the SAME editor-owned
//               generator ParamPages shared with SynthWorkspace — reparented
//               between the two workspaces on a Synth<->FX toggle, never
//               regenerated), RIGHT 50% = the editor-owned FxMatrixView.
//
// The FX-slot order / series-parallel topology controls live on the full-width
// FxRoutingBar (fx_topo / fx_order params); each FxSlotCard binds its own
// fx{N}_type / fx{N}_enabled / params. It is a structural clone of the synth
// layout's row skeleton; the card visual design is tuned here.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <unordered_map>

#include "CentralModBar.h"

class FxMatrixView;
class FxRoutingBar;
class FxSlotCard;
class ParamPage;
class ThemeManager;

//==============================================================================
class FxWorkspace : public juce::Component
{
public:
    explicit FxWorkspace (ThemeManager& themeManager);

    // The SPACIOUS top-row gap: taken off ALL FOUR sides of the top row AND
    // placed between its four columns (routing + 3 FX cards), so the
    // borderless card panels sit in generous whitespace. SynthWorkspace
    // mirrors this constant for synth-page header-padding parity (2026-08-20;
    // pinned equal by tests/workspace_padding_test.cpp).
    static constexpr int kRowGap = 8;
    int rowPaddingForTest() const noexcept { return kRowGap; }

    // TOP-row FX-slot cards: one self-contained FxSlotCard per slot (0..2),
    // reparented directly (never regenerated). setFxSlotCard(slot, card).
    void setFxSlotCard (int slot, FxSlotCard* card);

    /** Show/hide the central mod-pill bar seam (mirrors SynthWorkspace so
        SYNTH<->FX never reflows on the difference). */
    void setModBarVisible (bool visible)
    {
        modBarVisible_ = visible;
        resized();
    }

    // TOP-row slim ROUTING column (FLOW topology dropdown + global MIX +
    // master EQ), now the leftmost of the 4-column top row. Editor-owned,
    // hosted NON-owned.
    void setFxRoutingBar (FxRoutingBar* bar);

    // ---- Bottom-left: the ACTIVE GENERATOR EDITOR (shared with SynthWorkspace) ----
    // Register a generator (a MOD_SRC_* enum whose catalogue entry is a
    // generator, or the bar-only Note Sequencer sentinel) -> { owning
    // ParamPage*, group names shown via setVisibleGroups }. The page stays
    // editor-owned; the workspace reparents it into the active-editor host when
    // its generator is selected (NEVER regenerated). The SAME generator pages
    // are registered here as in SynthWorkspace (shared editor), so the mode
    // toggle reparents a single active selection between the two workspaces.
    void registerGeneratorPage (int modSrcEnum, ParamPage* page,
                                const juce::StringArray& groupNames);

    // Show the registered generator's page (reparent + setVisibleGroups) and
    // highlight its bar pill. No-op if @p modSrcEnum is not a registered
    // generator.
    void setActiveGenerator (int modSrcEnum);

    // Detach the currently-active generator page from this workspace's
    // active-editor host (non-owned: removeChildComponent, never deleted) and
    // forget it. Used by the editor on a Synth<->FX toggle so the SHARED page
    // re-parents cleanly into the newly-visible workspace (a JUCE Component can
    // only have one parent — without this the outgoing workspace's stale
    // activePage_ would skip re-adding the page on the return toggle).
    void releaseActiveEditor();

    // Drag-only (Perf / Util / Const) pill click — the editor registers a handler
    // that briefly highlights the FX-matrix rows currently routed FROM that source
    // (FxMatrixView::flashRowsForSource). Generators do NOT reach this handler.
    void setOnDragOnlyPillClicked (std::function<void (int)> cb);

    // Fired from setActiveGenerator whenever the active generator selection moves
    // (a generator pill click). The editor uses it to track the SHARED active
    // generator so a Synth<->FX toggle reparents the right page into the
    // newly-visible workspace.
    void setOnActiveGeneratorChanged (std::function<void (int)> cb);

    // Host an editor-owned FxMatrixView as the BOTTOM-RIGHT panel (direct child,
    // non-owned — the editor retains ownership, exactly like the reparented
    // ParamPages). The view paints + lays out its own rows in its resized().
    void setFxMatrixView (FxMatrixView* view);

    void resized() override;
    void paint (juce::Graphics&) override;

    // Re-apply theme colours to the slot pages + the bar + the active editor
    // page + the FxMatrixView. Called by the editor on a theme switch.
    void applyThemeColors();

    // The bar's no-clipping minimum width (CentralModBar::preferredWidth).
    int barPreferredWidth() const;

private:
    ThemeManager& themeManager_;

    // TOP-row direct FX-slot cards (FX1/FX2/FX3) — all shown directly.
    FxSlotCard* fxSlotCards_[3] { nullptr, nullptr, nullptr };

    // TOP-row full-width FX routing header bar (editor-owned, NON-owned host).
    FxRoutingBar* fxRoutingBar_ = nullptr;

    // MIDDLE seam: the full-width Central Modulation Bar (workspace-owned).
    std::unique_ptr<CentralModBar> modBar_;
    bool modBarVisible_ = true;   // [MOD] header toggle state (bar shown by default)

    // BOTTOM-LEFT: the vertical-scroll host that reparents ONE generator page
    // at a time. A Viewport SAFETY NET mirroring SynthWorkspace (T4): no
    // scrollbar when the page fits its cell — reflowToWidth grows the page to
    // at least the view height — and a vertical scrollbar only in short host
    // frames, where the page previously clipped unrecoverably. The reparented
    // page stays editor-owned (generatedPages_); the host owns only its layout
    // slot, so reparenting never duplicates a control/attachment.
    // TOP-row scroll host (R3): holds the routing bar + slot cards; viewed by
    // topRowViewport_ so a compacted frame scrolls the row at its natural
    // minimum height instead of overlapping the rows below.
    std::unique_ptr<juce::Component> topRowHost_;
    std::unique_ptr<juce::Viewport> topRowViewport_;

    std::unique_ptr<juce::Viewport> activeEditorHost_;
    ParamPage* activePage_ = nullptr;   // page currently reparented into the host

    // Generator -> { page, groups-to-show } registration (built by the editor from
    // the page-generation loop; one entry per generator pill).
    struct GenEntry { ParamPage* page = nullptr; juce::StringArray groups; };
    std::unordered_map<int, GenEntry> generators_;

    // Editor-supplied handler for a drag-only (Perf/Util/Const) pill click.
    std::function<void (int)> onDragOnlyPillClicked_;

    // Editor-supplied handler fired when the active generator selection moves.
    std::function<void (int)> onActiveGenChanged_;

    // BOTTOM-RIGHT: the editor-owned FxMatrixView (direct-hosted, non-owned).
    FxMatrixView* fxMatrixView_ = nullptr;

    // Reparent + setVisibleGroups + size the registered generator's page into the
    // active-editor host (the page is never regenerated).
    void showGenerator (int modSrcEnum);

    // Reflow the currently-active page into the host's current bounds. Called
    // from resized() (and after a generator swap) so the page follows resizes.
    void reflowActiveEditor();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxWorkspace)
};

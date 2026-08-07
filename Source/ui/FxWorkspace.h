// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// FxWorkspace — the content of the top-level FX tab. A rigid, void-free 3-row
// integrated panel that mirrors SynthWorkspace's skeleton, hosting EDITOR-OWNED
// ParamPages (reparented, NOT regenerated) so every APVTS attachment and the
// verified byte-bridge survive unchanged:
//
//   TOP row:    3 equal columns  [ FX1 | FX2 | FX3 ] — one ParamPage per FX
//               slot (the fx1_/fx2_/fx3_ descriptors), reparented directly.
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
// The FX-slot order / series-parallel topology controls live on the FX-slot
// ParamPages themselves (fx_order / fx_topo params), so this workspace adds no
// bespoke reorder UI. The placeholder is deliberately a structural clone of the
// synth layout; visual design can be tuned later.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <unordered_map>

#include "CentralModBar.h"

class FxMatrixView;
class ParamPage;
class ThemeManager;

//==============================================================================
class FxWorkspace : public juce::Component
{
public:
    explicit FxWorkspace (ThemeManager& themeManager);

    // TOP-row FX-slot columns: one editor-owned ParamPage per slot (0..2),
    // reparented directly (never regenerated). setFxSlotPage(slot, page).
    void setFxSlotPage (int slot, ParamPage* page);

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

    // Drag-only (Perf / Util / Const) pill click — the editor registers a handler
    // that briefly highlights the FX-matrix rows currently routed FROM that source
    // (FxMatrixView::flashRowsForSource). Generators do NOT reach this handler.
    void setOnDragOnlyPillClicked (std::function<void (int)> cb);

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

    // TOP-row direct FX-slot pages (FX1/FX2/FX3) — all shown directly.
    ParamPage* fxSlotPages_[3] { nullptr, nullptr, nullptr };

    // MIDDLE seam: the full-width Central Modulation Bar (workspace-owned).
    std::unique_ptr<CentralModBar> modBar_;

    // BOTTOM-LEFT: a plain host that reparents ONE generator page at a time. The
    // reparented page stays editor-owned (generatedPages_); the host owns only
    // its layout slot, so reparenting never duplicates a control/attachment.
    std::unique_ptr<juce::Component> activeEditorHost_;
    ParamPage* activePage_ = nullptr;   // page currently reparented into the host

    // Generator -> { page, groups-to-show } registration (built by the editor from
    // the page-generation loop; one entry per generator pill).
    struct GenEntry { ParamPage* page = nullptr; juce::StringArray groups; };
    std::unordered_map<int, GenEntry> generators_;

    // Editor-supplied handler for a drag-only (Perf/Util/Const) pill click.
    std::function<void (int)> onDragOnlyPillClicked_;

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

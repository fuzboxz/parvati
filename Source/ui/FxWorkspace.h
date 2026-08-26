// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.
//
// FxWorkspace — the content of the top-level FX tab. A rigid, void-free 3-row
// integrated panel that mirrors SynthWorkspace's skeleton, hosting EDITOR-OWNED
// ParamPages (reparented, NOT regenerated) so every APVTS attachment and the
// checked byte-bridge survive unchanged:
//
//   TOP row:    4 columns [ ROUTING | FX1 | FX2 | FX3 ]. A slim FxRoutingBar
//               column (topology dropdown + global MIX + master EQ) on the
//               left, then 3 equal-width columns of self-contained
//               FxSlotCards (FX1/FX2/FX3) — each a modular card (power/bypass
//               toggle + type combo + a per-algorithm visualizer + a param knob
//               grid with the dry/wet anchored rightmost). Every column gets the
//               FULL top-row height (synth-page OSC/MIXER/FILTER parity), so
//               the knobs reach their 52px synth-parity dial. The cards are
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
// The bar seam, both viewport hosts, the generator-page registry and the
// three-row split live in the shared GeneratorHostWorkspace base (see
// ui/GeneratorHost.h). THIS class owns only the FX top row.
//
// The FX-slot order / series-parallel topology controls live on the full-width
// FxRoutingBar (fx_topo / fx_order params); each FxSlotCard binds its own
// fx{N}_type / fx{N}_enabled / params. It is a structural clone of the synth
// layout's row skeleton; the card visual design is tuned here.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "GeneratorHost.h"

class FxMatrixView;
class FxRoutingBar;
class FxSlotCard;

//==============================================================================
class FxWorkspace : public GeneratorHostWorkspace
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

    // Inter-module whitespace of the FX top row (2026-08-23 user request:
    // "a tiny bit more between the FX modules for visual clarity") — a few
    // px WIDER than the outer row margin, which stays kRowGap (synth parity
    // pins kRowGap equality, NOT the column gap — the synth page keeps its
    // kRowGap column spacing; the FX modules are denser cards and read
    // better with the wider separation). Pinned by workspace_padding_test [3].
    static constexpr int kColGap = kRowGap + 4;   // 12 between routing/FX1/FX2/FX3

    // FIXED top-row module heights (2026-08-23): the routing bar and the
    // FX-slot cards never stretch with the window — they keep these
    // content-natural heights, pin to the TOP of the row (synth-page parity:
    // extra frame height shows page background BELOW the modules, exactly
    // how a taller synth viewport leaves background under its group cards;
    // a shorter frame scrolls at the kTopRowNaturalH floor), and the cards
    // carry the user's +20px "spacious" bump over the routing bar. Pinned
    // by tests/workspace_padding_test.cpp [3].
    static constexpr int kRouteModuleH = 224;   // routing bar (content-natural)
    static constexpr int kCardModuleH  = 244;   // FX-slot cards (routing + 20)

    // OUTER margin of the FX module row (2026-08-23 harmonization): 16 ==
    // the SYNTH page's effective outer whitespace (workspace kRowGap 8 +
    // ParamPage::kMargin 8) — "the FX page global container has as much
    // whitespace as the synth page". The BETWEEN-module gap stays kColGap
    // (12, the tuned value). Pinned by tests/workspace_padding_test.cpp [3].
    static constexpr int kOuterMargin = 16;

    // TOP-row FX-slot cards: one self-contained FxSlotCard per slot (0..2),
    // reparented directly (never regenerated). setFxSlotCard(slot, card).
    void setFxSlotCard (int slot, FxSlotCard* card);

    // TOP-row slim ROUTING column (FLOW topology dropdown + global MIX +
    // master EQ), now the leftmost of the 4-column top row. Editor-owned,
    // hosted NON-owned.
    void setFxRoutingBar (FxRoutingBar* bar);

    // Host an editor-owned FxMatrixView as the BOTTOM-RIGHT panel (direct child,
    // non-owned — the editor retains ownership, exactly like the reparented
    // ParamPages). The view paints + lays out its own rows in its resized().
    void setFxMatrixView (FxMatrixView* view);

    void resized() override;

    // Re-apply theme colours to the slot pages + the bar + the active editor
    // page + the FxMatrixView. Called by the editor on a theme switch.
    void applyThemeColors();

private:
    // TOP-row direct FX-slot cards (FX1/FX2/FX3) — all shown directly.
    FxSlotCard* fxSlotCards_[3] { nullptr, nullptr, nullptr };

    // TOP-row full-width FX routing header bar (editor-owned, NON-owned host).
    FxRoutingBar* fxRoutingBar_ = nullptr;

    // BOTTOM-RIGHT: the editor-owned FxMatrixView (direct-hosted, non-owned).
    FxMatrixView* fxMatrixView_ = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxWorkspace)
};

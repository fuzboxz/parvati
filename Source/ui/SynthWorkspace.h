// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// SynthWorkspace — the content of the top-level SYNTH tab. A rigid, void-free
// 3-row integrated panel that hosts the EXISTING, editor-owned ParamPages
// (reparented, NOT regenerated), so every APVTS attachment and the checked
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
// The bar seam, both viewport hosts, the generator-page registry and the
// three-row split live in the shared GeneratorHostWorkspace base (see
// ui/GeneratorHost.h). THIS class owns only the SYNTH top row.
//
// NO per-page juce::Viewport wrappers on the MAIN-ROW pages: each fits its
// cell, so there are ZERO param-panel scrollbars there. The BOTTOM-LEFT
// active-editor host IS a vertical-scroll Viewport, but only as a T4 safety
// net (see the base class).
//
// The pages stay owned by ParvatiEditor
// (generatedPages_); the workspace owns only the bar + the active-editor host,
// so reparenting never duplicates a ParamControl / APVTS attachment.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "GeneratorHost.h"

class ModMatrixView;
class ParamPage;

//==============================================================================
class SynthWorkspace : public GeneratorHostWorkspace
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

    // Host an editor-owned ModMatrixView as the BOTTOM-RIGHT panel (direct child,
    // non-owned — the editor retains ownership, exactly like the reparented
    // ParamPages). The view paints + lays out its own rows in its resized().
    void setModMatrixView (ModMatrixView* view);

    void resized() override;

    // Re-apply theme colours to the main pages + the bar + the active editor
    // page + the ModMatrixView. Called by the editor on a theme switch.
    void applyThemeColors();

private:
    // Main-row direct pages (Oscillators / Mixer / Filter) — all shown directly.
    ParamPage* mainOscPage_   = nullptr;    // Oscillators (direct; BOTH osc panels visible)
    ParamPage* mainLeftPage_  = nullptr;    // Mixer (direct)
    ParamPage* mainRightPage_ = nullptr;    // Filter (direct)

    // BOTTOM-RIGHT: the editor-owned ModMatrixView (direct-hosted, non-owned).
    ModMatrixView* modMatrixView_ = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SynthWorkspace)
};

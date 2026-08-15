// HIG sizing-contract test. The iOS STYLE is now the SINGLE default UI on every
// platform (no more desktop #else branches), so these layout constants are the
// same value everywhere. This test pins that unified contract — a future edit
// that drifts a constant fails CI here. Asserted STATIC (compile-time) AND
// mirrored as runtime checks.
//
//   cmake --build build --target parvati_ipad_hig_sizing_test
//   ./build/parvati_ipad_hig_sizing_test

#include <cstdio>

#include <juce_core/juce_core.h>

#include "PluginEditor.h"          // ParvatiEditor (kHeaderH/kBarHeight) + ParamPage (kMargin)
#include "ui/CentralModBar.h"      // kBarHeight / kPillH / kPillGap
#include "ui/ModMatrixView.h"      // kRowHeight
#include "ui/FxMatrixView.h"       // kRowHeight

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}
}

// ---- Compile-time contract (the primary guard) ----
// Unified values (iOS style is the default on ALL platforms).
static_assert (ParvatiEditor::kHeaderH   == 44, "header height must be 44 (HIG)");
static_assert (ParvatiEditor::kBarHeight == 44, "header icon strip must be 44 (HIG targets)");
static_assert (ParamPage::kMargin        == 8,  "page margin must be 8 (HIG spacing)");
static_assert (CentralModBar::kBarHeight == 92, "mod-bar height must be 92 (hosts 72pt pills + a coloured label tab + nav arrows)");
static_assert (CentralModBar::kPillH     == 72, "pill height must be 72 (~1.5x bigger blips)");
static_assert (CentralModBar::kPillGap   == 8,  "pill gap must be 8 (HIG spacing)");
static_assert (ModMatrixView::kRowHeight == 48, "mod-matrix row height must be 48");
static_assert (FxMatrixView::kRowHeight  == 48, "FX-matrix row height must be 48");

int main()
{
    std::printf ("[HIG sizing contract] unified UI (single style, all platforms)\n");

    check (ParvatiEditor::kHeaderH    == 44, "header height == 44");
    check (ParvatiEditor::kBarHeight  == 44, "header icon strip == 44");
    check (ParamPage::kMargin         == 8,  "page margin == 8");
    check (CentralModBar::kBarHeight  == 92, "mod-bar height == 92");
    check (CentralModBar::kPillH      == 72, "pill height == 72");
    check (CentralModBar::kPillGap    == 8,  "pill gap == 8");
    check (ModMatrixView::kRowHeight  == 48, "mod-matrix row == 48");
    check (FxMatrixView::kRowHeight   == 48, "FX-matrix row == 48");

    std::printf ("\n%s (%d failures)\n",
                 g_failures ? "HIG SIZING TEST: FAILURES" : "HIG SIZING TEST: ALL CHECKS PASSED",
                 g_failures);
    return g_failures ? 1 : 0;
}

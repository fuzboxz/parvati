// HIG sizing-contract test (iPad M5). Asserts the iOS layout constants equal the
// Apple HIG touch minimums (44pt targets, 48pt matrix rows, 8pt spacing) under an
// iOS build, and that the desktop constants are UNCHANGED. Guards the
// compile-time-gated sizing contract so a future edit that drifts a constant (or
// accidentally widens it onto desktop) fails CI here.
//
// The asserts are STATIC (compile-time) AND mirrored as runtime checks, so the
// contract is enforced both when the test compiles and when it runs.
//
//   cmake --build build --target parvati_ipad_hig_sizing_test
//   ./build/parvati_ipad_hig_sizing_test
// (On iOS, configure the build_ios tree first; the test target is built the same
//  way and asserts the iOS values.)

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
#if JUCE_IOS
static_assert (ParvatiEditor::kHeaderH  == 44, "iOS header height must be 44 (HIG)");
static_assert (ParvatiEditor::kBarHeight == 44, "iOS header icon strip must be 44 (HIG targets)");
static_assert (ParamPage::kMargin       == 8,  "iOS page margin must be 8 (HIG spacing)");
static_assert (CentralModBar::kBarHeight == 58, "iOS mod-bar height must be 58 (hosts 50pt pills)");
static_assert (CentralModBar::kPillH    == 50, "iOS pill height must be 50 (HIG target)");
static_assert (CentralModBar::kPillGap  == 8,  "iOS pill gap must be 8 (HIG spacing)");
static_assert (ModMatrixView::kRowHeight == 48, "iOS mod-matrix row height must be 48");
static_assert (FxMatrixView::kRowHeight  == 48, "iOS FX-matrix row height must be 48");
#else
// Desktop must be UNCHANGED by the HIG work (every change is #if JUCE_IOS).
static_assert (ParvatiEditor::kHeaderH  == 40, "desktop header height unchanged");
static_assert (ParvatiEditor::kBarHeight == 34, "desktop header icon strip unchanged");
static_assert (ParamPage::kMargin       == 10, "desktop page margin unchanged");
static_assert (CentralModBar::kBarHeight == 38, "desktop mod-bar height unchanged");
static_assert (ModMatrixView::kRowHeight == 34, "desktop mod-matrix row height unchanged");
static_assert (FxMatrixView::kRowHeight  == 34, "desktop FX-matrix row height unchanged");
#endif

int main()
{
    std::printf ("[HIG sizing contract] platform = %s\n",
#if JUCE_IOS
                 "iOS"
#else
                 "desktop"
#endif
                 );

#if JUCE_IOS
    check (ParvatiEditor::kHeaderH    == 44, "header height == 44");
    check (ParvatiEditor::kBarHeight  == 44, "header icon strip == 44");
    check (ParamPage::kMargin         == 8,  "page margin == 8");
    check (CentralModBar::kBarHeight  == 58, "mod-bar height == 58");
    check (CentralModBar::kPillH      == 50, "pill height == 50");
    check (CentralModBar::kPillGap    == 8,  "pill gap == 8");
    check (ModMatrixView::kRowHeight  == 48, "mod-matrix row == 48");
    check (FxMatrixView::kRowHeight   == 48, "FX-matrix row == 48");
#else
    check (ParvatiEditor::kHeaderH    == 40, "desktop header unchanged (40)");
    check (ParvatiEditor::kBarHeight  == 34, "desktop header strip unchanged (34)");
    check (ParamPage::kMargin         == 10, "desktop page margin unchanged (10)");
    check (CentralModBar::kBarHeight  == 38, "desktop mod-bar unchanged (38)");
    check (ModMatrixView::kRowHeight  == 34, "desktop mod-matrix row unchanged (34)");
    check (FxMatrixView::kRowHeight   == 34, "desktop FX-matrix row unchanged (34)");
#endif

    std::printf ("\n%s (%d failures)\n",
                 g_failures ? "HIG SIZING TEST: FAILURES" : "HIG SIZING TEST: ALL CHECKS PASSED",
                 g_failures);
    return g_failures ? 1 : 0;
}

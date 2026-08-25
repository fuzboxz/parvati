// HIG sizing-contract test. The iOS STYLE is now the SINGLE default UI on every
// platform (no more desktop #else branches), so these layout constants are the
// same value everywhere. This test pins that unified contract — a future edit
// that drifts a constant fails CI here. Asserted STATIC (compile-time) AND
// mirrored as runtime checks.
//
//   ./build_unified/parvati_unified_tests ipad_hig_sizing_test

#include <cstdio>
#include "unified_test_runner.h"

#include <juce_core/juce_core.h>

#include "PluginEditor.h"          // ParvatiEditor (kHeaderH/kBarHeight) + ParamPage (kMargin)
#include "ui/CentralModBar.h"      // kBarHeight / kPillH / kPillGap
#include "ui/ModMatrixView.h"      // kRowHeight / kAddButtonH
#include "ui/FxMatrixView.h"       // kRowHeight / kAddButtonH
#include "ui/FxSlotCard.h"         // kPowerHitSize (FX power-toggle hit area)
#include "ui/FxRoutingBar.h"       // kStepBtnW/kStepBtnH/kEqKnobSize (routing-bar targets)
#include "ui/WheelsComponent.h"    // kOctBtnSize (wheels octave-switch targets)
#include "ui/ParvatiLookAndFeel.h" // kPopupRowHeight (default popup rows)
#include "ui/SeqLengthStepper.h" // kPopupRowHeight (the seq-length picker rows)

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
// kHeaderH grew 44 -> 48 -> 52: room for the 22pt wordmark + the border
// stack (the dark cast shadow now falls BELOW the border into the content
// clearance). The 44pt HIG target lives in kBarHeight (the control strip
// stays 44); the band chrome adds 8px of depth.
static_assert (ParvatiEditor::kHeaderH   == 52, "header height must be 52 (44pt HIG strip + 8px chrome depth)");
static_assert (ParvatiEditor::kBarHeight == 44, "header icon strip must be 44 (HIG targets)");
static_assert (ParamPage::kMargin        == 8,  "page margin must be 8 (HIG spacing)");
static_assert (CentralModBar::kBarHeight == 86, "mod-bar height must be 86 (8px sep clearance + label tab 14 + gap 4 + 56pt pills + 4px bottom inset, 2026-08-21)");
static_assert (CentralModBar::kPillH     == 56, "pill height must be 56 (compact pills; still >= 44pt HIG touch target — the bar seam is collapsible via [MOD])");
static_assert (CentralModBar::kPillGap   == 8,  "pill gap must be 8 (HIG spacing)");
static_assert (ModMatrixView::kRowHeight == 48, "mod-matrix row height must be 48");
static_assert (FxMatrixView::kRowHeight  == 48, "FX-matrix row height must be 48");
static_assert (FxSlotCard::kPowerHitSize == 44, "FX power-toggle hit area must be 44 (HIG)");
static_assert (FxRoutingBar::kStepBtnW   == 44, "FX topology steppers must be 44 wide (HIG)");
static_assert (FxRoutingBar::kStepBtnH   == 44, "FX topology steppers must be 44 tall (HIG)");
static_assert (FxRoutingBar::kEqKnobSize == 44, "FX master-EQ dial must be 44 (HIG)");
static_assert (ModMatrixView::kAddButtonH == 44, "mod-matrix Add button must be 44 tall (HIG)");
static_assert (FxMatrixView::kAddButtonH  == 44, "FX-matrix Add button must be 44 tall (HIG)");
static_assert (ParvatiLookAndFeel::kPopupRowHeight == 44, "default popup-menu rows must be 44 (HIG)");
static_assert (WheelsComponent::kOctBtnSize == 44, "wheels octave-switch buttons must be 44 (HIG)");

TEST(ipad_hig_sizing_test)
{
    std::printf ("[HIG sizing contract] unified UI (single style, all platforms)\n");

    check (ParvatiEditor::kHeaderH    == 52, "header height == 52 (44 HIG strip + chrome depth)");
    check (ParvatiEditor::kBarHeight  == 44, "header icon strip == 44");
    check (ParamPage::kMargin         == 8,  "page margin == 8");
    check (CentralModBar::kBarHeight  == 86, "mod-bar height == 86");
    check (CentralModBar::kPillH      == 56, "pill height == 56");
    check (CentralModBar::kPillGap    == 8,  "pill gap == 8");
    check (ModMatrixView::kRowHeight  == 48, "mod-matrix row == 48");
    check (FxMatrixView::kRowHeight   == 48, "FX-matrix row == 48");
    check (FxSlotCard::kPowerHitSize  == 44, "FX power-toggle hit == 44");
    check (FxRoutingBar::kStepBtnW    == 44, "FX topology stepper width == 44");
    check (FxRoutingBar::kStepBtnH    == 44, "FX topology stepper height == 44");
    check (FxRoutingBar::kEqKnobSize  == 44, "FX master-EQ dial == 44");
    check (ModMatrixView::kAddButtonH == 44, "mod-matrix Add button == 44");
    check (FxMatrixView::kAddButtonH  == 44, "FX-matrix Add button == 44");
    check (ParvatiLookAndFeel::kPopupRowHeight == 44, "default popup-menu row == 44");
    check (CentralModBar::kNavHitW            == 44, "mod-bar nav scroller hit band == 44 (F-ios-touch-1)");
    check (SeqLengthStepper::kPopupRowHeight  == 44, "seq-length picker row == 44 (F-ios-touch-2)");
    check (WheelsComponent::kOctBtnSize == 44, "wheels octave-switch button == 44");

    std::printf ("\n%s (%d failures)\n",
                 g_failures ? "HIG SIZING TEST: FAILURES" : "HIG SIZING TEST: ALL CHECKS PASSED",
                 g_failures);
    return g_failures == 0;
}

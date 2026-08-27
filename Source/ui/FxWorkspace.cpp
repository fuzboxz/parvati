// Copyright (c) 2026 805Labs Kft. / Hellcat.  See FxWorkspace.h.

#include "FxWorkspace.h"

#include "FxMatrixView.h"
#include "FxRoutingBar.h"
#include "FxSlotCard.h"
#include "ParamPage.h"        // ParamPage complete type (activePage_ theming)

//==============================================================================
FxWorkspace::FxWorkspace (ThemeManager& themeManager)
    : GeneratorHostWorkspace (themeManager)
{
}

//==============================================================================
void FxWorkspace::setFxSlotCard (int slot, FxSlotCard* card)
{
    if (slot < 0 || slot >= 3)
        return;
    // Direct child (editor-owned card, reparented not regenerated).
    fxSlotCards_[slot] = card;
    if (card != nullptr)
        topRowHost_->addAndMakeVisible (*card);
}

void FxWorkspace::setFxRoutingBar (FxRoutingBar* bar)
{
    // Host the editor-owned FX routing header bar directly as the TOP strip of
    // the upper region (NON-owned — the editor retains ownership, exactly like
    // the slot cards + the FxMatrixView). addAndMakeVisible reparents without
    // transferring ownership, so the teardown order (fxWorkspace_ before
    // fxRoutingBar_) stays safe.
    fxRoutingBar_ = bar;
    if (bar != nullptr)
        topRowHost_->addAndMakeVisible (*bar);
}

void FxWorkspace::setFxMatrixView (FxMatrixView* view)
{
    // Host the editor-owned FxMatrixView directly as the BOTTOM-RIGHT panel
    // (single content, no tab bar). NON-owned (the editor retains ownership via
    // fxMatrixView_): addAndMakeVisible reparents without transferring
    // ownership, exactly like the reparented ParamPages, so the teardown order
    // (fxWorkspace_ before fxMatrixView_) stays safe.
    fxMatrixView_ = view;
    if (view != nullptr)
        addAndMakeVisible (*view);
}

//==============================================================================
namespace
{
// Bottom-row cap: exactly 4 rows visible in the bottom-right matrix view -
// 2*4 outer inset + 22 header + 4 gap + 4 first-row inset + 4 * (row + gap).
// The rows scroll inside the view's own Viewport, so a longer routing list
// stays reachable; the freed height goes to the top (synth/fx) row.
constexpr int kBottomRowMaxH = 8 + 22 + 4 + 4 + 4 * (FxMatrixView::kRowHeight + 4);
}

void FxWorkspace::resized()
{
    auto area = getLocalBounds();
    if (area.isEmpty())
        return;

    // ---- 3 rows: TOP (slots) | MIDDLE (bar) | BOTTOM (generators | matrix) ----
    // The shared splitRows() caps the bottom row and gives every freed strip
    // (including the hidden bar) to the TOP row (see GeneratorHost.h).
    const auto rows   = splitRows (area, kBottomRowMaxH);
    auto mainRow      = area.removeFromTop (rows.main);
    auto barRow       = area.removeFromTop (rows.bar);
    auto bottomRow    = area;   // exactly rows.bottom (main consumes the rest)

    // ---- Upper region: 4 columns [ ROUTING | FX1 | FX2 | FX3 ] ----
    // The ROUTING column (FLOW topology + MIX + master EQ) on the left, then
    // three equal-width FX-slot cards. The routing column is sized for its
    // flow diagram, not "slim": the [◀][diagram][▶] row costs 2x44pt steppers
    // + 2x6pt bar padding, and the Series diagram needs midW >= 3x22pt blocks
    // + 4x2pt gaps + IN 20 + OUT 26 + 8pt frame inset = 228pt of column —
    // below that the blocks clamp to their 22pt floor and the last one juts
    // into OUT (the "ruined diagram" look). The 232pt floor renders Series
    // cleanly (22pt blocks, 3pt gaps); 19% grows it toward the comfortable
    // 3x40pt-block layout, capped at 288pt. SPACIOUS layout (synth-page
    // parity): a uniform kGap margin is taken off ALL FOUR sides of the top row
    // AND placed between every column, so the borderless card panels sit in
    // generous whitespace (page backgroundBase) instead of butting each other
    // and the workspace edges — mirroring how the synth page's kMargin insets
    // its GroupComponent cards. FIXED MODULE HEIGHTS (2026-08-23 user
    // request — "all FX module heights fixed, like FX Routing"): the modules
    // keep their content-natural FIXED heights (kRouteModuleH for the routing
    // bar, kCardModuleH = +20px for the cards — the follow-up exempted the
    // routing bar from the spaciousness bump) and pin to the row's TOP like
    // the synth page's controls: a taller frame leaves page background BELOW
    // the modules (NO vertical centring — the user explicitly rejected the
    // centred band), a shorter frame still scrolls at the kTopRowNaturalH
    // floor. The BETWEEN-module whitespace is kColGap (kRowGap + 4,
    // 2026-08-23: "a tiny bit more for visual clarity").
    constexpr int kGap = kOuterMargin;   // OUTER margins (all four sides; synth-page effective whitespace parity — see header)
    constexpr int kBetweenCols = FxWorkspace::kColGap;   // BETWEEN modules (the class constant — the wider tuned gap)
    // R3: the top row's NATURAL height — derived from the TALLEST fixed
    // module (the cards' kCardModuleH) plus the uniform kGap margins. The
    // viewport host is never laid shorter than this; a shorter FRAME scrolls
    // instead of overlapping the rows below.
    constexpr int kTopRowNaturalH = kCardModuleH + 2 * kGap;
    topRowViewport_->setBounds (mainRow);
    // mainRow (NOT Viewport::getViewWidth()) is the width source: the viewport
    // caches its visible area and can be stale mid-cascade (SynthWorkspace hit
    // this and collapsed its first layout to the 150pt floor).
    const int viewW = juce::jmax (0, mainRow.getWidth());
    const int viewH = juce::jmax (kTopRowNaturalH, mainRow.getHeight());
    auto layoutTopRow = [&] (int w)
    {
        // Column budget (RIGHT-MARGIN FIX, 2026-08-23 — "the right edge of
        // the FX3 module is truncated"): BOTH outer margins and the three
        // kColGap column gaps come off the top, so the columns end at
        // w - kGap by construction. The old fx3 width formula (x0 + w - kGap
        // - fx3x) double-counted the left inset and left fx3 FLUSH with the
        // host edge — combined with the (until now missing) scrollbar pass
        // below, the overlay scrollbar truncated the card.
        // 232pt floor = the diagram's no-overlap Series minimum (see the
        // column comment above); 288pt cap keeps the cards roomy on wide
        // frames. At the 1024pt editor floor: routeW 232 -> cards ~238pt
        // each (well above the ~200pt card comfort floor).
        const int innerW  = juce::jmax (0, w - 2 * kGap - 3 * kBetweenCols);
        const int routeW  = juce::jlimit (232, 288, innerW * 19 / 100);
        const int cardW   = juce::jmax (0, (innerW - routeW) / 3);

        const int x0 = kGap;
        // TOP-PINNED (synth-page parity, 2026-08-23 user follow-up: the
        // centred band was rejected): the modules start at the top margin at
        // ANY frame height — a taller window shows page background BELOW
        // them (like a taller synth viewport), never a re-centred band.
        const int y0 = kGap;
        const int routeH0 = kRouteModuleH;   // FIXED heights (see header)
        const int cardH0  = kCardModuleH;

        const juce::Rectangle<int> routeCol (x0, y0, routeW, routeH0);
        const juce::Rectangle<int> fx1Col   (x0 + routeW + kBetweenCols,                  y0, cardW, cardH0);
        const juce::Rectangle<int> fx2Col   (x0 + routeW + kBetweenCols + cardW + kBetweenCols, y0, cardW, cardH0);
        const int fx3x = x0 + routeW + 3 * kBetweenCols + 2 * cardW;   // 3 gaps between 4 columns (ROUTE/FX1/FX2/FX3)
        // fx3 absorbs the cardW division remainder (0..2px) so the row ends
        // EXACTLY at the right margin — never past it.
        const juce::Rectangle<int> fx3Col   (fx3x, y0, juce::jmax (0, w - kGap - fx3x), cardH0);

        if (fxRoutingBar_ != nullptr)
            fxRoutingBar_->setBounds (routeCol);

        auto sizeCard = [] (FxSlotCard* card, const juce::Rectangle<int>& b)
        {
            if (card != nullptr)
                card->setBounds (b);
        };
        sizeCard (fxSlotCards_[0], fx1Col);
        sizeCard (fxSlotCards_[1], fx2Col);
        sizeCard (fxSlotCards_[2], fx3Col);
    };
    layoutTopRow (viewW);
    // SCROLLBAR-AWARE RE-LAYOUT (2026-08-23 — the other half of the fx3
    // truncation): the top viewport scrolls vertically when the frame is
    // shorter than kTopRowNaturalH, and the overlay scrollbar then covers the
    // right ~14pt of the host — fx3's freshly restored right margin. The
    // SynthWorkspace precedent: re-lay the columns one scrollbar-thickness
    // NARROWER when the host will scroll, so every module + its whitespace
    // stays fully visible; the host keeps the full width (the freed strip is
    // empty page background behind the scrollbar).
    if (viewH > mainRow.getHeight())
        layoutTopRow (juce::jmax (0, viewW - topRowViewport_->getScrollBarThickness()));
    topRowHost_->setSize (viewW, viewH);

    // ---- Middle seam + bottom row: the shared helpers (GeneratorHost.h) ----
    layoutBarSeam (barRow);
    layoutBottomRow (bottomRow, fxMatrixView_);
}

//==============================================================================
void FxWorkspace::applyThemeColors()
{
    if (fxRoutingBar_ != nullptr)
        fxRoutingBar_->applyThemeColors();

    for (auto* card : fxSlotCards_)
        if (card != nullptr)
            card->applyThemeColors();

    if (modBar_ != nullptr)
        modBar_->applyThemeColors();

    // The active generator page re-themes here; non-active pages are themed by
    // the editor's generatedPages_ pass in applyAllColoursFromTheme.
    if (activePage_ != nullptr)
        activePage_->applyThemeColors();

    if (fxMatrixView_ != nullptr)
        fxMatrixView_->applyThemeColors();

    repaint();
}

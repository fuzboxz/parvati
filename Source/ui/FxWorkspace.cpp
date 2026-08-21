// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See FxWorkspace.h.

#include "FxWorkspace.h"

#include "PluginEditor.h"   // ParamPage complete type
#include "FxMatrixView.h"
#include "FxRoutingBar.h"
#include "FxSlotCard.h"
#include "ModSourceCatalog.h"   // parvati::entryFor (generator vs drag-only)
#include "ChromeRule.h"      // parvati::ChromeRule (the mod-bar top rule)
#include "ThemeManager.h"

//==============================================================================
FxWorkspace::FxWorkspace (ThemeManager& tm)
    : themeManager_ (tm)
{
    // The full-width Central Modulation Bar (middle seam). FX owns its OWN bar
    // instance (same pill set as the synth — modulators come from the synth); the
    // generator ParamPages are SHARED editor-owned (registered below), not
    // duplicated.
    modBar_ = std::make_unique<CentralModBar> (themeManager_);
    addAndMakeVisible (*modBar_);
    // MOD-BAR TOP RULE (2026-08-21 fix — see SynthWorkspace for the full
    // rationale): line at the bar's top edge, falloff shading the empty 8px
    // top pad; the old shadowBelow=false drew it 6px into the bar over the
    // label tabs. One ChromeRule family with the editor chrome.
    barRule_ = std::make_unique<parvati::ChromeRule> (true);
    addAndMakeVisible (*barRule_);
    barRule_->setVisible (false);   // laid out only while the seam is shown

    // TOP row: the routing bar + three FX-slot cards live in a vertical-scroll
    // Viewport host (R3). The host keeps a fixed NATURAL minimum height
    // (kTopRowNaturalH) so the bar/cards always lay out at their designed row
    // heights; at the tuned design size the row already meets that minimum so
    // no scrollbar ever appears (layout byte-identical). A compacted frame
    // SCROLLS the row instead of the cards/bar painting over the mod bar and
    // the bottom rows (the reported overlap).
    topRowHost_ = std::make_unique<juce::Component>();
    topRowViewport_ = std::make_unique<juce::Viewport>();
    topRowViewport_->setScrollBarsShown (true, false, false, false);   // vertical-only, shown only on overflow
    topRowViewport_->setViewedComponent (topRowHost_.get(), false);
    addAndMakeVisible (*topRowViewport_);

    // BOTTOM-LEFT: a vertical-scroll host (T4 safety net) that reparents one
    // generator page at a time. Mirrors SynthWorkspace's host exactly: no
    // scrollbar when the page fits its cell (reflowToWidth grows the page to
    // at least the view height, so the layout is byte-identical to the old
    // plain host at the tuned design size), a vertical scrollbar only in short
    // host frames (previously unrecoverable clipping). Desktop mouse drags
    // never scroll-on-drag (default ScrollOnDragMode::nonHover is touch-only);
    // the mouse WHEEL scrolls (knob wheels are disabled and juce bubbles the
    // unhandled wheel up to the Viewport); a TOUCH drag on a control does not
    // scroll (ParamControl sets the viewport ignore-drag flag), on the page
    // background it does.
    activeEditorHost_ = std::make_unique<juce::Viewport>();
    activeEditorHost_->setScrollBarsShown (true, false, false, false);   // vertical-only, shown only when the page overflows
    addAndMakeVisible (*activeEditorHost_);

    // Wire the bar's pill clicks. A GENERATOR pill (catalogue isGenerator) swaps
    // the bottom-left active editor to its page+group AND lights the pill's
    // underline glow. A drag-only pill (Perf/Util/Const) delegates to the
    // editor-registered handler (FxMatrixView row flash for rows routed from
    // that source). The drag itself (any pill) is handled inside the bar and
    // needs no wiring here — it carries the "parvatiModSrc:<enum>" payload.
    modBar_->setOnPillClicked ([this] (int src)
    {
        // Tap-to-assign mode: a pill tap selects this mod source for the next
        // dest tap and SUPPRESSES the generator-page flip (assign mode is
        // focused). Bar-only sentinels (src < 0, e.g. the Note Sequencer) are
        // never a valid mod source and are skipped.
        if (ParamControl::tapAssignActive())
        {
            if (src >= 0)
                ParamControl::setTapSelectedSource (src);
            return;
        }
        if (parvati::entryFor (src).isGenerator)
            setActiveGenerator (src);
        else if (onDragOnlyPillClicked_)
            onDragOnlyPillClicked_ (src);
    });
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

void FxWorkspace::registerGeneratorPage (int modSrcEnum, ParamPage* page,
                                         const juce::StringArray& groupNames)
{
    generators_[modSrcEnum] = { page, groupNames };
}

void FxWorkspace::setOnDragOnlyPillClicked (std::function<void (int)> cb)
{
    onDragOnlyPillClicked_ = std::move (cb);
}

void FxWorkspace::setOnActiveGeneratorChanged (std::function<void (int)> cb)
{
    onActiveGenChanged_ = std::move (cb);
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
void FxWorkspace::showGenerator (int modSrcEnum)
{
    const auto it = generators_.find (modSrcEnum);
    if (it == generators_.end() || it->second.page == nullptr)
        return;

    auto* page = it->second.page;

    // Reparent the page into the active-editor host (the page is NEVER
    // regenerated — only its parent + visible-group subset change). Because
    // the SAME page is shared with SynthWorkspace, reparenting it here detaches
    // it from the other workspace's host on a Synth<->FX toggle (single active
    // selection — no double-parent: a JUCE Component can only have one parent).
    // The Viewport API (delete-on-remove = false) only reparents, never
    // deletes, and resets the scroll to the top on every swap.
    if (activePage_ != page)
    {
        activeEditorHost_->setViewedComponent (page, false);
        activePage_ = page;
    }

    // Show just this generator's group subset (EMPTY array => ALL groups, e.g.
    // ARP). The stored array is passed through unchanged.
    page->setVisibleGroups (it->second.groups);

    reflowActiveEditor();
}

void FxWorkspace::setActiveGenerator (int modSrcEnum)
{
    showGenerator (modSrcEnum);
    modBar_->setActiveGenerator (modSrcEnum);   // underline-glow the pill
    if (onActiveGenChanged_)
        onActiveGenChanged_ (modSrcEnum);
}

void FxWorkspace::releaseActiveEditor()
{
    if (activePage_ != nullptr)
    {
        // Detach via the Viewport API (non-owned: nullptr with the same
        // delete-on-remove = false used at attach, so the page is only
        // reparented away, never deleted).
        activeEditorHost_->setViewedComponent (nullptr, false);
        activePage_ = nullptr;
    }
}

void FxWorkspace::reflowActiveEditor()
{
    if (activePage_ == nullptr)
        return;
    const auto b = activeEditorHost_->getLocalBounds();
    if (b.isEmpty())
        return;
    // Anchor at the Viewport origin (setViewedComponent resets the scroll, and
    // setSize below preserves the top-left, so this is only load-bearing on the
    // first layout after a swap).
    activePage_->setTopLeftPosition (0, 0);
    // Full view width first: a page that FITS keeps the old plain-host layout
    // (reflowToWidth grows it to at least the view height — no scrollbar, no
    // width change). Only an overflowing page is re-laid one
    // scrollbar-thickness narrower so the scrollbar never covers the right
    // edge (mirrors SynthWorkspace / the FxMatrixView precedent).
    activePage_->reflowToWidth (juce::jmax (150, b.getWidth()), juce::jmax (0, b.getHeight()));
    if (activePage_->getHeight() > b.getHeight())
        activePage_->reflowToWidth (juce::jmax (150, b.getWidth()
                                                      - activeEditorHost_->getScrollBarThickness()),
                                    juce::jmax (0, b.getHeight()));
}

//==============================================================================
int FxWorkspace::barPreferredWidth() const
{
    return modBar_ != nullptr ? modBar_->preferredWidth() : 0;
}

//==============================================================================
void FxWorkspace::paint (juce::Graphics& g)
{
    // Flat windowBackground fill so any integer-division remainder between the
    // rigid cells (or a short page) never bleeds the default component colour.
    g.fillAll (themeManager_.getCurrentTheme().backgroundBase);
}

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
    // The bottom row keeps the height it would have WITH the bar shown and is
    // capped at kBottomRowMaxH (exactly 4 matrix rows + chrome); everything
    // else - including the freed bar strip when [MOD] hides the seam - goes to
    // the TOP row, so toggling the pill bar grows the fx section only (the
    // matrix + generator-editor bottom section keeps its size). Mirrored with
    // SynthWorkspace so switching SYNTH<->FX never reflows on the difference.
    constexpr float kBottomShare = 0.60f;
    const int withBarH = juce::jmax (0, area.getHeight() - CentralModBar::kBarHeight);
    const int bottomH = juce::jmin (juce::roundToInt (static_cast<float> (withBarH) * kBottomShare),
                                    kBottomRowMaxH);
    const int barH = modBarVisible_ ? CentralModBar::kBarHeight : 0;
    const int mainH = juce::jmax (0, area.getHeight() - barH - bottomH);

    auto mainRow  = area.removeFromTop (mainH);
    auto barRow   = area.removeFromTop (barH);
    auto bottomRow = area;   // exactly bottomH (mainH consumes the rest)

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
    // its GroupComponent cards. Each card gets the remaining height and sizes
    // its knobs internally. (kRowGap is the class constant; the former local
    // kGap was hoisted for the parity test.)
    constexpr int kGap = kRowGap;
    // R3: the top row's NATURAL height — the routing bar's stacked rows (flow
    // diagram 50 + EQ 60 + Dry/Wet band) need ~190px, and the FX-slot cards'
    // FIXED-height knob grid (2 x kCellH = 140px of grid + header 16 + type row
    // 44 + gaps 4 + card padding 12 = 216px) needs 216px + the host's 2*kGap
    // margins = 232px for FULL-SIZE knobs; 240 keeps a small breathing margin.
    // (The per-slot VISUALIZER band was REMOVED 2026-08-20 — 264 became 240;
    // the freed 24px flows to the workspace rows below.) The viewport host is
    // never laid shorter than this; a shorter FRAME scrolls instead of
    // starving the rows (which previously made the EQ labels / Dry/Wet caption
    // / stepper buttons paint outside the bar and over the rows below — and,
    // pre-2026-08, shrank the FX knob cells themselves). Knob-size stability
    // parity with the synth pages (ParamPage::reflowToWidth scrolls the same
    // way).
    constexpr int kTopRowNaturalH = 240;
    topRowViewport_->setBounds (mainRow);
    // mainRow (NOT Viewport::getViewWidth()) is the width source: the viewport
    // caches its visible area and can be stale mid-cascade (SynthWorkspace hit
    // this and collapsed its first layout to the 150pt floor).
    const int viewW = juce::jmax (0, mainRow.getWidth());
    const int viewH = juce::jmax (kTopRowNaturalH, mainRow.getHeight());
    auto layoutTopRow = [&] (int w)
    {
        // 232pt floor = the diagram's no-overlap Series minimum (see the
        // column comment above); 288pt cap keeps the cards roomy on wide
        // frames. At the 1024pt editor floor: routeW 232 -> cards 768/3 = 256pt
        // each (well above the ~200pt card comfort floor).
        const int routeW = juce::jlimit (232, 288, (w - 3 * kGap) * 19 / 100);
        const int cardsRegionW = juce::jmax (0, w - routeW - 3 * kGap);
        const int cardW = cardsRegionW / 3;

        const int x0 = kGap, y0 = kGap;
        const int h0 = viewH - 2 * kGap;

        const juce::Rectangle<int> routeCol (x0, y0, routeW, h0);
        const juce::Rectangle<int> fx1Col   (x0 + routeW + kGap,                    y0, cardW, h0);
        const juce::Rectangle<int> fx2Col   (x0 + routeW + kGap + cardW + kGap,     y0, cardW, h0);
        const int fx3x = x0 + routeW + 3 * kGap + 2 * cardW;   // 3 gaps between 4 columns (ROUTE/FX1/FX2/FX3)
        const juce::Rectangle<int> fx3Col   (fx3x, y0, juce::jmax (0, x0 + w - kGap - fx3x), h0);

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
    topRowHost_->setSize (viewW, viewH);

    // ---- Middle seam: full-width bar ----
    modBar_->setVisible (modBarVisible_);
    if (modBarVisible_)
        modBar_->setBounds (barRow);
    // The seam's top separator (matches SynthWorkspace exactly).
    if (barRule_ != nullptr)
    {
        barRule_->setVisible (modBarVisible_);
        if (modBarVisible_)
            barRule_->setBounds (barRow.getX(), barRow.getY(),
                                 barRow.getWidth(), 1 + parvati::kRuleShadowH);
    }

    // ---- Bottom row: LEFT 50% = active editor, RIGHT 50% = FxMatrixView ----
    auto modLeft  = bottomRow.removeFromLeft (bottomRow.getWidth() / 2);
    auto modRight = bottomRow;

    activeEditorHost_->setBounds (modLeft);
    reflowActiveEditor();

    if (fxMatrixView_ != nullptr)
        fxMatrixView_->setBounds (modRight);   // its resized() lays out the rows
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

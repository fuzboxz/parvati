// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See FxWorkspace.h.

#include "FxWorkspace.h"

#include "PluginEditor.h"   // ParamPage complete type
#include "FxMatrixView.h"
#include "FxRoutingBar.h"
#include "FxSlotCard.h"
#include "ModSourceCatalog.h"   // parvati::entryFor (generator vs drag-only)
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

    // BOTTOM-LEFT: a plain host that reparents one generator page at a time.
    activeEditorHost_ = std::make_unique<juce::Component>();
    addAndMakeVisible (*activeEditorHost_);

    // Wire the bar's pill clicks. A GENERATOR pill (catalogue isGenerator) swaps
    // the bottom-left active editor to its page+group AND lights the pill's
    // underline glow. A drag-only pill (Perf/Util/Const) delegates to the
    // editor-registered handler (FxMatrixView row flash for rows routed from
    // that source). The drag itself (any pill) is handled inside the bar and
    // needs no wiring here — it carries the "parvatiModSrc:<enum>" payload.
    modBar_->setOnPillClicked ([this] (int src)
    {
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
        addAndMakeVisible (*card);
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
        addAndMakeVisible (*bar);
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
    // regenerated — only its parent + visible-group subset change). The prior
    // page is detached (non-owned: removeChildComponent, never deleted). Because
    // the SAME page is shared with SynthWorkspace, reparenting it here detaches
    // it from the other workspace's host on a Synth<->FX toggle (single active
    // selection — no double-parent: a JUCE Component can only have one parent).
    if (activePage_ != page)
    {
        if (activePage_ != nullptr)
            activeEditorHost_->removeChildComponent (activePage_);
        activeEditorHost_->addAndMakeVisible (*page);
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
        activeEditorHost_->removeChildComponent (activePage_);
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
    activePage_->setBounds (b);
    activePage_->reflowToWidth (juce::jmax (150, b.getWidth()), juce::jmax (0, b.getHeight()));
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

void FxWorkspace::resized()
{
    auto area = getLocalBounds();
    if (area.isEmpty())
        return;

    // ---- 3 rows: TOP (slots) | MIDDLE (bar) | BOTTOM (generators | matrix) ----
    // Mirrors SynthWorkspace: the bar is a fixed-height full-width seam; the
    // remaining height splits evenly between the top main row and the bottom row.
    const int remaining = juce::jmax (0, area.getHeight() - CentralModBar::kBarHeight);
    const int mainH = remaining / 2;

    auto mainRow  = area.removeFromTop (mainH);
    auto barRow   = area.removeFromTop (CentralModBar::kBarHeight);
    auto bottomRow = area;   // remaining (mainH or mainH + 1px remainder)

    // ---- Upper region: 4 columns [ ROUTING | FX1 | FX2 | FX3 ] ----
    // A slim ROUTING column (FLOW topology + MIX + keep-tails) on the left, then
    // three equal-width FX-slot cards. SPACIOUS layout (synth-page parity): a
    // uniform kGap margin is taken off ALL FOUR sides of the top row AND placed
    // between every column, so the borderless card panels sit in generous
    // whitespace (page backgroundBase) instead of butting each other and the
    // workspace edges — mirroring how the synth page's kMargin insets its
    // GroupComponent cards. Each card gets the remaining height and sizes its
    // knobs/visualizer internally.
    constexpr int kGap = 10;
    auto inner = mainRow.reduced (kGap);
    if (! inner.isEmpty())
    {
        const int innerW = inner.getWidth();
        const int routeW = juce::jlimit (210, 320, (innerW - 3 * kGap) * 20 / 100);
        const int cardsRegionW = juce::jmax (0, innerW - routeW - 3 * kGap);
        const int cardW = cardsRegionW / 3;

        const int x0 = inner.getX();
        const int y0 = inner.getY();
        const int h0 = inner.getHeight();

        const juce::Rectangle<int> routeCol (x0, y0, routeW, h0);
        const juce::Rectangle<int> fx1Col   (x0 + routeW + kGap,                    y0, cardW, h0);
        const juce::Rectangle<int> fx2Col   (x0 + routeW + kGap + cardW + kGap,     y0, cardW, h0);
        const int fx3x = x0 + routeW + 2 * kGap + 2 * cardW;
        const juce::Rectangle<int> fx3Col   (fx3x, y0, juce::jmax (0, x0 + innerW - fx3x), h0);

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
    }

    // ---- Middle seam: full-width bar ----
    modBar_->setBounds (barRow);

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

// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See SynthWorkspace.h.

#include "SynthWorkspace.h"

#include "PluginEditor.h"   // ParamPage complete type
#include "ModMatrixView.h"
#include "ModSourceCatalog.h"   // parvati::entryFor (generator vs drag-only)
#include "ThemeManager.h"

//==============================================================================
SynthWorkspace::SynthWorkspace (ThemeManager& tm)
    : themeManager_ (tm)
{
    // The full-width Central Modulation Bar (middle seam).
    modBar_ = std::make_unique<CentralModBar> (themeManager_);
    addAndMakeVisible (*modBar_);

    // BOTTOM-LEFT: a plain host that reparents one generator page at a time.
    activeEditorHost_ = std::make_unique<juce::Component>();
    addAndMakeVisible (*activeEditorHost_);

    // Wire the bar's pill clicks. A GENERATOR pill (catalogue isGenerator) swaps
    // the bottom-left active editor to its page+group AND lights the pill's
    // underline glow. A drag-only pill (Perf/Util/Const) delegates to the
    // editor-registered handler (ModMatrixView row flash for rows routed from
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
void SynthWorkspace::setMainLeft (ParamPage* page)
{
    // Mixer — direct child (editor-owned page, reparented not regenerated).
    mainLeftPage_ = page;
    if (page != nullptr)
        addAndMakeVisible (*page);
}

void SynthWorkspace::setOscillators (ParamPage* page)
{
    // Oscillators — shown DIRECTLY (both "Osc 1"/"Osc 2" visible; an empty
    // visibleGroups_ set => all groups render). The page stays editor-owned
    // (reparented, never regenerated).
    mainOscPage_ = page;
    if (page != nullptr)
        addAndMakeVisible (*page);
}

void SynthWorkspace::setMainRight (ParamPage* page)
{
    // Filter — direct child (editor-owned page, reparented not regenerated).
    mainRightPage_ = page;
    if (page != nullptr)
        addAndMakeVisible (*page);
}

void SynthWorkspace::registerGeneratorPage (int modSrcEnum, ParamPage* page,
                                            const juce::StringArray& groupNames)
{
    generators_[modSrcEnum] = { page, groupNames };
}

void SynthWorkspace::setOnDragOnlyPillClicked (std::function<void (int)> cb)
{
    onDragOnlyPillClicked_ = std::move (cb);
}

void SynthWorkspace::setModMatrixView (ModMatrixView* view)
{
    // Host the editor-owned ModMatrixView directly as the BOTTOM-RIGHT panel
    // (single content, no tab bar). NON-owned (the editor retains ownership via
    // modMatrixView_): addAndMakeVisible reparents without transferring
    // ownership, exactly like the reparented ParamPages, so the teardown order
    // (synthWorkspace_ before modMatrixView_) stays safe.
    modMatrixView_ = view;
    if (view != nullptr)
        addAndMakeVisible (*view);
}

//==============================================================================
void SynthWorkspace::showGenerator (int modSrcEnum)
{
    const auto it = generators_.find (modSrcEnum);
    if (it == generators_.end() || it->second.page == nullptr)
        return;

    auto* page = it->second.page;

    // Reparent the page into the active-editor host (the page is NEVER
    // regenerated — only its parent + visible-group subset change). The prior
    // page is detached (non-owned: removeChildComponent, never deleted).
    if (activePage_ != page)
    {
        if (activePage_ != nullptr)
            activeEditorHost_->removeChildComponent (activePage_);
        activeEditorHost_->addAndMakeVisible (*page);
        activePage_ = page;
    }

    // Show just this generator's group subset (EMPTY array => ALL groups, e.g.
    // ARP). The stored array is passed through unchanged so a multi-group entry
    // (e.g. the Note Sequencer's "Note Pitch" + "Note Velocity") reveals every
    // group it names.
    page->setVisibleGroups (it->second.groups);

    reflowActiveEditor();
}

void SynthWorkspace::setActiveGenerator (int modSrcEnum)
{
    showGenerator (modSrcEnum);
    modBar_->setActiveGenerator (modSrcEnum);   // underline-glow the pill
}

void SynthWorkspace::reflowActiveEditor()
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
int SynthWorkspace::barPreferredWidth() const
{
    return modBar_ != nullptr ? modBar_->preferredWidth() : 0;
}

//==============================================================================
void SynthWorkspace::paint (juce::Graphics& g)
{
    // Flat windowBackground fill so any integer-division remainder between the
    // rigid cells (or a short page) never bleeds the default component colour.
    g.fillAll (themeManager_.getCurrentTheme().windowBackground);
}

void SynthWorkspace::resized()
{
    auto area = getLocalBounds();
    if (area.isEmpty())
        return;

    // ---- 3 rows: TOP (main) | MIDDLE (bar) | BOTTOM (generators | matrix) ----
    // The bar is a fixed-height full-width seam; the remaining height splits
    // evenly between the top main row and the bottom row (as the prior 50/50).
    const int remaining = juce::jmax (0, area.getHeight() - CentralModBar::kBarHeight);
    const int mainH = remaining / 2;

    auto mainRow  = area.removeFromTop (mainH);
    auto barRow   = area.removeFromTop (CentralModBar::kBarHeight);
    auto bottomRow = area;   // remaining (mainH or mainH + 1px remainder)

    // ---- Main row columns: OSCILLATORS 40% | MIXER 20% | FILTER 40% ----
    const int fullW = mainRow.getWidth();
    auto oscCol = mainRow.removeFromLeft (fullW * 40 / 100);
    auto mixCol = mainRow.removeFromLeft (fullW * 20 / 100);
    auto filCol = mainRow;                       // remaining 40%

    auto sizeDirect = [] (ParamPage* page, const juce::Rectangle<int>& b)
    {
        if (page == nullptr)
            return;
        page->setBounds (b);
        page->reflowToWidth (juce::jmax (150, b.getWidth()), juce::jmax (0, b.getHeight()));
    };
    sizeDirect (mainOscPage_,   oscCol);
    sizeDirect (mainLeftPage_,  mixCol);
    sizeDirect (mainRightPage_, filCol);

    // ---- Middle seam: full-width bar ----
    modBar_->setBounds (barRow);

    // ---- Bottom row: LEFT 50% = active editor, RIGHT 50% = ModMatrixView ----
    auto modLeft  = bottomRow.removeFromLeft (bottomRow.getWidth() / 2);
    auto modRight = bottomRow;

    activeEditorHost_->setBounds (modLeft);
    reflowActiveEditor();

    if (modMatrixView_ != nullptr)
        modMatrixView_->setBounds (modRight);   // its resized() lays out the rows
}

//==============================================================================
void SynthWorkspace::applyThemeColors()
{
    if (mainOscPage_   != nullptr) mainOscPage_->applyThemeColors();
    if (mainLeftPage_  != nullptr) mainLeftPage_->applyThemeColors();
    if (mainRightPage_ != nullptr) mainRightPage_->applyThemeColors();

    if (modBar_ != nullptr)
        modBar_->applyThemeColors();

    // The active generator page re-themes here; non-active pages are themed by
    // the editor's generatedPages_ pass in applyAllColoursFromTheme.
    if (activePage_ != nullptr)
        activePage_->applyThemeColors();

    if (modMatrixView_ != nullptr)
        modMatrixView_->applyThemeColors();

    repaint();
}

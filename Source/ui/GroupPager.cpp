// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See GroupPager.h.

#include "GroupPager.h"

#include "PluginEditor.h"   // ParamPage complete type
#include "ThemeManager.h"

//==============================================================================
GroupPager::GroupPager (ThemeManager& tm, ParamPage* page, std::vector<Subset> subsets,
                        juce::Colour categoryColour)
    : themeManager_ (tm), page_ (page), subsets_ (std::move (subsets)),
      tabCategoryColour_ (categoryColour)
{
    bar_.setMinimumTabScaleFactor (0.25);
    addAndMakeVisible (bar_);
    bar_.addChangeListener (this);   // TabbedButtonBar broadcasts on every click

    const auto bg = themeManager_.getCurrentTheme().windowBackground;
    for (const auto& s : subsets_)
        bar_.addTab (s.first, bg, -1);   // (name, tab fill colour, append)

    // Colour every sub-tab with the bar's parent-category hue (ENV*->cyan,
    // LFO*->magenta, SEQ*->green, MOD MATRIX/MODIFIERS ->amber).
    applySubTabCategoryColours();

    // The paginated page stays editor-owned (generatedPages_); it is merely
    // reparented here so every APVTS attachment survives. addAndMakeVisible does
    // NOT transfer ownership, so GroupPager never deletes it.
    if (page_ != nullptr)
        addAndMakeVisible (*page_);

    if (! subsets_.empty())
        selectSubset (0);               // show the first sub-tab's groups
    else if (page_ != nullptr)
        page_->setVisibleGroups ({});   // empty => show ALL groups
}

//==============================================================================
void GroupPager::paint (juce::Graphics& g)
{
    // Void-free fill behind the bar + page (transparent children sit on this).
    g.fillAll (themeManager_.getCurrentTheme().windowBackground);
}

void GroupPager::resized()
{
    auto area = getLocalBounds();
    if (area.isEmpty())
        return;

    bar_.setBounds (area.removeFromTop (kBarH));

    // Reflow the page to the content area width/height. reflowToWidth sizes the
    // page to (width, max(naturalH, viewH)); the per-sub-tab pagination keeps each
    // subset short enough to fit the height, so the page fills the area with NO
    // scrollbar (vertical scroll was removed workspace-wide).
    if (page_ != nullptr)
    {
        page_->setBounds (area);
        page_->reflowToWidth (juce::jmax (150, area.getWidth()), juce::jmax (0, area.getHeight()));
    }
}

//==============================================================================
void GroupPager::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // The bar broadcasts on every tab change: show the new sub-tab's group subset.
    selectSubset (bar_.getCurrentTabIndex());
}

void GroupPager::selectSubset (int index)
{
    if (index < 0 || index >= (int) subsets_.size())
        return;
    current_ = index;
    if (page_ == nullptr)
        return;

    // Switch the page to the new group subset, then REFLOW to the real content
    // area (below the bar). reflowToWidth lays out at the GroupPager's actual
    // width/height and re-seats the page — defence-in-depth so a runtime sub-tab
    // switch never leaves a stale-width layout (reviewer blocker B1).
    page_->setVisibleGroups (subsets_[(size_t) index].second);

    auto area = getLocalBounds();
    if (area.getHeight() > kBarH)
    {
        area.removeFromTop (kBarH);
        page_->setBounds (area);
        page_->reflowToWidth (juce::jmax (150, area.getWidth()),
                              juce::jmax (0, area.getHeight()));
    }
}

//==============================================================================
void GroupPager::setTabCategoryColour (juce::Colour colour)
{
    tabCategoryColour_ = colour;
    applySubTabCategoryColours();
}

void GroupPager::applySubTabCategoryColours()
{
    for (int i = 0; i < bar_.getNumTabs(); ++i)
        if (auto* btn = bar_.getTabButton (i))
            btn->setColour (parvatiTabCategoryColourId, tabCategoryColour_);
}

//==============================================================================
void GroupPager::applyThemeColors()
{
    const auto bg = themeManager_.getCurrentTheme().windowBackground;
    for (int i = 0; i < bar_.getNumTabs(); ++i)
        bar_.setTabBackgroundColour (i, bg);
    applySubTabCategoryColours();   // re-colour sub-tabs (snapshot set by setTabCategoryColour)
    if (page_ != nullptr)
        page_->applyThemeColors();
    repaint();
}

// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See SynthWorkspace.h.

#include "SynthWorkspace.h"

#include "PluginEditor.h"   // ParamPage complete type
#include "ThemeManager.h"

//==============================================================================
SynthWorkspace::SynthWorkspace (ThemeManager& tm)
    : themeManager_ (tm)
{
    // Two nested TabbedComponents for the bottom mod row. They inherit the
    // editor-wide ParvatiLookAndFeel, so tab/combo colours come automatically;
    // only the tab-bar depth + outline are set here.
    envLfoTabs_ = std::make_unique<juce::TabbedComponent> (juce::TabbedButtonBar::TabsAtTop);
    envLfoTabs_->setTabBarDepth (kNestedTabBarDepth);
    envLfoTabs_->setOutline (1);   // 1px card border (left/right/bottom); tab baseline (drawTabButton) supplies the top edge
    envLfoTabs_->getProperties().set ("parvatiCardTabs", true);   // marker: drawTabButton renders the embedded bracket motif for these card tabs only
    addAndMakeVisible (*envLfoTabs_);

    modTabs_ = std::make_unique<juce::TabbedComponent> (juce::TabbedButtonBar::TabsAtTop);
    modTabs_->setTabBarDepth (kNestedTabBarDepth);
    modTabs_->setOutline (1);   // 1px card border (left/right/bottom); tab baseline (drawTabButton) supplies the top edge
    modTabs_->getProperties().set ("parvatiCardTabs", true);   // marker: embedded bracket motif (card tabs only)
    addAndMakeVisible (*modTabs_);
}

//==============================================================================
void SynthWorkspace::setMainLeft (ParamPage* page)
{
    // Mixer — direct child (editor-owned page, reparented not regenerated).
    mainLeftPage_ = page;
    if (page != nullptr)
        addAndMakeVisible (*page);
}

void SynthWorkspace::setOscillators (ParamPage* page, GroupSubsets subsets)
{
    // Oscillators — one GroupPager paginates the page by oscillator ([OSC1][OSC2]).
    // The workspace owns the pager; the page inside stays editor-owned.
    oscPager_ = std::make_unique<GroupPager> (themeManager_, page, std::move (subsets));
    addAndMakeVisible (*oscPager_);
}

void SynthWorkspace::setMainRight (ParamPage* page)
{
    // Filter — direct child (editor-owned page, reparented not regenerated).
    mainRightPage_ = page;
    if (page != nullptr)
        addAndMakeVisible (*page);
}

void SynthWorkspace::addEnvLfoTab (const juce::String& shortName, ParamPage* page, GroupSubsets subsets)
{
    envLfoTabNames_.push_back (shortName);
    const auto bg = themeManager_.getCurrentTheme().windowBackground;
    if (! subsets.empty())
    {
        // GroupPager paginates the page by generator (one Env/LFO per sub-tab).
        // The nested TC owns (deletes) the GroupPager; the page stays editor-owned.
        auto pager = std::make_unique<GroupPager> (themeManager_, page, std::move (subsets));
        envLfoTabs_->addTab (shortName, bg, pager.release(), true);
    }
    else
    {
        envLfoTabs_->addTab (shortName, bg, page, false);   // editor-owned page; TC must NOT delete it
    }
}

void SynthWorkspace::addModTab (const juce::String& shortName, ParamPage* page, GroupSubsets subsets)
{
    modTabNames_.push_back (shortName);
    const auto bg = themeManager_.getCurrentTheme().windowBackground;
    if (! subsets.empty())
    {
        auto pager = std::make_unique<GroupPager> (themeManager_, page, std::move (subsets));
        modTabs_->addTab (shortName, bg, pager.release(), true);
    }
    else
    {
        modTabs_->addTab (shortName, bg, page, false);
    }
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

    // ---- Main row (top 50%) + mod row (bottom 50%): butted, no gap ----
    const int mainH = area.getHeight() / 2;
    auto mainRow = area.removeFromTop (mainH);
    auto modRow  = area;   // remaining bottom half

    // ---- Main row columns: OSCILLATORS 40% | MIXER 20% | FILTER 40% ----
    // Signal-chain order (Osc -> Mixer -> Filter), left to right.
    const int fullW = mainRow.getWidth();
    auto oscCol = mainRow.removeFromLeft (fullW * 40 / 100);
    auto mixCol = mainRow.removeFromLeft (fullW * 20 / 100);
    auto filCol = mainRow;                       // remaining 40%

    // OSC = GroupPager (its resized() repositions the bar + reflows the page).
    if (oscPager_ != nullptr)
        oscPager_->setBounds (oscCol);

    // MIX / FILTER = direct pages, sized + reflowed to the column (no Viewport,
    // no scrollbar: the group layout is kept dense enough to fit the cell).
    auto sizeDirect = [] (ParamPage* page, const juce::Rectangle<int>& b)
    {
        if (page == nullptr)
            return;
        page->setBounds (b);
        page->reflowToWidth (juce::jmax (150, b.getWidth()), juce::jmax (0, b.getHeight()));
    };
    sizeDirect (mainLeftPage_,  mixCol);
    sizeDirect (mainRightPage_, filCol);

    // ---- Mod row: LEFT 50% = envLfoTabs_, RIGHT 50% = modTabs_ ----
    auto modLeft  = modRow.removeFromLeft (modRow.getWidth() / 2);
    auto modRight = modRow;

    envLfoTabs_->setBounds (modLeft);
    modTabs_->setBounds    (modRight);

    // Reflow EVERY tab's content (GroupPager or direct page) to the nested tab
    // content area, so non-current tabs are laid out before they are shown — JUCE
    // only sizes the CURRENT tab's content, so the headless test / screenshots
    // (and a real tab switch) need every page laid out ahead of time.
    auto reflowAllTabs = [] (juce::TabbedComponent* tc)
    {
        if (tc == nullptr)
            return;
        const int w = juce::jmax (1, tc->getWidth());
        const int h = juce::jmax (0, tc->getHeight() - kNestedTabBarDepth);
        for (int i = 0; i < tc->getNumTabs(); ++i)
        {
            auto* content = tc->getTabContentComponent (i);
            if (auto* pager = dynamic_cast<GroupPager*> (content))
                pager->setBounds ({ 0, 0, w, h });   // resized() reflows the page inside
            else if (auto* page = dynamic_cast<ParamPage*> (content))
            {
                page->setBounds ({ 0, 0, w, h });
                page->reflowToWidth (w, h);
            }
        }
    };
    reflowAllTabs (envLfoTabs_.get());
    reflowAllTabs (modTabs_.get());
}

//==============================================================================
void SynthWorkspace::reapplyTabLabels()
{
    for (size_t i = 0; i < envLfoTabNames_.size(); ++i)
        envLfoTabs_->setTabName (static_cast<int> (i), envLfoTabNames_[i]);
    for (size_t i = 0; i < modTabNames_.size(); ++i)
        modTabs_->setTabName (static_cast<int> (i), modTabNames_[i]);
}

void SynthWorkspace::applyThemeColors()
{
    const auto bg = themeManager_.getCurrentTheme().windowBackground;

    if (oscPager_ != nullptr)
        oscPager_->applyThemeColors();
    if (mainLeftPage_  != nullptr) mainLeftPage_->applyThemeColors();
    if (mainRightPage_ != nullptr) mainRightPage_->applyThemeColors();

    auto applyTc = [bg] (juce::TabbedComponent* tc)
    {
        if (tc == nullptr)
            return;
        for (int i = 0; i < tc->getNumTabs(); ++i)
        {
            tc->setTabBackgroundColour (i, bg);
            if (auto* pager = dynamic_cast<GroupPager*> (tc->getTabContentComponent (i)))
                pager->applyThemeColors();
            else if (auto* page = dynamic_cast<ParamPage*> (tc->getTabContentComponent (i)))
                page->applyThemeColors();
        }
    };
    applyTc (envLfoTabs_.get());
    applyTc (modTabs_.get());

    repaint();
}

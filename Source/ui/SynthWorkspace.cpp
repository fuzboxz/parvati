// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See SynthWorkspace.h.

#include "SynthWorkspace.h"

#include "PluginEditor.h"   // ParamPage complete type
#include "ModMatrixView.h"
#include "ThemeManager.h"

//==============================================================================
// Map a nested-tab shortName to its FUNCTION-CATEGORY colour token from the
// active theme (ENV=cyan, LFO=magenta, ARP=purple, SEQ=green, MOD MATRIX /
// MODIFIERS=amber). Falls back to tabUnderline so a tab without a category keeps
// the default highlight. Drives the per-tab category colour (see
// drawTabButton + parvatiTabCategoryColourId).
static juce::Colour categoryColourForShortName (const juce::String& shortName, const ParvatiTheme& t)
{
    if (shortName.startsWithIgnoreCase ("ENV")) return t.catEnv;
    if (shortName.startsWithIgnoreCase ("LFO")) return t.catLfo;
    if (shortName.startsWithIgnoreCase ("ARP")) return t.catArp;
    if (shortName.startsWithIgnoreCase ("SEQ")) return t.catSeq;
    if (shortName.startsWithIgnoreCase ("MOD")) return t.catAudio;   // MOD MATRIX / MODIFIERS
    return t.tabUnderline;
}

// Set a single TabbedComponent tab button's category colour (the individual
// TabBarButton carries the hue; drawTabButton reads it).
static void colourTabButton (juce::TabbedComponent& tc, int tabIndex, juce::Colour colour)
{
    auto& bar = tc.getTabbedButtonBar();
    if (auto* btn = bar.getTabButton (tabIndex))
        btn->setColour (parvatiTabCategoryColourId, colour);
}

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
    addAndMakeVisible (*envLfoTabs_);

    modTabs_ = std::make_unique<juce::TabbedComponent> (juce::TabbedButtonBar::TabsAtTop);
    modTabs_->setTabBarDepth (kNestedTabBarDepth);
    modTabs_->setOutline (1);   // 1px card border (left/right/bottom); tab baseline (drawTabButton) supplies the top edge
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

void SynthWorkspace::addEnvLfoTab (const juce::String& shortName, ParamPage* page, GroupSubsets subsets,
                                   GroupPager::TabSourceMap tabDragSource)
{
    envLfoTabNames_.push_back (shortName);
    const auto& theme = themeManager_.getCurrentTheme();
    const auto bg = theme.windowBackground;
    const auto catColour = categoryColourForShortName (shortName, theme);
    if (! subsets.empty())
    {
        // GroupPager paginates the page by generator (one Env/LFO per sub-tab).
        // The nested TC owns (deletes) the GroupPager; the page stays editor-owned.
        // The bar's parent category colour is propagated to every sub-tab.
        // tabDragSource makes each sub-tab a draggable mod-source drag SOURCE
        // (passed through only for the ENV/LFO/SEQ generator pagers).
        auto pager = std::make_unique<GroupPager> (themeManager_, page, std::move (subsets), catColour,
                                                   std::move (tabDragSource));
        envLfoTabs_->addTab (shortName, bg, pager.release(), true);
    }
    else
    {
        envLfoTabs_->addTab (shortName, bg, page, false);   // editor-owned page; TC must NOT delete it
    }
    colourTabButton (*envLfoTabs_, envLfoTabs_->getNumTabs() - 1, catColour);
}

void SynthWorkspace::addModTab (const juce::String& shortName, ParamPage* page, GroupSubsets subsets)
{
    modTabNames_.push_back (shortName);
    const auto& theme = themeManager_.getCurrentTheme();
    const auto bg = theme.windowBackground;
    const auto catColour = categoryColourForShortName (shortName, theme);
    if (! subsets.empty())
    {
        auto pager = std::make_unique<GroupPager> (themeManager_, page, std::move (subsets), catColour);
        modTabs_->addTab (shortName, bg, pager.release(), true);
    }
    else
    {
        modTabs_->addTab (shortName, bg, page, false);
    }
    colourTabButton (*modTabs_, modTabs_->getNumTabs() - 1, catColour);
}

void SynthWorkspace::setModMatrixView (ModMatrixView* view)
{
    // Host the editor-owned ModMatrixView as the MOD MATRIX tab content. Mirrors
    // the direct-host path of addModTab (deleteWhenNotNeeded=false): the view
    // stays editor-owned; the TabbedComponent must NOT delete it. Replaces the
    // old 1-4/5-8/9-12/13-14 GroupPager pagination for the MOD MATRIX tab only
    // (MODIFIERS keeps its GroupPager).
    const juce::String shortName { "MOD MATRIX" };
    modTabNames_.push_back (shortName);
    const auto& theme = themeManager_.getCurrentTheme();
    const auto bg = theme.windowBackground;
    const auto catColour = categoryColourForShortName (shortName, theme);
    modTabs_->addTab (shortName, bg, view, false);
    colourTabButton (*modTabs_, modTabs_->getNumTabs() - 1, catColour);
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

    // All three main-row columns are direct pages, sized + reflowed to the
    // column (no Viewport, no scrollbar: the group layout is dense enough to fit
    // the cell). OSC shows BOTH oscillators (empty visibleGroups_ => all groups).
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
                pager->setBounds ({ 0, kNestedTabBarDepth, w, h });   // BELOW the tab bar (was {0,0} which overlapped it)
            else if (auto* page = dynamic_cast<ParamPage*> (content))
            {
                page->setBounds ({ 0, kNestedTabBarDepth, w, h });
                page->reflowToWidth (w, h);
            }
            else if (auto* mmv = dynamic_cast<ModMatrixView*> (content))
                mmv->setBounds ({ 0, kNestedTabBarDepth, w, h });   // editor-owned view; sized like a direct page (its resized() lays out rows)
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
    const auto& theme = themeManager_.getCurrentTheme();
    const auto bg = theme.windowBackground;

    if (mainOscPage_   != nullptr) mainOscPage_->applyThemeColors();
    if (mainLeftPage_  != nullptr) mainLeftPage_->applyThemeColors();
    if (mainRightPage_ != nullptr) mainRightPage_->applyThemeColors();

    // Re-apply the per-tab FUNCTION-CATEGORY colour from the NEW theme token +
    // the stored shortName to each nested card tab, and propagate the fresh hue
    // to any GroupPager sub-tabs (setTabCategoryColour), so cycling themes
    // re-colours the tabs. (The category Colour is a theme snapshot, so it MUST
    // be re-resolved here — a plain repaint would freeze the old theme's hue.)
    auto applyTc = [&bg, &theme] (juce::TabbedComponent* tc, const std::vector<juce::String>& names)
    {
        if (tc == nullptr)
            return;
        for (int i = 0; i < tc->getNumTabs(); ++i)
        {
            tc->setTabBackgroundColour (i, bg);
            const juce::Colour catColour = (i < (int) names.size())
                ? categoryColourForShortName (names[(size_t) i], theme)
                : theme.tabUnderline;
            colourTabButton (*tc, i, catColour);
            if (auto* pager = dynamic_cast<GroupPager*> (tc->getTabContentComponent (i)))
            {
                pager->setTabCategoryColour (catColour);
                pager->applyThemeColors();
            }
            else if (auto* page = dynamic_cast<ParamPage*> (tc->getTabContentComponent (i)))
                page->applyThemeColors();
            else if (auto* mmv = dynamic_cast<ModMatrixView*> (tc->getTabContentComponent (i)))
                mmv->applyThemeColors();
        }
    };
    applyTc (envLfoTabs_.get(), envLfoTabNames_);
    applyTc (modTabs_.get(),    modTabNames_);

    repaint();
}

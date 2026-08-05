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

namespace
{
// Create + configure an owned Viewport viewing an editor-owned page. The page is
// reparented into the Viewport but NOT deleted by it (the editor retains
// ownership, preserving every APVTS attachment / the byte-bridge).
std::unique_ptr<juce::Viewport> makePageViewport (ParamPage* page)
{
    // JUCE 9 Viewport paints no background (transparent): the owning workspace /
    // nested TabbedComponent fills windowBackground behind it (void-free, theme-aware).
    auto vp = std::make_unique<juce::Viewport>();
    // Pages fill the column width, so only vertical scrolling is ever needed.
    vp->setScrollBarsShown (true, false);
    vp->setViewedComponent (page, false);   // editor owns the page
    return vp;
}
}  // namespace

void SynthWorkspace::setMainLeft (ParamPage* page)
{
    mainLeftPage_ = page;
    mainLeft_ = makePageViewport (page);
    addAndMakeVisible (*mainLeft_);
}

void SynthWorkspace::setMainCenter (ParamPage* page)
{
    mainCenterPage_ = page;
    mainCenter_ = makePageViewport (page);
    addAndMakeVisible (*mainCenter_);
}

void SynthWorkspace::setMainRight (ParamPage* page)
{
    mainRightPage_ = page;
    mainRight_ = makePageViewport (page);
    addAndMakeVisible (*mainRight_);
}

void SynthWorkspace::addEnvLfoTab (const juce::String& shortName, ParamPage* page)
{
    envLfoTabNames_.push_back (shortName);
    envLfoPages_.push_back (page);
    auto vp = makePageViewport (page);
    // The nested TabbedComponent owns the Viewport; the page stays editor-owned.
    envLfoTabs_->addTab (shortName, themeManager_.getCurrentTheme().windowBackground, vp.release(), true);
}

void SynthWorkspace::addModTab (const juce::String& shortName, ParamPage* page)
{
    modTabNames_.push_back (shortName);
    modPages_.push_back (page);
    auto vp = makePageViewport (page);
    modTabs_->addTab (shortName, themeManager_.getCurrentTheme().windowBackground, vp.release(), true);
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

    // ---- Main row (top ~2/3) + mod row (bottom ~1/3): butted, no gap ----
    const int mainH = (area.getHeight() * 2) / 3;
    auto mainRow = area.removeFromTop (mainH);
    auto modRow  = area;   // remaining bottom third

    // ---- Main row columns: Mixer 18% | Oscillators 42% | Filter 40% ----
    // A shared 60% vertical boundary is computed ONCE so the top tier's
    // OSC|Filter seam and the bottom mod row's LeftMod|RightMod seam land on the
    // same x to the pixel. Both rows are full width, so the same fullW is used.
    const int fullW     = mainRow.getWidth();
    const int boundaryX = fullW * 60 / 100;        // shared 60% vertical line
    const int mixerW    = fullW * 18 / 100;
    const int oscW      = boundaryX - mixerW;      // == 42%; subtract keeps the line exact
    auto leftCol   = mainRow.removeFromLeft (mixerW);
    auto centerCol = mainRow.removeFromLeft (oscW);
    auto rightCol  = mainRow;                       // remaining 40%

    auto sizeMainCell = [] (juce::Viewport* vp, ParamPage* page, juce::Rectangle<int> bounds)
    {
        if (vp == nullptr)
            return;
        vp->setBounds (bounds);
        if (page != nullptr)
        {
            // Reflow to the actual column width (no 200 floor): at the 1100px min
            // window width the LEFT column is ~183px, so a 200 floor would clip
            // ~17px of right-edge content (the horizontal scrollbar is hidden).
            const int w = juce::jmax (150, vp->getWidth() - 16);
            const int h = juce::jmax (0, vp->getHeight());
            page->reflowToWidth (w, h);
        }
    };
    sizeMainCell (mainLeft_.get(),   mainLeftPage_,   leftCol);
    sizeMainCell (mainCenter_.get(), mainCenterPage_, centerCol);
    sizeMainCell (mainRight_.get(),  mainRightPage_,  rightCol);

    // ---- Mod row: LEFT 60% = envLfoTabs_, RIGHT 40% = modTabs_ ----
    // Reuses the shared 60% boundary so this seam aligns with the top tier's
    // OSC|Filter seam (both at boundaryX from the left edge).
    auto modLeft  = modRow.removeFromLeft (boundaryX);  // 60% — aligned to the top tier
    auto modRight = modRow;                              // remaining 40%

    // Reflow the nested pages to the nested tab content area (uniform for every
    // tab, matching the editor's old reflow pattern: JUCE only sizes the CURRENT
    // tab's Viewport, so reading it per-page would mis-reflow non-current tabs).
    auto reflowNested = [] (juce::TabbedComponent* tabs, const std::vector<ParamPage*>& pages)
    {
        if (tabs == nullptr)
            return;
        const int w = juce::jmax (280, tabs->getWidth() - 16);
        const int h = juce::jmax (0, tabs->getHeight() - kNestedTabBarDepth);
        for (auto* p : pages)
            if (p != nullptr)
                p->reflowToWidth (w, h);
    };

    envLfoTabs_->setBounds (modLeft);
    modTabs_->setBounds    (modRight);
    reflowNested (envLfoTabs_.get(), envLfoPages_);
    reflowNested (modTabs_.get(),    modPages_);
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
    for (int i = 0; i < envLfoTabs_->getNumTabs(); ++i)
        envLfoTabs_->setTabBackgroundColour (i, bg);
    for (int i = 0; i < modTabs_->getNumTabs(); ++i)
        modTabs_->setTabBackgroundColour (i, bg);
    repaint();
}

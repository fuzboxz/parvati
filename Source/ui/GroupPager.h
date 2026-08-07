// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// GroupPager — a reusable sub-tab strip that paginates ONE editor-owned
// ParamPage by its named groups. Each sub-tab calls page->setVisibleGroups
// (subset) so a dense section shows a few groups at a time with NO regeneration
// of any ParamControl / APVTS attachment:
//
//   OSCILLATORS  [OSC1] [OSC2]
//   MOD MATRIX   [1-4] [5-8] [9-12] [13-14]
//   MODIFIERS    [1-2] [3-4]
//   ENV/LFO/SEQ  one generator per sub-tab
//
// The page stays owned by ParvatiEditor (reparent, never regenerate); GroupPager
// holds only a raw ParamPage* and never destroys it. The bare juce::TabbedButtonBar
// renders the segmented sub-tabs via the editor-wide ParvatiLookAndFeel.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <utility>
#include <vector>

class ParamPage;
class ThemeManager;

//==============================================================================
class GroupPager : public juce::Component,
                   private juce::ChangeListener
{
public:
    // One sub-tab: { tab label, group names to show on it }. An empty group list
    // shows ALL of the page's groups for that tab.
    using Subset = std::pair<juce::String, juce::StringArray>;

    // @p page is the editor-owned ParamPage to paginate (NOT owned/deleted here).
    // @p categoryColour is the bar's FUNCTION-CATEGORY colour (resolved from the
    // parent tab's shortName + theme); all of this pager's sub-tabs are coloured
    // with it (see drawTabButton + parvatiTabCategoryColourId).
    GroupPager (ThemeManager& themeManager, ParamPage* page, std::vector<Subset> subsets,
                juce::Colour categoryColour = {});

    void resized() override;
    void paint (juce::Graphics&) override;

    // The bar + page pick colours up via the inherited editor L&F; this just
    // repaints (and re-applies the tab fill) on a theme switch.
    void applyThemeColors();

    // Re-colour all sub-tabs with @p colour (the bar's category hue resolved
    // from the CURRENT theme). Called by SynthWorkspace on a theme switch so the
    // sub-tabs follow the new theme's category token value.
    void setTabCategoryColour (juce::Colour colour);

    // The paginated page (for the editor's per-tab reflow / sanity tooling).
    ParamPage* getPage() const noexcept { return page_; }

private:
    // juce::ChangeListener — the bare TabbedButtonBar is a ChangeBroadcaster:
    // it broadcasts on every tab click, so show that sub-tab's group subset.
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;

    void selectSubset (int index);

    // Push the stored category colour onto every sub-tab button (drawTabButton
    // reads parvatiTabCategoryColourId per TabBarButton).
    void applySubTabCategoryColours();

    ThemeManager& themeManager_;
    ParamPage* page_;                 // non-owning; editor-owned (generatedPages_)
    std::vector<Subset> subsets_;
    juce::TabbedButtonBar bar_ { juce::TabbedButtonBar::TabsAtTop };
    juce::Colour tabCategoryColour_;  // bar's parent-category colour (theme token snapshot)
    int current_ = 0;

    static constexpr int kBarH = 28;   // compact sub-tab strip (matches nested cards)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GroupPager)
};

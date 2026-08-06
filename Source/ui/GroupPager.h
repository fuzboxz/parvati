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
// carries the "parvatiCardTabs" property so the editor-wide ParvatiLookAndFeel
// renders the embedded "[ LABEL ]" bracket motif for these sub-tabs too.

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
    GroupPager (ThemeManager& themeManager, ParamPage* page, std::vector<Subset> subsets);

    void resized() override;
    void paint (juce::Graphics&) override;

    // The bar + page pick colours up via the inherited editor L&F; this just
    // repaints (and re-applies the tab fill) on a theme switch.
    void applyThemeColors();

    // The paginated page (for the editor's per-tab reflow / sanity tooling).
    ParamPage* getPage() const noexcept { return page_; }

private:
    // juce::ChangeListener — the bare TabbedButtonBar is a ChangeBroadcaster:
    // it broadcasts on every tab click, so show that sub-tab's group subset.
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;

    void selectSubset (int index);

    ThemeManager& themeManager_;
    ParamPage* page_;                 // non-owning; editor-owned (generatedPages_)
    std::vector<Subset> subsets_;
    juce::TabbedButtonBar bar_ { juce::TabbedButtonBar::TabsAtTop };
    int current_ = 0;

    static constexpr int kBarH = 28;   // compact sub-tab strip (matches nested cards)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GroupPager)
};

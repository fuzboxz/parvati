// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// SynthWorkspace — the content of the top-level SYNTH tab. A rigid, void-free
// 2-row integrated panel that hosts the EXISTING, editor-owned ParamPages
// (reparented, NOT regenerated), so every APVTS attachment and the verified
// byte-bridge survive the reorganization unchanged:
//
//   Main row (top 50%): 3 columns  [ OSCILLATORS 40% | MIXER 20% | FILTER 40% ]
//       OSCILLATORS = a GroupPager ([OSC1][OSC2], one oscillator per sub-tab)
//       MIXER / FILTER = direct ParamPages
//   Mod row (bottom 50%): 2 halves, each a nested TabbedComponent (depth 28)
//       LEFT  = [ENV][LFO]      — each tab a GroupPager (one generator/sub-tab)
//       RIGHT = [MOD MATRIX][MODIFIERS][ARP][SEQ]
//                           — GroupPagers where a section paginates by group;
//                             ARP is a single group shown directly
//
// NO per-page juce::Viewport wrappers: every page fits its cell (the GroupPager
// sub-tabs keep each visible group-subset short enough), so there are ZERO
// param-panel scrollbars. The pages stay owned by ParvatiEditor (generatedPages_);
// the workspace owns only the GroupPagers + nested TabbedComponents, so reparenting
// never duplicates a ParamControl / APVTS attachment.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <utility>
#include <vector>

#include "GroupPager.h"   // GroupPager (oscPager_ + nested-tab content)

class ParamPage;
class ThemeManager;

//==============================================================================
class SynthWorkspace : public juce::Component
{
public:
    explicit SynthWorkspace (ThemeManager& themeManager);

    // Sub-tab partitions handed to a GroupPager: { tab label, group names shown }.
    using GroupSubsets = std::vector<std::pair<juce::String, juce::StringArray>>;

    // Main-row columns in signal-chain order (OSC | MIX | FILTER at 40/20/40).
    // OSC is paginated by a GroupPager (one oscillator per sub-tab); MIX/FILTER
    // are shown directly. Pages stay editor-owned (reparented, never regenerated).
    void setMainLeft    (ParamPage* page);                       // Mixer (direct)
    void setOscillators (ParamPage* page, GroupSubsets subsets); // Oscillators (GroupPager)
    void setMainRight   (ParamPage* page);                       // Filter (direct)

    // A nested mod-row tab. A non-empty @p subsets builds a GroupPager (the page
    // is paginated by group); an empty subsets hosts the page directly (ARP).
    void addEnvLfoTab (const juce::String& shortName, ParamPage* page, GroupSubsets subsets);
    void addModTab    (const juce::String& shortName, ParamPage* page, GroupSubsets subsets);

    void resized() override;
    void paint (juce::Graphics&) override;

    // Re-apply the stored short tab labels to the nested TabbedComponents. The
    // abbreviations are language-neutral; the editor calls this on a live
    // language switch so any future translation re-resolves through here.
    void reapplyTabLabels();

    // Re-apply theme colours to the nested tab + GroupPager + page backgrounds.
    void applyThemeColors();

private:
    ThemeManager& themeManager_;

    // Main-row direct pages (Mixer / Filter) + the OSCILLATORS GroupPager.
    ParamPage* mainLeftPage_  = nullptr;    // Mixer (direct)
    ParamPage* mainRightPage_ = nullptr;    // Filter (direct)
    std::unique_ptr<GroupPager> oscPager_;  // Oscillators (owns the pager; the page is editor-owned)

    // Nested tab groups (workspace-owned) + their short labels.
    std::unique_ptr<juce::TabbedComponent> envLfoTabs_, modTabs_;
    std::vector<juce::String> envLfoTabNames_, modTabNames_;

    static constexpr int kNestedTabBarDepth = 28;   // compact nested tab strip

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SynthWorkspace)
};

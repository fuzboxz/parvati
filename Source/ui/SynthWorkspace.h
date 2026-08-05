// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// SynthWorkspace — the content of the top-level SYNTH tab. A rigid, void-free
// 2-row integrated panel that hosts the EXISTING, editor-owned ParamPages
// (reparented, NOT regenerated), so every APVTS attachment and the verified
// byte-bridge survive the reorganization unchanged:
//
//   Main row (top ~2/3): 3 columns  [LEFT 1* | CENTER 3* | RIGHT 2*]
//       LEFT   = Mixer page
//       CENTER = Oscillators page
//       RIGHT  = Filter page
//   Mod row (bottom ~1/3): 2 halves, each a nested TabbedComponent (depth 28)
//       LEFT  = [ENV][LFO]
//       RIGHT = [MOD MATRIX][MODIFIERS][ARP][SEQ]
//
// Each hosted page is wrapped in an OWNED juce::Viewport (vertical scroll only)
// and reflowed to its cell width so the dense grouped layout fills the cell. The
// pages themselves stay owned by ParvatiEditor (generatedPages_); the workspace
// owns only the Viewports and the nested TabbedComponents, so reparenting never
// duplicates a ParamControl / APVTS attachment.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

class ParamPage;
class ThemeManager;

//==============================================================================
class SynthWorkspace : public juce::Component
{
public:
    explicit SynthWorkspace (ThemeManager& themeManager);

    // Hand an editor-owned page to a main-row column. The page is reparented
    // into a Viewport this workspace owns; the page itself is NOT owned/deleted
    // by the workspace (setViewedComponent with deleteOnRemoval=false).
    void setMainLeft   (ParamPage* page);   // Mixer
    void setMainCenter (ParamPage* page);   // Oscillators
    void setMainRight  (ParamPage* page);   // Filter

    // Hand an editor-owned page to one of the nested tab groups (ENV/LFO on the
    // left half; MOD MATRIX/MODIFIERS/ARP/SEQ on the right half).
    void addEnvLfoTab (const juce::String& shortName, ParamPage* page);
    void addModTab    (const juce::String& shortName, ParamPage* page);

    void resized() override;
    void paint (juce::Graphics&) override;

    // Re-apply the stored short tab labels to the nested TabbedComponents. The
    // abbreviations are language-neutral; the editor calls this on a live
    // language switch so any future translation re-resolves through here.
    void reapplyTabLabels();

    // Re-apply theme colours to the nested tab backgrounds. The tab/combo widget
    // colours come from the inherited editor LookAndFeel automatically; only the
    // tab background fill is re-applied explicitly here.
    void applyThemeColors();

private:
    ThemeManager& themeManager_;

    // Main-row viewports (workspace-owned; each views an editor-owned page).
    std::unique_ptr<juce::Viewport> mainLeft_, mainCenter_, mainRight_;
    ParamPage* mainLeftPage_   = nullptr;
    ParamPage* mainCenterPage_ = nullptr;
    ParamPage* mainRightPage_  = nullptr;

    // Nested tab groups (workspace-owned) + their hosted editor-owned pages.
    std::unique_ptr<juce::TabbedComponent> envLfoTabs_, modTabs_;
    std::vector<juce::String> envLfoTabNames_, modTabNames_;
    std::vector<ParamPage*>   envLfoPages_,    modPages_;

    static constexpr int kNestedTabBarDepth = 28;   // compact nested tab strip

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SynthWorkspace)
};

// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// SettingsPanel — a compact settings UI hosted in the editor's SidePanel. Lets
// the user pick the active theme, adjust the global zoom, and toggle tooltips.
// Each control writes its preference straight into the processor (for
// persistence) and fires a callback so the editor can apply the change live
// (zoom via Desktop::setGlobalScaleFactor, tooltips via the ParamControl
// static flag). Styled entirely through the inherited ParvatiLookAndFeel — no
// manual colours. Phase 4a of docs/UI_MODERNIZATION_PLAN.md (gap D13/F21).

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

class ParvatiAudioProcessor;
class ThemeManager;

//==============================================================================
class SettingsPanel : public juce::Component
{
public:
    /** @param onZoomChanged        fired when the zoom slider moves (editor applies
                                 Desktop::setGlobalScaleFactor).
        @param onTooltipsChanged    fired when the tooltips toggle flips (editor
                                 enables/disables ParamControl tooltips).
        @param onSmoothingChanged   fired when the parameter-smoothing toggle flips
                                 (editor enables/disables engine smoothing).
        @param onOversamplingChanged fired when the filter-quality combo changes
                                 (1/2/4); the processor already applies + persists
                                 it, this is an editor-side hook.
        @param onLanguageChanged    fired when the language combo changes; the editor
                                 persists the pref, installs the LocalisedStrings,
                                 and re-applies every chrome string live. */
    SettingsPanel (ParvatiAudioProcessor& proc,
                   ThemeManager& themeManager,
                   std::function<void (double)> onZoomChanged,
                   std::function<void (bool)>   onTooltipsChanged,
                   std::function<void (bool)>   onSmoothingChanged,
                   std::function<void (int)>    onOversamplingChanged,
                   std::function<void (const juce::String&)> onLanguageChanged,
                   std::function<void (int)>    onVoiceModeChanged);

    ~SettingsPanel() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Programmatic zoom update (e.g. from the editor's zoom keyboard
    // shortcuts): moves the slider without re-firing onValueChange (which would
    // loop back into the editor's setZoom). Phase 4b.
    void setZoomValue (double zoom);

    // Re-apply every chrome string through TRANS() and rebuild the
    // language-dependent combos (called by the editor after a live language
    // switch so the panel updates immediately).
    void refreshLanguage();

    // Re-seed the Voice Mode combo from the processor's current ui_voice_mode
    // (dontSendNotification). Called by the editor after a .parvati multi load,
    // which can change the global Voice Mode, so the combo stays in sync.
    void refreshVoiceModeCombo();

private:
    // (Re)build the Filter Quality combo from TRANS() labels. The item IDs
    // (1/2/4) are stable across languages, so the selection survives a rebuild.
    void populateOversamplingCombo();
    // (Re)build the Voice Mode combo from TRANS() labels. IDs 1/2 are stable
    // across languages (1 = Hardware 6 voices, 2 = Extended 16 voices).
    void populateVoiceModeCombo();
    // Index <-> persisted-code helpers for the Language combo (which uses
    // index+1 as its item ID). An unknown code maps to index 0 ("auto").
    int          languageIndexFromCode (const juce::String& code) const;
    juce::String languageCodeFromIndex (int index) const;
    ParvatiAudioProcessor& proc_;
    ThemeManager& themeManager_;

    std::function<void (double)> onZoomChanged_;
    std::function<void (bool)>   onTooltipsChanged_;
    std::function<void (bool)>   onSmoothingChanged_;
    std::function<void (int)>    onOversamplingChanged_;
    std::function<void (const juce::String&)> onLanguageChanged_;
    std::function<void (int)>    onVoiceModeChanged_;

    juce::Label     themeLabel_, zoomLabel_, osLabel_, langLabel_, voiceModeLabel_;
    juce::ComboBox  themeCombo_, osCombo_, langCombo_, voiceModeCombo_;
    juce::Slider    zoomSlider_;
    juce::ToggleButton tooltipsToggle_ { "Tooltips" };
    juce::ToggleButton smoothingToggle_ { "Parameter Smoothing" };

    // While true, zoomSlider_ value changes are programmatic (setZoomValue) and
    // must NOT re-fire onZoomChanged_ (avoids an editor<->panel feedback loop).
    bool suppressCallback_ { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsPanel)
};

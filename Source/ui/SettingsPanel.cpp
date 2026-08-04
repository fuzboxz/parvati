// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See SettingsPanel.h.

#include "SettingsPanel.h"

#include "ParvatiTheme.h"
#include "PluginProcessor.h"
#include "ThemeManager.h"
#include "Translations.h"

//==============================================================================
SettingsPanel::SettingsPanel (ParvatiAudioProcessor& proc,
                              ThemeManager& themeManager,
                              std::function<void (double)> onZoomChanged,
                              std::function<void (bool)> onTooltipsChanged,
                              std::function<void (bool)> onSmoothingChanged,
                              std::function<void (int)> onOversamplingChanged,
                              std::function<void (const juce::String&)> onLanguageChanged,
                              std::function<void (int)> onVoiceModeChanged)
    : proc_ (proc),
      themeManager_ (themeManager),
      onZoomChanged_ (std::move (onZoomChanged)),
      onTooltipsChanged_ (std::move (onTooltipsChanged)),
      onSmoothingChanged_ (std::move (onSmoothingChanged)),
      onOversamplingChanged_ (std::move (onOversamplingChanged)),
      onLanguageChanged_ (std::move (onLanguageChanged)),
      onVoiceModeChanged_ (std::move (onVoiceModeChanged))
{
    // ---- Theme ----
    themeLabel_.setText (TRANS ("Theme"), juce::dontSendNotification);
    themeLabel_.setFont (juce::FontOptions (14.0f));
    themeLabel_.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (themeLabel_);

    for (const auto& name : themeManager_.getThemeNames())
        themeCombo_.addItem (name, themeCombo_.getNumItems() + 1);
    // Select the persisted theme (graceful: setText if not found).
    themeCombo_.setText (proc_.getUiTheme(), juce::dontSendNotification);
    themeCombo_.onChange = [this] {
        const auto name = themeCombo_.getText();
        themeManager_.selectByName (name);   // broadcasts -> editor re-applies L&F
        proc_.setUiTheme (name);
    };
    addAndMakeVisible (themeCombo_);

    // ---- Zoom ----
    zoomLabel_.setText (TRANS ("Zoom"), juce::dontSendNotification);
    zoomLabel_.setFont (juce::FontOptions (14.0f));
    zoomLabel_.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (zoomLabel_);

    zoomSlider_.setSliderStyle (juce::Slider::LinearHorizontal);
    zoomSlider_.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 20);
    zoomSlider_.setRange (0.75, 2.0, 0.05);
    zoomSlider_.setNumDecimalPlacesToDisplay (0);
    // Display as a percentage (75% .. 200%).
    zoomSlider_.textFromValueFunction = [] (double v) { return juce::String (juce::roundToInt (v * 100.0)) + "%"; };
    zoomSlider_.valueFromTextFunction = [] (const juce::String& t) {
        return t.replace ("%", "").getDoubleValue() / 100.0;
    };
    zoomSlider_.setValue (proc_.getUiZoom(), juce::dontSendNotification);
    zoomSlider_.onValueChange = [this] {
        if (suppressCallback_)
            return;   // programmatic update from setZoomValue — don't re-fire
        const double v = zoomSlider_.getValue();
        proc_.setUiZoom (v);
        if (onZoomChanged_)
            onZoomChanged_ (v);
    };
    addAndMakeVisible (zoomSlider_);

    // ---- Tooltips ----
    tooltipsToggle_.setButtonText (TRANS ("Tooltips"));
    tooltipsToggle_.setToggleState (proc_.getUiTooltips(), juce::dontSendNotification);
    tooltipsToggle_.onClick = [this] {
        const bool b = tooltipsToggle_.getToggleState();
        proc_.setUiTooltips (b);
        if (onTooltipsChanged_)
            onTooltipsChanged_ (b);
    };
    addAndMakeVisible (tooltipsToggle_);

    // ---- Parameter Smoothing ----
    // Reduces zipper noise on continuous knob turns / automation of cutoff,
    // resonance and volume by ramping those params per-sample (20 ms). Default
    // OFF keeps the audio path bit-identical to the un-smoothed engine.
    smoothingToggle_.setButtonText (TRANS ("Parameter Smoothing"));
    smoothingToggle_.setToggleState (proc_.getUiSmoothing(), juce::dontSendNotification);
    smoothingToggle_.onClick = [this] {
        const bool b = smoothingToggle_.getToggleState();
        proc_.setParameterSmoothing (b);
        if (onSmoothingChanged_)
            onSmoothingChanged_ (b);
    };
    addAndMakeVisible (smoothingToggle_);

    // ---- Filter Quality (oversampling) ----
    // Oversamples ONLY the digital filter MODEL (not the oscillators, which stay
    // at the fixed 39216 Hz internal rate for authenticity) to reduce the
    // filter's aliasing for higher fidelity. The item ID is the factor (1/2/4).
    // Default "Standard" (1x) keeps the audio path bit-identical.
    osLabel_.setText (TRANS ("Filter Quality"), juce::dontSendNotification);
    osLabel_.setFont (juce::FontOptions (14.0f));
    osLabel_.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (osLabel_);

    // The combo items are chrome (localised); the IDs (1/2/4) are stable and
    // persist separately, so a language switch only relabels them.
    populateOversamplingCombo();
    osCombo_.setSelectedId (proc_.getUiOversampling(), juce::dontSendNotification);
    osCombo_.onChange = [this] {
        const int factor = osCombo_.getSelectedId();
        proc_.setOversamplingFactor (factor);
        if (onOversamplingChanged_)
            onOversamplingChanged_ (factor);
    };
    addAndMakeVisible (osCombo_);

    // ---- Language ----
    // Editor chrome language. Item ID = index+1 into getAvailableLanguages();
    // the stored code ("auto"/"en"/"fr") is round-tripped, not the label, so a
    // localized label never leaks into persistence.
    langLabel_.setText (TRANS ("Language"), juce::dontSendNotification);
    langLabel_.setFont (juce::FontOptions (14.0f));
    langLabel_.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (langLabel_);

    const auto& langs = getAvailableLanguages();
    for (size_t i = 0; i < langs.size(); ++i)
        langCombo_.addItem (langs[i].second, static_cast<int> (i) + 1);
    langCombo_.setSelectedId (languageIndexFromCode (proc_.getUiLanguage()) + 1,
                              juce::dontSendNotification);
    langCombo_.onChange = [this] {
        const auto code = languageCodeFromIndex (langCombo_.getSelectedId() - 1);
        proc_.setUiLanguage (code);
        if (onLanguageChanged_)
            onLanguageChanged_ (code);
    };
    addAndMakeVisible (langCombo_);

    // ---- Voice Mode (polyphony capacity) ----
    // Hardware (6 voices) = faithful Ambika (one voice per voicecard);
    // Extended (16 voices) = the full per-voicecard block for polyphony headroom.
    // Item IDs 1/2 are stable; the stored value is 0/1 (ID - 1). Default Hardware.
    voiceModeLabel_.setText (TRANS ("Voice Mode"), juce::dontSendNotification);
    voiceModeLabel_.setFont (juce::FontOptions (14.0f));
    voiceModeLabel_.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (voiceModeLabel_);

    populateVoiceModeCombo();
    voiceModeCombo_.setSelectedId (proc_.getUiVoiceMode() + 1, juce::dontSendNotification);
    // Persistence + engine + meter view are owned by the editor's
    // onVoiceModeChanged callback (single source of truth); the panel only fires it.
    voiceModeCombo_.onChange = [this] {
        if (onVoiceModeChanged_)
            onVoiceModeChanged_ (voiceModeCombo_.getSelectedId() - 1);   // 1/2 -> 0/1
    };
    // Clarify the audible trade-off: Hardware caps polyphony at the 6 voicecards
    // (faithful to the Ambika); Extended raises it to 16 for headroom and is what
    // lets UNISON_2X/CHAIN double beyond the 6-voicecard limit.
    const juce::String vmTip = TRANS (
        "Hardware = faithful 6-voice Ambika (default). "
        "Extended = 16 voices for polyphony headroom; needed for Unison/Chain "
        "doubling beyond the 6 voicecards.");
    voiceModeCombo_.setTooltip (vmTip);
    voiceModeLabel_.setTooltip (vmTip);
    addAndMakeVisible (voiceModeCombo_);
}

void SettingsPanel::setZoomValue (double zoom)
{
    // Move the slider to reflect a zoom change made elsewhere (e.g. the
    // editor's keyboard shortcuts) without re-firing onValueChange. The
    // ScopedValueSetter flips suppressCallback_ for the duration of the set.
    juce::ScopedValueSetter<bool> svs (suppressCallback_, true);
    zoomSlider_.setValue (zoom, juce::sendNotificationSync);
}

void SettingsPanel::populateOversamplingCombo()
{
    osCombo_.clear();
    // CharPointer_UTF8 (not a bare const char*) so the U+00D7 MULTIPLICATION
    // SIGN is decoded as UTF-8 by juce::translate, not latin1 (which would
    // mojibake and assert). Keeps the English combo text byte-identical.
    osCombo_.addItem (TRANS (juce::CharPointer_UTF8 ("Standard (1\xc3\x97)")), 1);   // 1x
    osCombo_.addItem (TRANS (juce::CharPointer_UTF8 ("High (2\xc3\x97)")),     2);   // 2x
    osCombo_.addItem (TRANS (juce::CharPointer_UTF8 ("Maximum (4\xc3\x97)")),  4);   // 4x
}

void SettingsPanel::populateVoiceModeCombo()
{
    voiceModeCombo_.clear();
    voiceModeCombo_.addItem (TRANS ("Hardware (6 voices)"),  1);   // 0
    voiceModeCombo_.addItem (TRANS ("Extended (16 voices)"), 2);   // 1
}

int SettingsPanel::languageIndexFromCode (const juce::String& code) const
{
    const auto& langs = getAvailableLanguages();
    for (size_t i = 0; i < langs.size(); ++i)
        if (langs[i].first == code)
            return static_cast<int> (i);
    return 0;   // unknown code -> "auto" (index 0)
}

juce::String SettingsPanel::languageCodeFromIndex (int index) const
{
    const auto& langs = getAvailableLanguages();
    if (index >= 0 && index < (int) langs.size())
        return langs[(size_t) index].first;
    return "auto";
}

void SettingsPanel::refreshLanguage()
{
    // Re-apply every chrome string through the now-active LocalisedStrings, and
    // rebuild the combos whose item text is language-dependent. The persisted
    // selections (IDs / toggle states) are preserved across the rebuild.
    themeLabel_.setText (TRANS ("Theme"), juce::dontSendNotification);
    zoomLabel_.setText (TRANS ("Zoom"), juce::dontSendNotification);
    tooltipsToggle_.setButtonText (TRANS ("Tooltips"));
    smoothingToggle_.setButtonText (TRANS ("Parameter Smoothing"));
    osLabel_.setText (TRANS ("Filter Quality"), juce::dontSendNotification);
    langLabel_.setText (TRANS ("Language"), juce::dontSendNotification);
    voiceModeLabel_.setText (TRANS ("Voice Mode"), juce::dontSendNotification);

    const int osId = osCombo_.getSelectedId();
    populateOversamplingCombo();
    osCombo_.setSelectedId (osId, juce::dontSendNotification);

    const int vmId = voiceModeCombo_.getSelectedId();
    populateVoiceModeCombo();
    voiceModeCombo_.setSelectedId (vmId, juce::dontSendNotification);

    repaint();
}

void SettingsPanel::refreshVoiceModeCombo()
{
    // A .parvati multi load can change the global Voice Mode under the panel;
    // re-seed the combo from the processor's current pref without firing onChange.
    voiceModeCombo_.setSelectedId (proc_.getUiVoiceMode() + 1, juce::dontSendNotification);
}

void SettingsPanel::paint (juce::Graphics& g)
{
    // Fill with the active theme's window background for a seamless look with
    // the rest of the editor. Controls inherit their colours from the
    // editor-wide ParvatiLookAndFeel (no manual setColour calls).
    g.fillAll (themeManager_.getCurrentTheme().windowBackground);
}

void SettingsPanel::resized()
{
    auto area = getLocalBounds().reduced (16, 16);

    const int rowH = 28;
    const int gap  = 8;

    // Theme row.
    themeLabel_.setBounds (area.removeFromTop (18));
    area.removeFromTop (2);
    themeCombo_.setBounds (area.removeFromTop (rowH));
    area.removeFromTop (gap + 8);

    // Zoom row.
    zoomLabel_.setBounds (area.removeFromTop (18));
    area.removeFromTop (2);
    zoomSlider_.setBounds (area.removeFromTop (rowH));
    area.removeFromTop (gap + 8);

    // Tooltips row.
    tooltipsToggle_.setBounds (area.removeFromTop (rowH));
    area.removeFromTop (gap);

    // Parameter Smoothing row.
    smoothingToggle_.setBounds (area.removeFromTop (rowH));
    area.removeFromTop (gap + 8);

    // Filter Quality (oversampling) row.
    osLabel_.setBounds (area.removeFromTop (18));
    area.removeFromTop (2);
    osCombo_.setBounds (area.removeFromTop (rowH));
    area.removeFromTop (gap + 8);

    // Language row.
    langLabel_.setBounds (area.removeFromTop (18));
    area.removeFromTop (2);
    langCombo_.setBounds (area.removeFromTop (rowH));
    area.removeFromTop (gap + 8);

    // Voice Mode row.
    voiceModeLabel_.setBounds (area.removeFromTop (18));
    area.removeFromTop (2);
    voiceModeCombo_.setBounds (area.removeFromTop (rowH));
}

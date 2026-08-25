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
class SettingsPanel : public juce::Component,
                      private juce::Timer   // 2 Hz clock-status refresh (Arp Clock row)
{
public:
    /** @param onZoomChanged        fired when the zoom slider moves (editor applies
                                 Desktop::setGlobalScaleFactor).
        @param onTooltipsChanged    fired when the tooltips toggle flips (editor
                                 enables/disables ParamControl tooltips).
        @param onSmoothingChanged   fired when the parameter-smoothing toggle flips
                                 (editor enables/disables engine smoothing).
        @param onOversamplingChanged fired when the filter-quality combo changes
                                 (1/2/4/8); the processor already applies + persists
                                 it, this is an editor-side hook.
        @param onLanguageChanged    fired when the language combo changes; the editor
                                 persists the pref, installs the LocalisedStrings,
                                 and re-applies every chrome string live.
        @param onRefreshChanged     fired when the visual-refresh combo changes
                                 (10/15/30/60 Hz); the processor already applies +
                                 persists it, this is an editor-side hook (the
                                 editor re-times its live-feedback timers).
                                 Defaulted: the pre-wiring call site (no callback
                                 yet) still compiles; the processor is always
                                 updated regardless. */
    SettingsPanel (ParvatiAudioProcessor& proc,
                   ThemeManager& themeManager,
                   std::function<void (double)> onZoomChanged,
                   std::function<void (bool)>   onTooltipsChanged,
                   std::function<void (bool)>   onSmoothingChanged,
                   std::function<void (int)>    onOversamplingChanged,
                   std::function<void (const juce::String&)> onLanguageChanged,
                   std::function<void (int)>    onRefreshChanged = {},
                   std::function<void (bool)>   onModLampChanged = {});

    ~SettingsPanel() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Re-applies the ROLE fonts (Y2K: PT Sans). The constructor runs before
    // the panel is parented into the editor tree, so the L&F (and its theme)
    // is not attached yet — the ctor-time conversion falls back to the exact
    // legacy font. This hook re-runs once the L&F arrives and on every theme
    // switch (sendLookAndFeelChange).
    void lookAndFeelChanged() override;

    void applyRoleFonts();

    // Full no-scroll height of every settings row + margins — the height the
    // scrolling drawer (the editor's settingsScroll_ Viewport) sizes this
    // panel to; pinned to resized()'s row budget (see its comment).
    int computePreferredHeight() const;

    // Programmatic zoom update (e.g. from the editor's zoom keyboard
    // shortcuts): moves the slider without re-firing onValueChange (which would
    // loop back into the editor's setZoom). Phase 4b.
    void setZoomValue (double zoom);

    // Refresh the zoom percentage readout from the persisted preference.
    void refreshZoomReadout();

    // Re-apply every chrome string through TRANS() and rebuild the
    // language-dependent combos (called by the editor after a live language
    // switch so the panel updates immediately).
    void refreshLanguage();

    // Rebuild the Arp Clock status line from the processor's published clock
    // source (host tempo present -> value + "manual ignored" note; absent ->
    // "manual tempo active"). Also called by the 2 Hz timer while shown.
    void refreshClockStatus();

    // ---- layout test seam (headless height-sweep; see tests/undo_clock_test.cpp) ----
    /** True when the manual-BPM slider row survived the R3 bottom-first
        degradation at the current panel height (isVisible: the row hides via
        setVisible(false) when the compacted drawer runs out of height). */
    bool debugBpmSliderVisible() const { return bpmSlider_.isVisible(); }

private:
    // 2 Hz: refresh the Arp Clock status line while the panel is on screen
    // (isShowing() gate — a hidden side-panel timer costs nothing).
    void timerCallback() override;
    // (Re)build the Filter Quality combo from TRANS() labels. The item IDs
    // (1/2/4/8) are stable across languages, so the selection survives a rebuild.
    void populateOversamplingCombo();
    // (Re)build the Visual Refresh combo from TRANS() labels. Like the
    // oversampling combo, the item IDs (10/15/30/60 Hz) are stable across
    // languages so the selection survives a rebuild.
    void populateRefreshCombo();
    // The nearest offered refresh rate (10/15/30/60) to a persisted Hz value.
    // The combo can only show the offered steps; a restored mid-range value
    // (e.g. 45) snaps the DISPLAY to the nearest step while the processor
    // keeps its exact value until the user actually changes the combo.
    int snapRefreshChoice (int hz) const;
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
    std::function<void (int)>    onRefreshChanged_;
    std::function<void (bool)>   onModLampChanged_;

    juce::Label     themeLabel_, zoomLabel_, osLabel_, langLabel_;
    juce::Label     clockLabel_, clockStatusLabel_;   // Arp Clock caption + live source line
    juce::ComboBox  themeCombo_, osCombo_, langCombo_;
    // Visual Refresh row (live mod-feedback animation cadence,
    // docs/LIVE_MOD_FEEDBACK_DESIGN.md): item IDs ARE the Hz values.
    juce::Label     refreshLabel_;
    juce::ComboBox  refreshCombo_;
    // Zoom row (2026-08-20): the slider was REPLACED by the three header zoom
    // buttons (in / out / reset) + a percentage readout — the user asked for
    // the top-bar buttons to live here instead. Steps of 0.1, clamped to the
    // editor's [0.75, 2.0] zoom range (the same applyZoom contract).
    juce::TextButton zoomOutBt_   { "-" };
    juce::TextButton zoomInBt_    { "+" };
    juce::TextButton zoomResetBt_ { TRANS ("Reset") };   // was "0" (user feedback 2026-08-20)
    juce::Label      zoomValueLabel_;
    juce::Slider    bpmSlider_;                       // manual arp-clock tempo (40..300 BPM)
    juce::ToggleButton tooltipsToggle_ { "Tooltips" };
    juce::ToggleButton smoothingToggle_ { "Parameter Smoothing" };
    // Mod-matrix lamp colour policy: category colour (on) vs theme accent
    // (off). Affects BOTH matrices at once.
    juce::ToggleButton modLampToggle_ { "Mod Lamp Colours" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsPanel)
};

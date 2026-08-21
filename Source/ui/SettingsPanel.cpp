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
                              std::function<void (int)> onRefreshChanged)
    : proc_ (proc),
      themeManager_ (themeManager),
      onZoomChanged_ (std::move (onZoomChanged)),
      onTooltipsChanged_ (std::move (onTooltipsChanged)),
      onSmoothingChanged_ (std::move (onSmoothingChanged)),
      onOversamplingChanged_ (std::move (onOversamplingChanged)),
      onLanguageChanged_ (std::move (onLanguageChanged)),
      onRefreshChanged_ (std::move (onRefreshChanged))
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

    // ---- Zoom: three buttons (the old top-bar controls moved here,
    // 2026-08-20) + a percentage readout. Steps of 0.1 on the same [0.75, 2.0]
    // clamp the editor's applyZoom uses; the value persists through
    // proc_.setUiZoom exactly like the old slider did. ----
    zoomLabel_.setText (TRANS ("Zoom"), juce::dontSendNotification);
    zoomLabel_.setFont (juce::FontOptions (14.0f));
    zoomLabel_.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (zoomLabel_);

    auto applyZoomFromSettings = [this] (double v)
    {
        v = juce::jlimit (0.75, 2.0, v);
        // Snap to the 0.05 grid the persisted values historically used so a
        // step from a restored odd value lands on a clean percentage.
        v = juce::roundToInt (v * 20.0) / 20.0;
        proc_.setUiZoom (v);
        refreshZoomReadout();
        if (onZoomChanged_)
            onZoomChanged_ (v);
    };
    zoomOutBt_.setTooltip (TRANS ("Zoom out"));
    zoomOutBt_.onClick = [this, applyZoomFromSettings]
        { applyZoomFromSettings (proc_.getUiZoom() - 0.1); };
    zoomInBt_.setTooltip (TRANS ("Zoom in"));
    zoomInBt_.onClick = [this, applyZoomFromSettings]
        { applyZoomFromSettings (proc_.getUiZoom() + 0.1); };
    zoomResetBt_.setTooltip (TRANS ("Reset zoom"));
    zoomResetBt_.onClick = [applyZoomFromSettings] { applyZoomFromSettings (1.0); };
    addAndMakeVisible (zoomOutBt_);
    addAndMakeVisible (zoomInBt_);
    addAndMakeVisible (zoomResetBt_);

    zoomValueLabel_.setFont (juce::FontOptions (14.0f));
    zoomValueLabel_.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (zoomValueLabel_);
    refreshZoomReadout();

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
    // filter's aliasing for higher fidelity. The item ID is the factor
    // (1/2/4/8). Default "High" (2x); "Standard" (1x) keeps the audio path
    // bit-identical.
    osLabel_.setText (TRANS ("Filter Quality"), juce::dontSendNotification);
    osLabel_.setFont (juce::FontOptions (14.0f));
    osLabel_.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (osLabel_);

    // The combo items are chrome (localised); the IDs (1/2/4/8) are stable and
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

    // HIG touch target: the three combos keep their compact 28pt DRAWN box but
    // get a 44pt tap band centred on their row (see resized — the extra band is
    // transparent padding into the row gaps, no sibling moves). The L&F reads
    // the "parvatiComboVisualH" property (drawComboBox / positionComboBoxText).
    for (auto* c : { &themeCombo_, &osCombo_, &langCombo_, &refreshCombo_ })
        c->getProperties().set ("parvatiComboVisualH", 28);

    // ---- Arp Clock (manual tempo fallback) ----
    // Hosts that expose no musical context to the plugin (GarageBand-class
    // AUv3 hosts; the Standalone app) used to run the arpeggiator clock at a
    // hard-coded 120 BPM. processBlock now resolves HOST bpm when the
    // playhead carries one, else THIS slider's value (persisted in the plugin
    // state). The status line shows which source is driving the clock right
    // now (2 Hz refresh — the source can only change when audio runs).
    clockLabel_.setText (TRANS ("Arp Clock"), juce::dontSendNotification);
    clockLabel_.setFont (juce::FontOptions (14.0f));
    clockLabel_.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (clockLabel_);

    clockStatusLabel_.setFont (juce::FontOptions (12.0f));
    clockStatusLabel_.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (clockStatusLabel_);

    bpmSlider_.setSliderStyle (juce::Slider::LinearHorizontal);
    bpmSlider_.setTextBoxStyle (juce::Slider::TextBoxRight, false, 72, 20);
    bpmSlider_.setRange (40.0, 300.0, 1.0);
    bpmSlider_.setNumDecimalPlacesToDisplay (0);
    bpmSlider_.textFromValueFunction = [] (double v)
        { return juce::String (juce::roundToInt (v)) + TRANS (" BPM"); };
    bpmSlider_.valueFromTextFunction = [] (const juce::String& t)
        { return t.upToFirstOccurrenceOf (TRANS (" BPM"), false, false).getDoubleValue(); };
    bpmSlider_.setValue ((double) proc_.getManualTempoBpm(), juce::dontSendNotification);
    bpmSlider_.onValueChange = [this]
        { proc_.setManualTempoBpm (juce::roundToInt (bpmSlider_.getValue())); };
    addAndMakeVisible (bpmSlider_);

    // ---- Visual Refresh (live mod-feedback animation cadence) ----
    // Caps how often the live modulation feedback repaints (CentralModBar
    // history strips, EnvelopeDisplay stage markers, FilterResponseDisplay
    // live curve — docs/LIVE_MOD_FEEDBACK_DESIGN.md). Every poll is
    // change-gated, so a higher rate only smooths animation; a LOWER rate is
    // the knob to turn on GPU-constrained hosts (short AUv3 panes). The item
    // ID is the Hz value itself (persisted exactly via setUiRefreshHz); a
    // persisted value between the offered steps snaps the combo's DISPLAY to
    // the nearest step while the processor keeps its exact value until the
    // user actually changes the combo.
    refreshLabel_.setText (TRANS ("Visual Refresh"), juce::dontSendNotification);
    refreshLabel_.setFont (juce::FontOptions (14.0f));
    refreshLabel_.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (refreshLabel_);

    populateRefreshCombo();
    refreshCombo_.setSelectedId (snapRefreshChoice (proc_.getUiRefreshHz()),
                                 juce::dontSendNotification);
    refreshCombo_.setTooltip (TRANS ("Animation rate of the live modulation indicators"));
    refreshCombo_.onChange = [this] {
        const int hz = refreshCombo_.getSelectedId();
        proc_.setUiRefreshHz (hz);
        if (onRefreshChanged_)
            onRefreshChanged_ (hz);
    };
    addAndMakeVisible (refreshCombo_);

    refreshClockStatus();
    startTimerHz (2);   // clock-status line only (gated by isShowing())
}

void SettingsPanel::setZoomValue (double)
{
    // Reflect a zoom change made elsewhere (the editor's keyboard shortcuts) —
    // buttons have no value-change callback to suppress, so this only
    // refreshes the readout (no onZoomChanged_ re-fire, no feedback loop).
    refreshZoomReadout();
}

void SettingsPanel::refreshZoomReadout()
{
    zoomValueLabel_.setText (
        juce::String (juce::roundToInt (proc_.getUiZoom() * 100.0)) + "%",
        juce::dontSendNotification);
}

void SettingsPanel::timerCallback()
{
    // 2 Hz, message thread. The source flag only changes when audio runs; a
    // hidden panel (side panel closed) does no work at all.
    if (isShowing())
        refreshClockStatus();
}

void SettingsPanel::refreshClockStatus()
{
    // Which source is driving the arpeggiator clock RIGHT NOW. Host present:
    // show the live value so the user sees why the slider is inert; absent:
    // say so explicitly (the slider is the active tempo).
    clockStatusLabel_.setText (
        proc_.isHostTempoPresent()
            ? TRANS ("Host tempo: ") + juce::String (proc_.getLastClockBpm(), 1)
              + TRANS (" BPM (manual ignored)")
            : TRANS ("No host tempo - manual tempo active"),
        juce::dontSendNotification);
}

void SettingsPanel::populateOversamplingCombo()
{
    osCombo_.clear (juce::dontSendNotification);
    // CharPointer_UTF8 (not a bare const char*) so the U+00D7 MULTIPLICATION
    // SIGN is decoded as UTF-8 by juce::translate, not latin1 (which would
    // mojibake and assert). Keeps the English combo text byte-identical.
    osCombo_.addItem (TRANS (juce::CharPointer_UTF8 ("Standard (1\xc3\x97)")), 1);   // 1x
    osCombo_.addItem (TRANS (juce::CharPointer_UTF8 ("High (2\xc3\x97)")),     2);   // 2x
#if ! JUCE_IOS
    // F-ios-perf-1 (iOS hunt 2026-08-19): the filter-oversampling workload is
    // PER-VOICE (96 voices at max polyphony). Measured on the repo's own
    // harness (M-series core, 96 voices, 48 kHz, 256-sample blocks): 1x =
    // 0.13x realtime, 2x = 0.26x, 4x = 0.48x, 8x = 0.93x. An A12-class iPad
    // core is ~2.5-4x slower for this float/filter workload => 8x is a
    // guaranteed 2.3-3.7x REALTIME (dropouts at max polyphony), 4x is
    // 1.2-1.9x (also overrun). iOS therefore offers only 1x/2x; a restored
    // state that requests 4x/8x is clamped in setStateInformation (see
    // PluginProcessor.cpp). Desktop keeps the full range unchanged.
    osCombo_.addItem (TRANS (juce::CharPointer_UTF8 ("Maximum (4\xc3\x97)")),  4);   // 4x
    osCombo_.addItem (TRANS (juce::CharPointer_UTF8 ("Ultra (8\xc3\x97)")),    8);   // 8x
#endif
}

void SettingsPanel::populateRefreshCombo()
{
    refreshCombo_.clear (juce::dontSendNotification);
    // Item IDs == the Hz values (stable across languages; persisted exactly).
    // "30 Hz (Default)" names the shipped default so a fresh install reads as
    // a choice, not a mystery number.
    refreshCombo_.addItem (TRANS ("10 Hz"),             10);
    refreshCombo_.addItem (TRANS ("15 Hz"),             15);
    refreshCombo_.addItem (TRANS ("30 Hz (Default)"),   30);
    refreshCombo_.addItem (TRANS ("60 Hz"),             60);
}

int SettingsPanel::snapRefreshChoice (int hz) const
{
    // Nearest offered step (10/15/30/60). The combo can only display the
    // offered steps; the processor keeps the exact persisted value until the
    // user changes the combo (so a restored 45 Hz still animates at 45 Hz
    // while the combo honestly shows the nearest available choice).
    int best = 30, bestDist = 1 << 30;
    for (int step : { 10, 15, 30, 60 })
    {
        const int d = hz > step ? hz - step : step - hz;   // |hz - step| without an include
        if (d < bestDist) { bestDist = d; best = step; }
    }
    return best;
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
    clockLabel_.setText (TRANS ("Arp Clock"), juce::dontSendNotification);
    refreshLabel_.setText (TRANS ("Visual Refresh"), juce::dontSendNotification);
    // (The BPM text lambdas evaluate TRANS() at invocation time, so a live
    // language switch re-resolves them with no re-assignment needed.)
    refreshClockStatus();

    const int osId = osCombo_.getSelectedId();
    populateOversamplingCombo();
    osCombo_.setSelectedId (osId, juce::dontSendNotification);

    // Same rebuild for the refresh combo: TRANS() item text, stable Hz item
    // IDs, selection preserved across the rebuild.
    const int refreshId = refreshCombo_.getSelectedId();
    populateRefreshCombo();
    refreshCombo_.setSelectedId (refreshId != 0 ? refreshId : 30, juce::dontSendNotification);

    repaint();
}

void SettingsPanel::paint (juce::Graphics& g)
{
    // Fill with the active theme's window background for a seamless look with
    // the rest of the editor. Controls inherit their colours from the
    // editor-wide ParvatiLookAndFeel (no manual setColour calls).
    g.fillAll (themeManager_.getCurrentTheme().backgroundBase);
}

int SettingsPanel::computePreferredHeight() const
{
    // The FULL no-scroll height of every row + margins — kept in lockstep
    // with resized()'s row budget (a static self-check below pins the sum so
    // the two can never drift apart silently).
    //   margins(16+16) + Theme(18+2+44) + 16 + Zoom(18+2+44) + 16
    // + Tooltips(44) + 8 + Smoothing(44) + 16 + Clock(18+2+18+8+44) + 16
    // + Refresh(18+2+44) + 16 + Filter(18+2+44) + 16 + Language(18+2+44)
    return 32 + 64 + 16 + 64 + 16 + 44 + 8 + 44 + 16 + 90 + 16 + 64 + 16 + 64 + 16 + 64;
}

void SettingsPanel::resized()
{
    // 2026-08-21: the settings drawer hosts this panel in a scrolling Viewport
    // (the editor's settingsScroll_), so EVERY row is reachable — the drawer
    // SCROLLS instead of degrading (the R3 hide-rows fallback only applies to
    // a no-viewport host, e.g. an embedder that sizes us directly). When a
    // Viewport parents us and we are shorter than the full row budget, size
    // ourselves once to it (pinned at the viewport origin); setSize re-enters
    // resized() at the full height and the layout below consumes it exactly.
    if (auto* vp = dynamic_cast<juce::Viewport*> (getParentComponent()))
    {
        const int prefH = juce::jmax (computePreferredHeight(), vp->getViewHeight());
        if (getHeight() != prefH || getWidth() != vp->getViewWidth())
        {
            setTopLeftPosition (0, 0);
            setSize (vp->getViewWidth(), prefH);
            return;
        }
    }

    auto area = getLocalBounds().reduced (16, 16);

    // Rows that no longer fit (a compacted drawer) are HIDDEN rather than
    // laid out beyond the panel bottom — the drawer degrades to showing what
    // fits instead of controls spilling over its edge (R3).
    auto takeRow = [&area] (int h)
    {
        if (area.getHeight() < h)
            return juce::Rectangle<int>();
        return area.removeFromTop (h);
    };
    const auto rowOrHide = [] (juce::Component* c, const juce::Rectangle<int>& r)
    {
        if (r.isEmpty()) { c->setVisible (false); return; }
        c->setVisible (true);
        c->setBounds (r);
    };

    // R3: combo/toggle rows are 44pt tall outright (the HIG tap minimum) —
    // NOT a 28pt row grown by a centred transparent band. The band trick
    // spilled 8pt into the caption/gap bands above and below, overlapping the
    // caption Labels (and off the panel bottom on the last row) when the panel
    // was compacted. The visual box stays 28pt via the "parvatiComboVisualH"
    // property; only the hit area is 44pt, and it now owns its own row.
    const int comboRowH = 44;
    const int gap  = 8;

    // Theme row.
    rowOrHide (&themeLabel_, takeRow (18));
    takeRow (2);
    rowOrHide (&themeCombo_, takeRow (comboRowH));
    takeRow (gap + 8);

    // Zoom row (44pt HIG tap band — the three buttons + readout).
    rowOrHide (&zoomLabel_, takeRow (18));
    takeRow (2);
    {
        auto row = takeRow (comboRowH);
        // [ - ] [ 100% ] [ + ] [ Reset ] — 44pt square out/in buttons, a
        // 64pt Reset, readout centred between out/in.
        zoomResetBt_.setBounds (row.removeFromRight (64));
        row.removeFromRight (8);
        zoomInBt_.setBounds (row.removeFromRight (44));
        row.removeFromRight (8);
        zoomValueLabel_.setBounds (row.removeFromRight (56));
        row.removeFromRight (8);
        zoomOutBt_.setBounds (row.removeFromRight (44));
    }
    takeRow (gap + 8);

    // Tooltips row.
    rowOrHide (&tooltipsToggle_, takeRow (comboRowH));
    takeRow (gap);

    // Parameter Smoothing row.
    rowOrHide (&smoothingToggle_, takeRow (comboRowH));
    takeRow (gap + 8);

    // Arp Clock rows: caption, live source/status line, manual BPM slider
    // (44pt row: the slider thumb is a touch target — same HIG reasoning as
    // the combo rows). PLACED ABOVE Filter Quality / Language deliberately:
    // the R3 drawer degrades bottom-first (rows hide when they no longer
    // fit), and this block is the ONE control whose target hosts are exactly
    // the short-pane ones — a GarageBand-class AUv3 pane or a 1024×500
    // desktop window must still reach the manual tempo. At the desktop
    // default size every row (incl. Filter Quality + Language) is visible.
    rowOrHide (&clockLabel_, takeRow (18));
    takeRow (2);
    rowOrHide (&clockStatusLabel_, takeRow (18));
    takeRow (gap);
    rowOrHide (&bpmSlider_, takeRow (comboRowH));
    takeRow (gap + 8);

    // Visual Refresh row (live mod-feedback animation cadence,
    // docs/LIVE_MOD_FEEDBACK_DESIGN.md). PLACED WITH the perf-relevant block
    // (above Filter Quality / Language) for the same R3 reason as Arp Clock:
    // the drawer degrades bottom-first, and this is exactly the control a
    // constrained host (a short AUv3 pane burning GPU on animations) needs to
    // reach. At the desktop default size every row below stays visible too.
    rowOrHide (&refreshLabel_, takeRow (18));
    takeRow (2);
    rowOrHide (&refreshCombo_, takeRow (comboRowH));
    takeRow (gap + 8);

    // Filter Quality (oversampling) row.
    rowOrHide (&osLabel_, takeRow (18));
    takeRow (2);
    rowOrHide (&osCombo_, takeRow (comboRowH));
    takeRow (gap + 8);

    // Language row.
    rowOrHide (&langLabel_, takeRow (18));
    takeRow (2);
    rowOrHide (&langCombo_, takeRow (comboRowH));
}

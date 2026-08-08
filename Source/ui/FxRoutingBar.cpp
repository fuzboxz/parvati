// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See FxRoutingBar.h.

#include "FxRoutingBar.h"

#include "FxMasterEqCurve.h"
#include "PluginProcessor.h"   // ParvatiAudioProcessor complete type (getApvts)
#include "ThemeManager.h"
#include "ParvatiTheme.h"

//==============================================================================
namespace
{
    // LEFT-column layout constants (px).
    constexpr int kMargin       = 10;   // horizontal edge inset (matches the slot cards)
    constexpr int kLeftPct      = 45;   // left column width = 45% of the bar
    constexpr int kRowGap       = 5;    // vertical gap between left rows
    constexpr int kFlowLabelW   = 38;   // "FLOW:" label width
    constexpr int kFlowComboW   = 190;  // topology (FLOW) combo width
    constexpr int kMixLabelW    = 34;   // "MIX:" label width
    constexpr int kMixKnobSize  = 40;   // MIX rotary knob cell
}

//==============================================================================
FxRoutingBar::FxRoutingBar (ParvatiAudioProcessor& processor, ThemeManager& themeManager)
    : processor_ (processor), themeManager_ (themeManager)
{
    // ---- Title (far left, top) ----
    titleLabel_.setText (TRANS ("ROUTING & MASTER EQ"), juce::dontSendNotification);
    titleLabel_.setJustificationType (juce::Justification::centredLeft);
    titleLabel_.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    titleLabel_.setColour (juce::Label::textColourId, themeManager_.getCurrentTheme().textSecondary);
    addAndMakeVisible (titleLabel_);

    // ---- "FLOW:" + topology combo (bound to fx_topo; its OWN choice list) ----
    flowLabel_.setText (TRANS ("FLOW:"), juce::dontSendNotification);
    flowLabel_.setJustificationType (juce::Justification::centredRight);
    flowLabel_.setFont (juce::FontOptions (12.0f));
    flowLabel_.setColour (juce::Label::textColourId, themeManager_.getCurrentTheme().textSecondary);
    addAndMakeVisible (flowLabel_);

    if (auto* p = dynamic_cast<juce::AudioParameterChoice*> (processor_.getApvts().getParameter ("fx_topo")))
        topoCombo_.addItemList (p->choices, 1);
    topoCombo_.setTooltip (TRANS ("FX chain routing topology"));
    addAndMakeVisible (topoCombo_);
    topoAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        processor_.getApvts(), "fx_topo", topoCombo_);

    // ---- "MIX:" + a rotary knob bound to fx_mix + "Global Wet/Dry" caption ----
    mixLabel_.setText (TRANS ("MIX:"), juce::dontSendNotification);
    mixLabel_.setJustificationType (juce::Justification::centredRight);
    mixLabel_.setFont (juce::FontOptions (12.0f));
    mixLabel_.setColour (juce::Label::textColourId, themeManager_.getCurrentTheme().textSecondary);
    addAndMakeVisible (mixLabel_);

    mixKnob_.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    mixKnob_.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);   // value drawn in-arc by the L&F
    mixKnob_.setScrollWheelEnabled (false);
    mixKnob_.setTooltip (TRANS ("Global FX wet/dry"));
    addAndMakeVisible (mixKnob_);
    mixAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processor_.getApvts(), "fx_mix", mixKnob_);

    mixCaption_.setText (TRANS ("Global Wet/Dry"), juce::dontSendNotification);
    mixCaption_.setJustificationType (juce::Justification::centredLeft);
    mixCaption_.setFont (juce::FontOptions (10.0f));
    mixCaption_.setColour (juce::Label::textColourId, themeManager_.getCurrentTheme().textSecondary);
    addAndMakeVisible (mixCaption_);

    // ---- "Keep FX Tails on Bypass" toggle (fx_keep_tails is an Int 0/1, bound
    //      via a Value + Value::Listener — NOT a ButtonAttachment) ----
    keepTailsToggle_.setButtonText (TRANS ("Keep FX Tails on Bypass"));
    keepTailsToggle_.setTooltip (TRANS ("Let delay/reverb tails ring out when an FX slot is bypassed"));
    keepTailsToggle_.setColour (juce::ToggleButton::textColourId, themeManager_.getCurrentTheme().textSecondary);
    keepTailsToggle_.setColour (juce::ToggleButton::tickColourId, themeManager_.getCurrentTheme().accentPrimary);
    addAndMakeVisible (keepTailsToggle_);
    keepTailsValue_ = processor_.getApvts().getParameterAsValue ("fx_keep_tails");
    keepTailsValue_.addListener (this);
    keepTailsToggle_.onClick = [this]
    {
        // Write 0/1 (the Int param's denormalized value via getParameterAsValue).
        keepTailsValue_ = keepTailsToggle_.getToggleState() ? 1.0f : 0.0f;
    };
    syncKeepTails();

    // ---- RIGHT: master EQ curve (read-only getters, normalized 0..1) ----
    auto norm = [this] (const char* id) -> float
    {
        auto* p = processor_.getApvts().getParameter (id);
        return p != nullptr ? p->getValue() : 0.0f;
    };
    eqCurve_ = std::make_unique<FxMasterEqCurve> (
        [norm] { return norm ("fx_eq_low"); },
        [norm] { return norm ("fx_eq_mid"); },
        [norm] { return norm ("fx_eq_high"); });
    if (const auto* th = &themeManager_.getCurrentTheme())
        eqCurve_->setCategoryColour (th->catAudio);   // amber trace, like the Filter curve
    addAndMakeVisible (*eqCurve_);
}

FxRoutingBar::~FxRoutingBar()
{
    keepTailsValue_.removeListener (this);
}

//==============================================================================
void FxRoutingBar::syncKeepTails()
{
    keepTailsToggle_.setToggleState (juce::roundToInt (keepTailsValue_.getValue()) != 0,
                                     juce::dontSendNotification);
}

void FxRoutingBar::valueChanged (juce::Value& v)
{
    if (v.refersToSameSourceAs (keepTailsValue_))
        syncKeepTails();
}

//==============================================================================
void FxRoutingBar::paint (juce::Graphics& g)
{
    const auto& t = themeManager_.getCurrentTheme();
    g.fillAll (t.backgroundPanel);

    // Thin divider along the bottom edge separates the bar from the slot cards.
    g.setColour (t.divider);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());

    // A subtle vertical divider between the left (routing/mix/tails) + right
    // (master EQ) columns.
    const int divX = getLocalBounds().getX() + (getWidth() * kLeftPct) / 100;
    g.setColour (t.divider);
    g.drawVerticalLine (divX, 3.0f, (float) (getHeight() - 3));
}

void FxRoutingBar::resized()
{
    // Layout against the ACTUAL assigned height (getHeight()), not the reserved
    // kBarHeight — the host may clamp this bar; the rows follow.
    auto area = getLocalBounds().reduced (kMargin, 4);
    if (area.isEmpty())
        return;

    // ---- LEFT / RIGHT split ----
    const int leftW = (area.getWidth() * kLeftPct) / 100;
    auto left  = area.removeFromLeft (leftW).withTrimmedRight (kRowGap);
    auto right = area;   // master EQ curve

    // ---- LEFT column: stacked rows ----
    auto row = [&] (int h) -> juce::Rectangle<int>
    {
        return left.removeFromTop (juce::jmin (h, left.getHeight()));
    };

    // Row 1: title.
    titleLabel_.setBounds (row (16));
    if (left.getHeight() <= 0) return;
    left.removeFromTop (kRowGap);

    // Row 2: FLOW label + combo.
    {
        auto r = row (juce::jmin (22, left.getHeight()));
        auto r2 = r;
        flowLabel_.setBounds (r2.removeFromLeft (juce::jmin (kFlowLabelW, r2.getWidth())));
        const int ch = juce::jmin (22, r.getHeight());
        topoCombo_.setBounds (r.getX() + kFlowLabelW, r.getY() + (r.getHeight() - ch) / 2,
                              juce::jmin (kFlowComboW, juce::jmax (0, r.getWidth() - kFlowLabelW)), ch);
        if (left.getHeight() <= 0) return;
        left.removeFromTop (kRowGap);
    }

    // Row 3: MIX label + knob + caption.
    {
        auto r = row (juce::jmin (kMixKnobSize, left.getHeight()));
        mixLabel_.setBounds (r.removeFromLeft (juce::jmin (kMixLabelW, r.getWidth())));
        mixKnob_.setBounds (r.removeFromLeft (juce::jmin (kMixKnobSize, r.getWidth()))
                               .withSizeKeepingCentre (kMixKnobSize, kMixKnobSize));
        mixCaption_.setBounds (r);   // remaining = "Global Wet/Dry"
        if (left.getHeight() <= 0) return;
        left.removeFromTop (kRowGap);
    }

    // Row 4: Keep FX Tails toggle.
    keepTailsToggle_.setBounds (row (juce::jmin (22, left.getHeight())));

    // ---- RIGHT column: master EQ curve fills it ----
    if (eqCurve_ != nullptr && ! right.isEmpty())
        eqCurve_->setBounds (right);
}

//==============================================================================
void FxRoutingBar::applyThemeColors()
{
    const auto& t = themeManager_.getCurrentTheme();
    titleLabel_.setColour (juce::Label::textColourId, t.textSecondary);
    flowLabel_.setColour (juce::Label::textColourId, t.textSecondary);
    mixLabel_.setColour (juce::Label::textColourId, t.textSecondary);
    mixCaption_.setColour (juce::Label::textColourId, t.textSecondary);

    topoCombo_.setColour (juce::ComboBox::backgroundColourId, t.backgroundInput);
    topoCombo_.setColour (juce::ComboBox::outlineColourId, t.outline);
    topoCombo_.setColour (juce::ComboBox::textColourId, t.textPrimary);
    topoCombo_.setColour (juce::ComboBox::arrowColourId, t.textSecondary);

    keepTailsToggle_.setColour (juce::ToggleButton::textColourId, t.textSecondary);
    keepTailsToggle_.setColour (juce::ToggleButton::tickColourId, t.accentPrimary);

    if (eqCurve_ != nullptr)
        eqCurve_->setCategoryColour (t.catAudio);   // amber trace, re-tinted live on theme switch

    repaint();
}

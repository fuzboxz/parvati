// Copyright (c) 2026 Jozsef Ottucsak / Parvati.

#include "ParvatiLookAndFeel.h"

ParvatiLookAndFeel::ParvatiLookAndFeel()
{
    // Default to Carbon so theme_ is never null and every colour ID is set
    // before any component reads it. ParvatiEditor overrides this immediately
    // via setTheme(themeManager_.getCurrentTheme()).
    setTheme (carbonTheme());
}

void ParvatiLookAndFeel::setTheme (const ParvatiTheme& t)
{
    theme_ = &t;

    // ---- Slider (rotary + text box) ----
    setColour (juce::Slider::rotarySliderFillColourId,        t.knobArc);       // knob fill arc (theme accent)
    setColour (juce::Slider::rotarySliderOutlineColourId,      t.knobTrack);     // rotary background track (== outline in Carbon)
    setColour (juce::Slider::thumbColourId,                    t.accent);
    setColour (juce::Slider::textBoxTextColourId,              t.text);
    setColour (juce::Slider::textBoxBackgroundColourId,        t.panelBackground);
    setColour (juce::Slider::textBoxOutlineColourId,           t.outline);
    setColour (juce::Slider::textBoxHighlightColourId,         t.accent2);

    // ---- ComboBox ----
    setColour (juce::ComboBox::backgroundColourId,             t.panelBackground);
    setColour (juce::ComboBox::outlineColourId,                t.outline);
    setColour (juce::ComboBox::textColourId,                   t.text);
    setColour (juce::ComboBox::arrowColourId,                  t.accent);
    setColour (juce::ComboBox::buttonColourId,                 t.accent);
    setColour (juce::ComboBox::focusedOutlineColourId,         t.accent);

    // ---- PopupMenu (used by every ComboBox's drop-down) ----
    setColour (juce::PopupMenu::backgroundColourId,            t.panelBackground2);
    setColour (juce::PopupMenu::textColourId,                  t.text);
    setColour (juce::PopupMenu::headerTextColourId,            t.accent);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, t.accent2);
    setColour (juce::PopupMenu::highlightedTextColourId,       t.text);

    // ---- Label ----
    // Default label text is the dim caption colour (the dominant label usage:
    // the small control-name labels under each knob / combo). Bright headings
    // override this per-component with theme.accent.
    setColour (juce::Label::textColourId,                      t.textDim);
    setColour (juce::Label::backgroundColourId,                juce::Colour (0x00000000)); // transparent (preserve default)
    setColour (juce::Label::outlineColourId,                   t.outline);

    // ---- ScrollBar (page Viewports) ----
    setColour (juce::ScrollBar::backgroundColourId,            t.windowBackground);
    setColour (juce::ScrollBar::thumbColourId,                 t.outline);
    setColour (juce::ScrollBar::trackColourId,                 t.panelBackground);

    // ---- TextButton ----
    setColour (juce::TextButton::buttonColourId,               t.panelBackground);
    setColour (juce::TextButton::buttonOnColourId,             t.accent);
    setColour (juce::TextButton::textColourOffId,              t.text);
    setColour (juce::TextButton::textColourOnId,               t.windowBackground);

    // ---- TabbedComponent / TabbedButtonBar ----
    setColour (juce::TabbedComponent::backgroundColourId,      t.windowBackground);
    setColour (juce::TabbedComponent::outlineColourId,         t.outline);
    setColour (juce::TabbedButtonBar::tabTextColourId,         t.textDim);
    setColour (juce::TabbedButtonBar::frontTextColourId,       t.accent);
    setColour (juce::TabbedButtonBar::tabOutlineColourId,      t.outline);
    setColour (juce::TabbedButtonBar::frontOutlineColourId,    t.accent);

    // ---- GroupComponent (bordered panel headings, used from Phase 2b on) ----
    setColour (juce::GroupComponent::textColourId,             t.accent);
    setColour (juce::GroupComponent::outlineColourId,          t.outline);

    // ---- ToggleButton (Multi page voice-allocation bits) ----
    setColour (juce::ToggleButton::textColourId,               t.text);
    setColour (juce::ToggleButton::tickColourId,               t.accent);
    setColour (juce::ToggleButton::tickDisabledColourId,       t.textDim);

    // ---- SidePanel (Settings panel) ----
    // Without these the chrome falls back to LookAndFeel_V4's hardcoded
    // dark-grey scheme, so the panel looks alien on non-default themes.
    setColour (juce::SidePanel::backgroundColour,            t.panelBackground);
    setColour (juce::SidePanel::titleTextColour,             t.text);
    setColour (juce::SidePanel::shadowBaseColour,            t.windowBackground.darker());
    setColour (juce::SidePanel::dismissButtonNormalColour,   t.textDim);
    setColour (juce::SidePanel::dismissButtonOverColour,    t.accent);
    setColour (juce::SidePanel::dismissButtonDownColour,    t.accent);
}

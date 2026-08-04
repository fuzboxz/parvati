// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// ParvatiLookAndFeel — a juce::LookAndFeel_V4 subclass that restyles every
// stock component from the active ParvatiTheme. setTheme() stores the theme
// pointer AND calls setColour() for all of the standard colour IDs (Slider,
// ComboBox, PopupMenu, Label, ScrollBar, TextButton, TabbedComponent /
// TabbedButtonBar, GroupComponent, ToggleButton), so pages built from stock
// widgets pick up the palette automatically without any per-component
// setColour() calls — that is the whole point of routing colour through the
// L&F. Phase 2a of docs/UI_MODERNIZATION_PLAN.md.
//
// Drawing is delegated to the V4 base (which reads the colour IDs we set); the
// vector knob / custom draw overrides land in a later phase.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "ParvatiTheme.h"

//==============================================================================
class ParvatiLookAndFeel : public juce::LookAndFeel_V4
{
public:
    /** Defaults to the Carbon theme so theme_ is never null before setTheme(). */
    ParvatiLookAndFeel();

    /** Points theme_ at @p t and applies setColour() for every standard colour
        ID derived from the theme fields. Safe to call repeatedly (e.g. on theme
        change). */
    void setTheme (const ParvatiTheme& t);

    /** The active theme, or nullptr if setTheme() has never been called. */
    const ParvatiTheme* getTheme() const noexcept { return theme_; }

private:
    const ParvatiTheme* theme_ = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParvatiLookAndFeel)
};

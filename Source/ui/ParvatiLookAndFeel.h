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

    // App-wide font mode (mirrors PluginProcessor::uiFontMode_). Every stock
    // text surface resolves its font through appFont(), so switching the mode
    // updates combos, buttons, tab labels, popup menus, labels AND group-
    // component headings live (see the getters below + refreshFontsIn).
    enum AppFontMode : int
    {
        fontConsole  = 0,   // embedded GNU Unifont (DOS/retro) — DEFAULT
        fontSerif    = 1,   // system default serif
        fontSansSerif = 2,  // system default sans-serif
    };

    void setFontMode (int mode) { fontMode_ = mode; }
    int  getFontMode() const noexcept { return fontMode_; }

    // Per-widget font getters (virtual): EVERY text-drawing stock component is
    // routed through appFont(), so a font-mode switch reaches combos, buttons,
    // tab labels, popup-menu items, labels and group titles alike (previously
    // only combos/buttons + manually-refreshed labels updated).
    juce::Font getComboBoxFont   (juce::ComboBox&) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getPopupMenuFont  () override;
    juce::Font getLabelFont      (juce::Label&) override;
    juce::Font getTabButtonFont  (juce::TabBarButton&, float height) override;

    // V4 inherits V3's tab drawing, whose createTabTextLayout() builds the tab
    // label with a HARDCODED font and never consults getTabButtonFont() — so the
    // tab labels stayed in the default font regardless of the mode. Override the
    // whole drawTabButton (and the width measurement) to route the tab text
    // through appFont(), matching V3's background / outline exactly otherwise.
    void drawTabButton (juce::TabBarButton&, juce::Graphics&, bool isMouseOver, bool isMouseDown) override;
    int  getTabButtonBestWidth (juce::TabBarButton&, int tabDepth) override;

    // Group-component panel titles are drawn by the L&F with a hardcoded font;
    // overridden so the title family follows the active mode. Borderless: the
    // outline colour is transparent, so only the text is visible.
    void drawGroupComponentOutline (juce::Graphics&, int width, int height,
                                    const juce::String& text,
                                    const juce::Justification& position,
                                    juce::GroupComponent&) override;

    // A Font for the active mode at @p height/style: console -> embedded GNU
    // Unifont typeface (DOS/retro); serif/sansSerif -> the system default family.
    // Public so the editor can re-apply every cached Label font on a mode switch.
    juce::Font appFont (float height, int styleFlags) const;

    // Wider, rounded, brighter scrollbar thumb than the V4 default (which draws
    // a faint 1px-ish thumb that is hard to grab on the long param pages).
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height,
                        bool isVertical, int thumbStartPosition, int thumbSize,
                        bool isMouseOver, bool isMouseDown) override;

private:
    const ParvatiTheme* theme_ = nullptr;
    int fontMode_ = fontConsole;   // 0 = Console (Unifont), 1 = Serif, 2 = Sans Serif
    juce::Typeface::Ptr unifontTypeface_;   // embedded GNU Unifont (console mode)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParvatiLookAndFeel)
};

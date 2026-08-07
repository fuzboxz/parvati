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
// Custom colour ID set per-tab-button on the nested card tabs (ENV/LFO/ARP/SEQ,
// MOD MATRIX/MODIFIERS) and the GroupPager sub-tabs (ENV1/2/3, LFO1/2/3…).
// drawTabButton reads it to colour each tab by its FUNCTION CATEGORY
// (ENV=cyan, LFO=magenta, ARP=purple, SEQ=green, MOD*=amber) instead of one
// shared accent — a single TabbedButtonBar holds several categories, so the
// colour must travel with the individual TabBarButton. A JUCE colour ID in the
// user range (well above the stock IDs).
constexpr int parvatiTabCategoryColourId = 0x2F000001;

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

    // Per-widget font getters (virtual): EVERY text-drawing stock component is
    // routed through appFont() (the system default sans-serif), so combos,
    // buttons, tab labels, popup-menu items, labels and group titles all share
    // one UI family. The ASCII "PARVATI" logo keeps its own monospaced font
    // (see ParvatiEditor::paint) and is NOT routed through appFont().
    juce::Font getComboBoxFont   (juce::ComboBox&) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getPopupMenuFont  () override;
    juce::Font getLabelFont      (juce::Label&) override;
    juce::Font getTabButtonFont  (juce::TabBarButton&, float height) override;

    // V4 inherits V3's tab drawing, whose createTabTextLayout() builds the tab
    // label with a HARDCODED font and never consults getTabButtonFont() — so the
    // tab labels stayed in the default font regardless of the mode. Override the
    // whole drawTabButton (and the width measurement) to route the tab text
    // through appFont(), and render a flat contiguous SEGMENTED bar (see the .cpp
    // for the per-state fill / frame logic).
    void drawTabButton (juce::TabBarButton&, juce::Graphics&, bool isMouseOver, bool isMouseDown) override;
    int  getTabButtonBestWidth (juce::TabBarButton&, int tabDepth) override;

    // Group-component panels draw a 1px rectangular border whose TOP-LEFT edge
    // is broken by the title text (classic fieldset/legend look: the section
    // header sits IN the top border line, e.g. ┌── [ OSC 1 ] ───┐). Title font
    // follows the active mode; title renders in ALL CAPS.
    void drawGroupComponentOutline (juce::Graphics&, int width, int height,
                                    const juce::String& text,
                                    const juce::Justification& position,
                                    juce::GroupComponent&) override;

    // Rotary knobs: a thin 1px amber arc-ring (dim empty track + bright fill
    // arc) with the numeric value drawn in the centre of the ring. No pointer
    // line, no value box underneath (value integrated into the dial).
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider&) override;

    // ComboBox: flat selection chip — a 4px rounded frame (matching the pill
    // buttons), a thin outline stroke (no inset bevel), and a minimal ▼ chevron
    // in a subtle token colour (textDim, lifted to text while open),
    // right-aligned. Inline text via positionComboBoxText() reserves ~24px on the
    // right for the chevron so long choices never clip.
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH,
                       juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    // TextButton background: flat pill — 4px rounded corners, a solid fill
    // (accent when toggled on via buttonOnColourId, panel fill otherwise), and a
    // thin 1px stroke (outline by default, brightened toward text on hover/press,
    // full text colour when toggled on). No inner-shadow inset bevel (the bulky
    // 3D look is gone). IconButton (gear/undo/redo) paints itself and bypasses
    // this. (Text itself is drawn by drawButtonText via getTextButtonFont.)
    void drawButtonBackground (juce::Graphics&, juce::Button&,
                               const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    // The app-wide UI font: the system default sans-serif family at the given
    // @p height / @p styleFlags. Public so the editor can re-apply every cached
    // Label font (juce::Label caches its font, so a repaint alone is not enough).
    juce::Font appFont (float height, int styleFlags) const;

    // ToggleButton text is drawn by the L&F with a hardcoded default font; the
    // override routes the button text through appFont() so the "Tooltips" and
    // "Parameter Smoothing" toggles (and the Multi page voice-allocation bits)
    // follow the active font mode. Otherwise a faithful copy of V4.
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                           bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    // Hover tooltips: both the SIZING (getTooltipBounds) and the DRAWING
    // (drawTooltip) use JUCE's layoutTooltipText helper, which builds the text
    // in a hardcoded default font. Overridden so the tooltip text — and its
    // measured width/height — follow the active font mode.
    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText,
                                           juce::Point<int> screenPos,
                                           juce::Rectangle<int> parentArea) override;
    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;

    // Wider, rounded, brighter scrollbar thumb than the V4 default (which draws
    // a faint 1px-ish thumb that is hard to grab on the long param pages).
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height,
                        bool isVertical, int thumbStartPosition, int thumbSize,
                        bool isMouseOver, bool isMouseDown) override;

private:
    const ParvatiTheme* theme_ = nullptr;

    // Draws a section-header / emphasised label with extra weight (Font::bold
    // in the app sans-serif). @p colour + @p font are applied as-is; @p area
    // centres the text horizontally within it.
    void drawHeadingText (juce::Graphics&, const juce::String& text,
                          const juce::Font& font, juce::Rectangle<float> area,
                          juce::Colour colour);

    // Draws a single line of text with a GlyphArrangement (addLineOfText) so the
    // LAST glyph is never silently curtailed — unlike Graphics::drawText(..,
    // rect, .., false) which internally reshapes with wordWrapWidth ==
    // rect.getWidth() and can drop the final glyph on a sub-pixel kerning
    // difference. @p justification positions the text within @p area. Underline
    // (from @p font) is honoured by GlyphArrangement::draw.
    void drawTextUncurtained (juce::Graphics&, const juce::String& text,
                              const juce::Font& font, juce::Rectangle<float> area,
                              juce::Colour colour, juce::Justification justification);

    // Tooltip text layout in the active app font (JUCE's layoutTooltipText uses
    // a hardcoded default-sans). Shared by getTooltipBounds (sizing) + drawTooltip.
    juce::TextLayout tooltipTextLayout (const juce::String& text, juce::Colour colour) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParvatiLookAndFeel)
};

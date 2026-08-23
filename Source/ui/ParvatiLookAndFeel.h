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
// (ENV=cyan, LFO=magenta, ARP=purple, SEQ=green, MOD=purple family) instead of one
// shared accent — a single TabbedButtonBar holds several categories, so the
// colour must travel with the individual TabBarButton. A JUCE colour ID in the
// user range (well above the stock IDs).
constexpr int parvatiTabCategoryColourId = 0x2F000001;

//==============================================================================
// ModuleLamp — the ONE module enable/disable indicator widget (2026-08-20).
// Shared by the synth mod matrix rows, the FX mod matrix rows, and the FX
// slot cards' power toggles so all three render the IDENTICAL control: a
// centred round lamp filled with the theme's accentPrimary while ON and the
// theme's textDisabled grey while OFF/bypassed, an outline-colour ring that
// brightens on hover, and an optional lamp-centre pin (the FX card header
// aligns the dot to the title's optical middle while the HIT area stays the
// full bounds — the 44pt HIG floor everywhere). The dot scales with the
// band (jmin(w,h) * 0.68, capped 30pt): matrix rows' 44pt bands render a
// ~28-30pt dot (the user-requested "a bit bigger"); tighter header bands
// clamp proportionally. Unified deliberately on accentPrimary (the FX card
// previously used accentSecondary — the style mismatch the user reported).
class ParvatiModuleLamp : public juce::Button
{
public:
    ParvatiModuleLamp() : juce::Button ({}) { setClickingTogglesState (false); }

    // The dot's border-ring stroke width (user 2026-08-20: "increase the
    // border of the indicator lights a tiny bit"). Public so paint and the
    // style tests read ONE value.
    static constexpr float kLampBorderWidth = 2.5f;

    // Pin the drawn LAMP to an explicit centre, as an offset from the
    // button's TOP-LEFT (the hit area stays the FULL bounds). A negative x
    // keeps the default: centred in the bounds.
    void setLampCentreOffset (juce::Point<float> centreFromTopLeft)
    {
        lampCentre_ = centreFromTopLeft;
    }

    // Pin the drawn DOT to an explicit diameter (the hit area stays the FULL
    // bounds). <= 0 keeps the default: proportional (jmin(w,h)*0.68, capped).
    // The mod-matrix rows pin the FX-card size (15pt) so the synth matrix's
    // taller band does not render a bigger dot than the FX modules' (2026-08-20
    // user request: the matrix lamp must use the FX enable/disable size).
    void setLampDiameter (float d) { lampDiameter_ = d; }

    // Override the ON fill colour (e.g. the row's modulator category colour).
    // An unset (transparent) colour falls back to the theme accent. The OFF
    // state keeps the theme's disabled grey.
    void setOnColour (juce::Colour c) { onColour_ = c; }

    // Test hook: the ON colour this instance would paint with RIGHT NOW
    // (resolved through the inherited L&F's active theme, with the shared
    // fallback). Style-parity tests call this on lamps from BOTH the synth
    // matrix and the FX card and assert equality per theme.
    juce::Colour resolvedOnColourForTest() const;
    // Test hook: the current drawn dot diameter for the given bounds.
    static float dotDiameterFor (juce::Rectangle<int> bounds);

    void paintButton (juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override;

private:
    juce::Point<float> lampCentre_ { -1.0f, -1.0f };   // <0 x => centre in bounds
    float lampDiameter_ = -1.0f;                       // <=0 => proportional
    juce::Colour onColour_ {};                         // transparent => theme accent

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParvatiModuleLamp)
};

//==============================================================================
class ParvatiLookAndFeel : public juce::LookAndFeel_V4
{
public:
    // Popup-menu ROW height for every default-sized menu in the app (HIG
    // minimum touch target). Pinned by tests/ipad_hig_sizing_test.cpp; the two
    // menus that opt in explicitly (FxTypeCombo, the zoom overflow popup)
    // reference the same constant via withStandardItemHeight.
    static constexpr int kPopupRowHeight = 44;

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

    // Popup-menu ROW height: every default-sized popup in the app gets 44pt
    // rows (HIG minimum touch target) instead of JUCE's ~font*1.3 (~22pt at the
    // 15pt popup font). Consulted ONLY when a menu's Options carry no explicit
    // standardItemHeight — the two popups that already opt into
    // withStandardItemHeight(44) (the FX type picker + the zoom overflow
    // popup) stay byte-identical, and this override brings every other menu
    // (PresetBrowser's nested Factory>Bank menus, Settings, ParamControl
    // combos, PatchPage combos, right-click context menus) up to the same 44.
    // The width keeps the V4 base's text-measured value, so menus stay exactly
    // as wide as their content.
    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator,
                                    int standardMenuItemHeight,
                                    int& idealWidth, int& idealHeight) override;
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

    // Group-component panels: a FLAT borderless solid rounded-rect CARD (7px)
    // with NO outline / shadow — depth is implied only by the tonal step to the
    // window bg. The section title is a BOLD muted-gray header at the top-left
    // (ALL CAPS).
    void drawGroupComponentOutline (juce::Graphics&, int width, int height,
                                    const juce::String& text,
                                    const juce::Justification& position,
                                    juce::GroupComponent&) override;

    // Rotary knobs: a flat SOLID-arc dial — a dark-gray TRACK arc (full sweep)
    // with a bright accent FILL arc on top (start -> value). No pointer line /
    // end tick; the numeric value is drawn in the centre of the ring. (See the
    // .cpp note on why the value is retained centred rather than relocated below
    // the label.)
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider&) override;

    // Linear sliders (the Settings zoom slider + the pitch/mod wheels — both
    // inherit this LookAndFeel). NOTE: the Mod-Matrix depth sliders use their
    // OWN BipolarSliderLNF (MatrixViewBase.cpp), so they are NOT styled here.
    // Flat vector style: a dark rounded track, an accent FILL from the start
    // (or the centre for bipolar ranges) to the handle, and a flat solid circle
    // handle — no 3D bevel/shadow.
    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;

    // ComboBox: a flat DARK dropdown — a 5px rounded solid dark-gray fill with
    // NO outline / inset shadow / arrow bevel, crisp WHITE inline text and a
    // minimal ▼ chevron (light token, right-aligned). A mod-matrix SOURCE combo
    // also carries a 4px family-colour TAG strip on its far-left edge
    // (the "parvatiComboTag" property). Inline text via positionComboBoxText()
    // reserves ~24px on the right for the chevron so long choices never clip.
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH,
                       juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    // TextButton background: flat tonal block — 4px rounded corners, a solid
    // fill (accent when toggled on via buttonOnColourId, panel fill
    // otherwise), LIGHTER on hover and DARKER on press, with NO stroke / bevel
    // / shadow, EXCEPT buttons carrying the "parvatiButtonOutlined" component
    // property (e.g. the Patch page's Ambika export actions) which also
    // get a 1px rounded stroke derived from their text colour — proper button
    // chrome where the flat block would read as floating text. IconButton
    // (gear/undo/redo) paints itself and bypasses this. (Text itself is drawn
    // by drawButtonText via getTextButtonFont.)
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

//==============================================================================
// Shared fit-to-text ComboBox measurement (single source). Previously
// ParamControl::maxChoiceTextWidth and FxSlotCard's maxComboItemWidth each
// carried their own copy of the font resolution and the widest-item loop, so
// the two could drift (one rounding tweak away from different dropdown
// widths for the same text).

// The combo-list font a fit-to-text measurement must use: @p owner's active
// ParvatiLookAndFeel 14 pt plain font, or the default 14 pt font when another
// LookAndFeel is installed.
inline juce::Font comboListFont (const juce::Component& owner)
{
    if (auto* lnf = dynamic_cast<const ParvatiLookAndFeel*> (&owner.getLookAndFeel()))
        return lnf->appFont (14.0f, juce::Font::plain);
    return juce::Font (juce::FontOptions (14.0f));
}

// The widest string among @p combo's own items, @p extraChoices, and the
// combo's current text — the text width a fit-to-text ComboBox needs. Each
// call site adds its own chrome padding on top.
inline int widestComboTextWidth (const juce::Component& owner, const juce::ComboBox* combo,
                                 const juce::StringArray& extraChoices = {})
{
    const juce::Font f = comboListFont (owner);
    int widest = 0;
    if (combo != nullptr)
        for (int i = 0; i < combo->getNumItems(); ++i)
            widest = juce::jmax (widest, juce::GlyphArrangement::getStringWidthInt (f, combo->getItemText (i)));
    for (const auto& c : extraChoices)
        widest = juce::jmax (widest, juce::GlyphArrangement::getStringWidthInt (f, c));
    if (combo != nullptr)
        widest = juce::jmax (widest, juce::GlyphArrangement::getStringWidthInt (f, combo->getText()));
    return widest;
}

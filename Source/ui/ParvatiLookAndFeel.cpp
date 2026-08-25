// Copyright (c) 2026 Jozsef Ottucsak / Parvati.

#include "ParvatiLookAndFeel.h"

// True when the active theme opts into the Y2K era chrome. The gate is the
// theme NAME, so every other theme keeps its exact rendering path. The Y2K
// branches add Win98 bevels, a glossy panel sheen, chrome knob bezels and
// chrome tabs (see ParvatiTheme.cpp y2kTheme).
static bool isY2kChrome (const ParvatiTheme* theme) noexcept
{
    return theme != nullptr && theme->name == "Y2K";
}

// The Y2K theme's three OFL typefaces, embedded via juce_add_binary_data
// (NAMESPACE ParvatiFonts, see CMakeLists.txt). Resolved through the generated
// getNamedResource() instead of #include "BinaryData.h": parvati_logo_assets
// and parvati_factory_presets also generate a BinaryData.h, so a direct
// include would be ambiguous (the ParvatiLogo pattern). Each Typeface::Ptr is
// built ONCE and cached: createSystemTypefaceFor parses the TTF every call.
namespace ParvatiFonts
{
    const char* getNamedResource (const char* resourceNameUTF8, int& dataSizeInBytes);
}

namespace
{
juce::Typeface::Ptr loadEmbeddedFont (const char* resourceName)
{
    int size = 0;
    const char* data = ParvatiFonts::getNamedResource (resourceName, size);
    jassert (data != nullptr && size > 0);   // the binary-data target ships every listed resource
    if (data == nullptr || size <= 0)
        return {};   // defensive: fall back to the default family
    return juce::Typeface::createSystemTypefaceFor (data, static_cast<size_t> (size));
}

// Cached Y2K typefaces (null until first Y2K use; a nullptr theme never
// reaches them).
juce::Typeface::Ptr y2kHeaderTypeface()
{
    static const juce::Typeface::Ptr tf = loadEmbeddedFont ("MichromaRegular_ttf");
    return tf;
}
juce::Typeface::Ptr y2kLabelTypeface (bool bold)
{
    static const juce::Typeface::Ptr regular = loadEmbeddedFont ("PT_SansWebRegular_ttf");
    static const juce::Typeface::Ptr boldFace = loadEmbeddedFont ("PT_SansWebBold_ttf");
    return bold ? boldFace : regular;
}
}   // namespace

juce::Font ParvatiLookAndFeel::headerFont (float height) const
{
    // Y2K: Michroma (wide geometric techno face, OFL). Other themes resolve
    // to the shared app font so their rendering is bit-identical.
    if (isY2kChrome (theme_))
        if (const auto tf = y2kHeaderTypeface(); tf != nullptr)
            return juce::Font (juce::FontOptions (tf).withHeight (juce::jmax (8.0f, height)));
    return appFont (height, juce::Font::bold);
}

juce::Font ParvatiLookAndFeel::labelFont (float height, int styleFlags) const
{
    // Y2K: Michroma (round 4, 2026-08-25) — the LABEL face is the module-HEADER
    // face, so one type family carries every caption on the card (user
    // request: labels read as the headers do). Height stays at the 10-11 px
    // compact band; Michroma runs ~1.55x wider than the former PT Sans, and
    // juce::Label's 0.7 minimum-horizontal-scale absorbs the widest captions
    // (a mild squeeze, no ellipsis at the 10 px floor). PT Sans drops to the
    // FALLBACK role (still embedded under its OFL obligation) and serves
    // only when the Michroma payload fails to load. Other themes keep the
    // app font at the caller's height.
    if (isY2kChrome (theme_))
    {
        if (const auto tf = y2kHeaderTypeface(); tf != nullptr)
            return juce::Font (juce::FontOptions (tf).withHeight (juce::jlimit (10.0f, 11.0f, height)));
        if (const auto fb = y2kLabelTypeface ((styleFlags & juce::Font::bold) != 0); fb != nullptr)
            return juce::Font (juce::FontOptions (fb).withHeight (juce::jlimit (10.0f, 11.0f, height)));
    }
    return appFont (height, styleFlags);
}

juce::Font ParvatiLookAndFeel::wordmarkFont (float height) const
{
    // The brand wordmark on EVERY theme: Michroma — the SAME payload the Y2K
    // header face uses (user request). Falls back to the app font when the
    // embedded payload fails to load, so the wordmark always renders.
    if (const auto tf = y2kHeaderTypeface(); tf != nullptr)
        return juce::Font (juce::FontOptions (tf).withHeight (juce::jmax (8.0f, height)));
    return appFont (height, juce::Font::bold);
}

juce::Font ParvatiLookAndFeel::dataFont (float height, int styleFlags) const
{
    // The VT323 console face is RETIRED (2026-XX-XX): every data readout
    // (values, combo text, matrix numbers, dropdown lists) uses the shared
    // shared default app font on EVERY theme, Y2K included. Y2K keeps ONLY
    // its own special face (Michroma) for headers and labels; the terminal
    // console readout face is gone.
    return appFont (height, styleFlags);
}
namespace
{
// Win98-class raised bevel on a rounded card: a light edge on the top half,
// a dark edge on the bottom half. The two clipped strokes meet at the
// horizontal centre line and read as one moulded plastic rim.
void drawY2kBevel (juce::Graphics& g, const juce::Rectangle<float>& r, float corner,
                   juce::Colour light, juce::Colour dark, float weight)
{
    juce::Path rim;
    rim.addRoundedRectangle (r, corner);
    const auto half = r.withHeight (r.getHeight() * 0.5f).toNearestInt();
    g.saveState();
    g.reduceClipRegion (half);
    g.setColour (light);
    g.strokePath (rim, juce::PathStrokeType (weight));
    g.restoreState();
    g.saveState();
    g.reduceClipRegion (r.withTop (r.getY() + r.getHeight() * 0.5f).toNearestInt());
    g.setColour (dark);
    g.strokePath (rim, juce::PathStrokeType (weight));
    g.restoreState();
}
}   // namespace

namespace parvati
{

bool isY2kTheme (const ParvatiTheme* t) noexcept { return isY2kChrome (t); }

juce::Colour indicatorFor (const ParvatiTheme& t, juce::Colour categoryColour) noexcept
{
    // Y2K: ONE accent — the LCD green — carries every indicator (the data
    // screens, the traces, the dial arcs). The category rainbow stays on the
    // light themes, where it encodes function without vibrating.
    if (isY2kChrome (&t))
        return t.accentPrimary;
    return categoryColour;
}

juce::Colour onCardText (const ParvatiTheme* t, juce::Colour themeToken) noexcept
{
    // Text ON a chrome module card. The Y2K cards are DARK-STEEL chrome
    // (see paintChromeCard), so the grey caption tier promotes to WHITE
    // there (the grey read as grey-on-steel); every other theme keeps its
    // theme token unchanged.
    if (isY2kChrome (t))
        return juce::Colour (0xffffffff);
    return themeToken;
}

void paintChromeCard (juce::Graphics& g, const juce::Rectangle<float>& r,
                      float corner, const ParvatiTheme* t, float alpha)
{
    // Non-Y2K themes keep the flat tonal-lift card.
    if (! isY2kChrome (t))
    {
        if (t != nullptr)
        {
            g.setColour (t->containerFill.withMultipliedAlpha (alpha));
            g.fillRoundedRectangle (r, corner);
        }
        return;
    }
    // FLAT CARD on the hardware world: the former liquid-chrome gradient
    // and bevel are retired. The card is one flat fill of the card body tone
    // (containerFill). White text keeps its contrast on the solid steel.
    g.setColour (t->containerFill.withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (r, corner);
}

void paintChromeWindow (juce::Graphics& g, const juce::Rectangle<float>& r,
                        const ParvatiTheme* t)
{
    // The Y2K WINDOW chrome: a POURED-METAL sweep over the silver desktop.
    // A bright silver band at the top settles through brushed steel to a
    // darker pooling at the bottom edge — five stops, amplitude ~+26%/-14%
    // of the base tone, with a slight DIAGONAL skew (the metal pours at an
    // angle, not a flat vertical fade). Dark text on the bottom strip keeps
    // contrast: the pooling stays within 14% of the base silver. The CARD
    // sweep is stronger still (the dark cards pop off the silver window).
    // The window is a FLAT base fill: the former poured-metal sweep is
    // retired. Dark text keeps its contrast on the solid silver base.
    if (t != nullptr)
        g.fillAll (t->backgroundBase);
    else
        g.fillAll (juce::Colour (0xff808080));
    (void) r;
}

}   // namespace parvati

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
    // Y2K: the knob fill arcs carry the LCD GREEN (one accent everywhere);
    // the VALUE readout is the LED data text on the near-black well.
    setColour (juce::Slider::rotarySliderFillColourId,        t.accentPrimary);        // knob fill arc (theme brand accent)
    setColour (juce::Slider::rotarySliderOutlineColourId,      t.trackEmpty);      // rotary background track (recedes)
    setColour (juce::Slider::thumbColourId,                    t.accentPrimary);
    setColour (juce::Slider::textBoxTextColourId,
               isY2kChrome (&t) ? t.accentPrimary : t.textPrimary);
    setColour (juce::Slider::textBoxBackgroundColourId,        t.backgroundInput);
    setColour (juce::Slider::textBoxOutlineColourId,           juce::Colour (0x00000000)); // borderless text box
    setColour (juce::Slider::textBoxHighlightColourId,         t.accentSecondary);

    // ---- ComboBox (dark container, 1px outline, accent chevron) ----
    setColour (juce::ComboBox::backgroundColourId,             t.backgroundInput);
    setColour (juce::ComboBox::outlineColourId,                juce::Colour (0x00000000));   // borderless (drawComboBox draws no outline)
    // ComboBox text is always LIGHT: drawComboBox fills every dropdown with a
    // uniform dark gray, so the inline text reads crisp white. On the dark
    // themes that is exactly textPrimary; on a light theme (whose textPrimary
    // is dark for its light surfaces) a fixed light value keeps the
    // closed-dropdown text legible on the dark fill. Y2K: the dropdown VALUE
    // is a data readout — the neon LED green.
    if (isY2kChrome (&t))
        setColour (juce::ComboBox::textColourId, t.accentPrimary);
    else
        setColour (juce::ComboBox::textColourId,
                   t.isDark ? t.textPrimary : juce::Colour (0xfff6f6fa));
    setColour (juce::ComboBox::arrowColourId,                  t.accentPrimary);
    setColour (juce::ComboBox::buttonColourId,                 t.accentPrimary);
    setColour (juce::ComboBox::focusedOutlineColourId,         t.accentPrimary);

    // ---- PopupMenu (used by every ComboBox's drop-down) ----
    setColour (juce::PopupMenu::backgroundColourId,            t.backgroundInput);
    // Y2K: the open dropdown list is the LED data surface — the LCD
    // green on the near-black popup fill. The selection highlight is the
    // dim teal with near-black text (dark reads on the mid-teal fill).
    setColour (juce::PopupMenu::textColourId,
               isY2kChrome (&t) ? t.accentPrimary : t.textPrimary);
    setColour (juce::PopupMenu::headerTextColourId,            t.accentPrimary);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, t.accentSecondary);
    setColour (juce::PopupMenu::highlightedTextColourId,
               isY2kChrome (&t) ? juce::Colour (0xff0A0A0A) : t.textPrimary);

    // ---- Label ----
    // Control-name labels (knob/combo captions) render in the low-contrast
    // textSecondary tier (regular-weight gray), NOT the accent. Per-component
    // overrides (e.g. the version/status labels, section headings) still set a
    // specific colour. Y2K: the LABEL DEFAULT flips near-black — it feeds the
    // SILVER surfaces (settings drawer, dialogs). Labels on the dark chrome
    // cards set their own colour (ParamControl::applyThemeFonts → white; the
    // matrix rows and data screens override too), so no card text uses this
    // default.
    setColour (juce::Label::textColourId,
               isY2kChrome (&t) ? juce::Colour (0xff141C30) : t.textSecondary);
    setColour (juce::Label::backgroundColourId,                juce::Colour (0x00000000)); // transparent (preserve default)
    setColour (juce::Label::outlineColourId,                   juce::Colour (0x00000000)); // borderless

    // ---- ScrollBar (page Viewports) ----
    setColour (juce::ScrollBar::backgroundColourId,            t.backgroundBase);
    setColour (juce::ScrollBar::thumbColourId,                 t.accentPrimary);    // accent-coloured thumb
    setColour (juce::ScrollBar::trackColourId,                 t.backgroundInput);

    // ---- TextButton ----
    setColour (juce::TextButton::buttonColourId,               t.backgroundPanel);
    setColour (juce::TextButton::buttonOnColourId,             t.accentPrimary);
    setColour (juce::TextButton::textColourOffId,              t.textPrimary);
    setColour (juce::TextButton::textColourOnId,               t.backgroundBase);

    // ---- TabbedComponent / TabbedButtonBar ----
    setColour (juce::TabbedComponent::backgroundColourId,      t.backgroundBase);
    setColour (juce::TabbedComponent::outlineColourId,         juce::Colour (0x00000000));   // no card outline (flat / borderless)
    setColour (juce::TabbedButtonBar::tabTextColourId,         t.textSecondary);
    setColour (juce::TabbedButtonBar::frontTextColourId,       t.accentPrimary);
    setColour (juce::TabbedButtonBar::tabOutlineColourId,      t.outline);
    setColour (juce::TabbedButtonBar::frontOutlineColourId,    t.accentPrimary);

    // ---- GroupComponent (borderless solid rounded-rect panel CARDS; the title
    // is a bold BRIGHT header on the textPrimary tier — UI feedback 2026-08-20:
    // the old textSecondary titles read too dim on the dark themes (WCAG
    // 4.6-5.8:1 vs the card fill); drawn by drawGroupComponentOutline) ----
    setColour (juce::GroupComponent::textColourId,             t.textPrimary);
    setColour (juce::GroupComponent::outlineColourId,          juce::Colour (0x00000000));   // no outline (borderless cards)

    // ---- ToggleButton (Multi page voice-allocation bits) ----
    setColour (juce::ToggleButton::textColourId,               t.textPrimary);
    setColour (juce::ToggleButton::tickColourId,               t.accentPrimary);
    setColour (juce::ToggleButton::tickDisabledColourId,       t.textSecondary);

    // ---- SidePanel (Settings panel) ----
    // Without these the chrome falls back to LookAndFeel_V4's hardcoded
    // dark-grey scheme, so the panel looks alien on non-default themes.
    setColour (juce::SidePanel::backgroundColour,            t.backgroundPanel);
    setColour (juce::SidePanel::titleTextColour,             t.textPrimary);
    setColour (juce::SidePanel::shadowBaseColour,            t.backgroundBase.darker());
    setColour (juce::SidePanel::dismissButtonNormalColour,   t.textSecondary);
    setColour (juce::SidePanel::dismissButtonOverColour,    t.accentPrimary);
    setColour (juce::SidePanel::dismissButtonDownColour,    t.accentPrimary);
}

void ParvatiLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                            bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    // Faithful copy of LookAndFeel_V4::drawToggleButton, with the ONE change that
    // the button text is routed through appFont() so it uses the app sans-serif
    // family instead of the JUCE default. This reaches every ToggleButton: the
    // "Tooltips" + "Parameter Smoothing" toggles in the Settings panel and the
    // Multi page voice-allocation bits.
    auto fontSize = juce::jmin (15.0f, (float) button.getHeight() * 0.75f);
    auto tickWidth = fontSize * 1.1f;

    drawTickBox (g, button, 4.0f, ((float) button.getHeight() - tickWidth) * 0.5f,
                 tickWidth, tickWidth,
                 button.getToggleState(),
                 button.isEnabled(),
                 shouldDrawButtonAsHighlighted,
                 shouldDrawButtonAsDown);

    g.setColour (button.findColour (juce::ToggleButton::textColourId));
    g.setFont (appFont (fontSize, juce::Font::plain));   // <-- app sans-serif

    if (! button.isEnabled())
        g.setOpacity (0.5f);

    g.drawFittedText (button.getButtonText(),
                      button.getLocalBounds().withTrimmedLeft (juce::roundToInt (tickWidth) + 10)
                                             .withTrimmedRight (2),
                      juce::Justification::centredLeft, 10);
}

juce::TextLayout ParvatiLookAndFeel::tooltipTextLayout (const juce::String& text, juce::Colour colour) const
{
    // Mirrors juce::detail::LookAndFeelHelpers::layoutTooltipText but builds the
    // attributed string in the app sans-serif (the JUCE helper hardcodes a
    // default-sans 13px bold), so hover tooltips use the app family.
    const float tooltipFontSize = 13.0f;
    const int maxToolTipWidth = 400;

    juce::AttributedString s;
    s.setWordWrap (juce::AttributedString::WordWrap::byChar);
    s.setJustification (juce::Justification::centred);
    s.append (text, appFont (tooltipFontSize, juce::Font::plain), colour);

    juce::TextLayout tl;
    tl.createLayoutWithBalancedLineLengths (s, (float) maxToolTipWidth);
    return tl;
}

juce::Rectangle<int> ParvatiLookAndFeel::getTooltipBounds (const juce::String& tipText,
                                                           juce::Point<int> screenPos,
                                                           juce::Rectangle<int> parentArea)
{
    // Same geometry as LookAndFeel_V2::getTooltipBounds, but measures the text
    // in the app font so the tooltip window is sized to match how it is drawn.
    const juce::TextLayout tl (tooltipTextLayout (tipText, juce::Colours::black));

    auto w = (int) (tl.getWidth() + 14.0f);
    auto h = (int) (tl.getHeight() + 6.0f);

    return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                                 screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6)  : screenPos.y + 6,
                                 w, h)
             .constrainedWithin (parentArea);
}

void ParvatiLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    // Same background/outline as LookAndFeel_V4::drawTooltip, but the text is
    // laid out in the active appFont via tooltipTextLayout().
    juce::Rectangle<int> bounds (width, height);
    const float cornerSize = 5.0f;

    g.setColour (findColour (juce::TooltipWindow::backgroundColourId));
    g.fillRoundedRectangle (bounds.toFloat(), cornerSize);

    g.setColour (findColour (juce::TooltipWindow::outlineColourId));
    g.drawRoundedRectangle (bounds.toFloat().reduced (0.5f, 0.5f), cornerSize, 1.0f);

    tooltipTextLayout (text, findColour (juce::TooltipWindow::textColourId))
        .draw (g, juce::Rectangle<float> ((float) width, (float) height));
}

void ParvatiLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar& scrollbar,
                                        int x, int y, int width, int height,
                                        bool isVertical,
                                        int thumbStartPosition, int thumbSize,
                                        bool isMouseOver, bool isMouseDown)
{
    // Faint track behind the thumb — SPANS the bar edge to edge. The former
    // 1px top/bottom inset left a visible gap against the chrome bands (the
    // header above, the status strip below); the track now butts them exactly.
    g.setColour (scrollbar.findColour (juce::ScrollBar::trackColourId));
    g.fillRect (x, y, width, height);

    if (thumbSize <= 0)
        return;

    // A wide, rounded accent-coloured thumb centred in the bar — far easier to
    // grab than the V4 default thin thumb. Brightens on hover / drag.
    auto thumb = scrollbar.findColour (juce::ScrollBar::thumbColourId);   // == theme accent
    if (isMouseOver || isMouseDown)
        thumb = thumb.brighter (0.3f);

    const float corner = juce::jmin (4.0f, (float) width * 0.5f, (float) height * 0.5f);
    juce::Rectangle<float> r;
    if (isVertical)
    {
        const float tw = juce::jmin ((float) width * 0.7f, 12.0f);
        r = juce::Rectangle<float> ((float) x + ((float) width - tw) * 0.5f,
                                    (float) thumbStartPosition,
                                    tw, (float) thumbSize);
    }
    else
    {
        const float th = juce::jmin ((float) height * 0.7f, 12.0f);
        r = juce::Rectangle<float> ((float) thumbStartPosition,
                                    (float) y + ((float) height - th) * 0.5f,
                                    (float) thumbSize, th);
    }
    g.setColour (thumb);
    g.fillRoundedRectangle (r.reduced (0.5f), corner);
}

juce::Font ParvatiLookAndFeel::appFont (float height, int styleFlags) const
{
    // The single app-wide UI font: the system default sans-serif family. Kept
    // explicit (not an empty FontOptions) so the family never silently resolves
    // to a serif on platforms where the default is ambiguous. The header
    // "Parvati" wordmark is also routed through here (bold) so it shares the
    // app typeface.
    // Point sizes / weights are passed through unchanged.
    return juce::Font (juce::FontOptions (juce::Font::getDefaultSansSerifFontName(),
                                          height, styleFlags));
}

juce::Font ParvatiLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
    // The dropdown VALUE text renders in the default app font on every theme
    // (the VT323 console readout face is retired). The colour is set by the
    // caller's ComboBox::textColourId.
    return appFont (14.0f, juce::Font::plain);
}

juce::Font ParvatiLookAndFeel::getComboListFontPublic() const
{
    // Measurement twin of getComboBoxFont (const, for the header-inline
    // comboListFont helper): the 14 pt app sans on every theme.
    return appFont (14.0f, juce::Font::plain);
}

juce::Font ParvatiLookAndFeel::getTextButtonFont (juce::TextButton&, int)
{
    // Y2K: button labels are the module-HEADER face (Michroma) — labelFont
    // resolves to it, so the top-row [SYNTH]/[FX]/[GLOBAL] buttons, the
    // generator pills and every TextButton carry the header typography.
    if (isY2kChrome (theme_))
        return labelFont (14.0f, juce::Font::bold);
    return appFont (14.0f, juce::Font::plain);
}

juce::Font ParvatiLookAndFeel::getPopupMenuFont()
{
    // The drop-down list of every ComboBox (and the Save format menu). Without
    // this override, PopupMenu would always render in the JUCE default font;
    // this routes it through the app sans-serif. 14pt — the SAME height as
    // getComboBoxFont/getTextButtonFont so a combo's inline text and its
    // open list match (was 15pt; UI feedback 2026-08-20: the seq length
    // picker read noticeably larger than every other dropdown text). Same
    // default app face on every theme (the VT323 console face is retired).
    return appFont (14.0f, juce::Font::plain);
}

void ParvatiLookAndFeel::getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator,
                                                    int standardMenuItemHeight,
                                                    int& idealWidth, int& idealHeight)
{
    // UNIFIED (iOS style is the single default on every platform): default-
    // sized popup rows are raised to the 44pt HIG touch minimum. The V4 base
    // measures the width (text + padding, never clipped), which we keep as-is;
    // only the row HEIGHT is lifted. This method is consulted only for menus
    // whose Options carry NO explicit standardItemHeight, so the FX type picker
    // and the zoom overflow popup (both withStandardItemHeight(44)) are
    // unaffected — and every other menu now matches their 44pt rows exactly.
    // Separators keep the base sizing (half a row).
    juce::LookAndFeel_V4::getIdealPopupMenuItemSize (text, isSeparator, standardMenuItemHeight,
                                                     idealWidth, idealHeight);
    if (! isSeparator && standardMenuItemHeight <= 0)
        idealHeight = kPopupRowHeight;
}

juce::Font ParvatiLookAndFeel::getLabelFont (juce::Label& label)
{
    // A ComboBox's inline (closed) value label draws through getLabelFont:
    // JUCE's default drawLabel calls getLabelFont (not the label's stored
    // font), so without this branch the closed dropdown text would render in
    // the generic caption face (Michroma on Y2K) instead of the shared
    // control face. Defer those labels to getComboBoxFont, which is the same
    // face the open list uses and re-resolves on every paint.
    if (auto* combo = dynamic_cast<juce::ComboBox*> (label.getParentComponent()))
        return getComboBoxFont (*combo);

    // Other labels preserve their own height/style and only swap the family.
    const auto f = label.getFont();
    if (isY2kChrome (theme_))
        return labelFont (f.getHeight(), f.getStyleFlags());
    return appFont (f.getHeight(), f.getStyleFlags());
}

juce::Font ParvatiLookAndFeel::getTabButtonFont (juce::TabBarButton&, float height)
{
    // +40% over the legacy factor (0.33) so the redesigned button tabs are
    // prominent and easy to click; the family is the app sans-serif.
    // Y2K: tab text becomes the PT Sans caption face (neon carries the state,
    // not the typography).
    if (isY2kChrome (theme_))
        return labelFont (height * 0.46f, juce::Font::bold);
    return appFont (height * 0.46f, juce::Font::plain);
}

int ParvatiLookAndFeel::getTabButtonBestWidth (juce::TabBarButton& button, int tabDepth)
{
    // Measure the SAME all-caps label that drawTabButton renders, plus symmetric
    // side padding. The redesigned button tabs carry NO bracket chrome (the
    // `[ LABEL ]` motif was removed), so the old isCard / bracketW branch is
    // gone. padX is widened (6 -> 8) to give the larger tab text a comfortable
    // hit area. Matches drawTabButton's layout exactly.
    const juce::Font font = getTabButtonFont (button, (float) tabDepth);
    constexpr int padX = 8;
    int width = juce::GlyphArrangement::getStringWidthInt (font, button.getButtonText().trim().toUpperCase())
              + 2 * padX
              + getTabButtonOverlap (tabDepth) * 2;

    if (auto* extraComponent = button.getExtraComponent())
        width += button.getTabbedButtonBar().isVertical() ? extraComponent->getHeight()
                                                          : extraComponent->getWidth();

    return juce::jlimit (tabDepth * 2, tabDepth * 8, width);
}

void ParvatiLookAndFeel::drawTabButton (juce::TabBarButton& button, juce::Graphics& g,
                                        bool isMouseOver, bool isMouseDown)
{
    // SEGMENTED tab bar with rounded-top tabs. Every tab (the nested
    // ENV|LFO|ARP|SEQ + MOD MATRIX|MODIFIERS cards, OSC1|OSC2, SYNTH) is a
    // rounded-top segment (only the UPPER-LEFT / UPPER-RIGHT corners are rounded;
    // the bottom is square and flush with the content card). Only the rounded TOP
    // edge is outlined — NOT a per-tab vertical outline — so adjacent tabs read
    // as one contiguous segmented control instead of discrete buttons:
    //   - active (front): SOLID highlight background (tabSelectedBg tinted toward
    //     accent for a clear on-brand accent segment) + bright text + the prominent
    //     full-width tabUnderline.
    //   - unselected: subtle panelBackground fill + dim text (textDim); lifted on
    //     hover for affordance.
    // All colours come from the active ParvatiTheme tokens (theme_). Horizontal
    // padding is owned by getTabButtonBestWidth (padX 8); the label is centred in
    // the full tab width so the two stay in sync. (Every Parvati tab bar is
    // TabsAtTop, so only the horizontal layout is handled here.)
    if (theme_ == nullptr)
    {
        juce::LookAndFeel_V4::drawTabButton (button, g, isMouseOver, isMouseDown);
        return;
    }

    const auto activeArea = button.getActiveArea().toFloat();
    const bool front = button.isFrontTab();
    const bool hover = (isMouseOver || isMouseDown) && ! front;

    // Per-tab FUNCTION-CATEGORY colour (set on each TabBarButton via
    // parvatiTabCategoryColourId by SynthWorkspace / GroupPager). Falls back to
    // the shared tabUnderline token for any tab without a category assigned (the
    // default look is unchanged). findColour returns opaque-black for an unset
    // ID, and no category token is opaque-black, so the comparison is a safe
    // unset-detect. The category hue drives the active highlight + underline and
    // a dimmed underline on inactive tabs so every tab reads as its function
    // (ENV=cyan, LFO=magenta, ARP=purple, SEQ=green, MOD=purple family).
    //
    // Robustness: juce::TabbedButtonBar can (re)create tab buttons (on resize /
    // add / setCurrentTabIndex), and the headless screen tool switches tabs
    // before painting — so an explicitly-set category colour may be absent on a
    // freshly-created button. When it is unset, DERIVE the category colour from
    // the tab's OWN text via the active theme, so tabs are ALWAYS correctly
    // coloured at paint time regardless of button recreation (the explicit
    // setColour stays the primary path; this only fills the gap).
    juce::Colour catColour = button.findColour (parvatiTabCategoryColourId, false);
    if (catColour == juce::Colours::black)
    {
        const juce::String tabText = button.getButtonText().trim().toUpperCase();
        if      (tabText.startsWith ("ENV")) catColour = theme_->catEnv;     // Envelopes / ENV 1-3
        else if (tabText.startsWith ("LFO")) catColour = theme_->catLfo;     // LFOs / LFO 1-3
        else if (tabText.startsWith ("ARP")) catColour = theme_->catArp;     // Arpeggiator
        else if (tabText.startsWith ("SEQ")) catColour = theme_->catSeq;     // Sequencer / SEQ 1-2
        else if (tabText.startsWith ("MOD")) catColour = theme_->catAudio;   // MOD MATRIX / MODIFIERS
        else                                 catColour = theme_->accentPrimary;
    }

    // Modern rounded-TOP tabs. Each tab's fill is a rectangle with a square
    // bottom (flush with the content card) and selectively-rounded TOP corners
    // (radius kTabCorner). Corner rounding is chosen so the bar stays contiguous
    // with NO background gaps between segments: a tab paints only within its own
    // (clipped) bounds, so a corner shared with a neighbour can only be rounded
    // on ONE side — the other side stays square and fills flush up to the seam.
    // So: the ACTIVE (front) tab rounds BOTH top corners (it reads as a
    // rounded-top button sitting on the bar), each bar-END tab rounds its OUTER
    // corner (rounds the bar's silhouette), and interior inactive tabs keep
    // square top corners (flush with their neighbours). Vertical sides are
    // never stroked (no per-tab vertical outlines). Colours are unchanged
    // (active = tabSelectedBg + category tint; inactive = panel fill, hover-lifted).
    constexpr float kTabCorner = 5.0f;
    const int numTabs = button.getTabbedButtonBar().getNumTabs();
    const int idx     = button.getIndex();
    const bool roundTL = front || (idx <= 0);
    const bool roundTR = front || (idx >= numTabs - 1);

    juce::Path tabShape;
    tabShape.addRoundedRectangle (activeArea.getX(), activeArea.getY(),
                                  activeArea.getWidth(), activeArea.getHeight(),
                                  kTabCorner, kTabCorner,
                                  roundTL ? 1.0f : 0.0f,   // top-left
                                  roundTR ? 1.0f : 0.0f,   // top-right
                                  0.0f, 0.0f);             // square bottom (flush)

    if (isY2kChrome (theme_))
    {
        // Y2K: STRUCTURE stays neutral; the neon is an INDICATOR, never a
        // fill. Active = neutral steel segment + a 2 px LCD-GREEN strip
        // along the tab TOP + near-white text. Inactive = dark steel, dim
        // text, no accent. One accent everywhere (the LCD green).
        if (front)
        {
            g.setColour (theme_->tabSelectedBg);
            g.fillPath (tabShape);
            g.setColour (theme_->accentPrimary);
            g.fillRect (juce::Rectangle<float> (activeArea.getX(), activeArea.getY(),
                                                activeArea.getWidth(), 2.0f));
        }
        else
        {
            g.setColour (hover ? theme_->tabUnselectedBg.brighter (0.10f)
                               : theme_->tabUnselectedBg);
            g.fillPath (tabShape);
        }
    }
    else if (front)
    {
        g.setColour (theme_->tabSelectedBg);
        g.fillPath (tabShape);
        g.setColour (catColour.withMultipliedAlpha (0.22f));
        g.fillPath (tabShape);
    }
    else
    {
        g.setColour (hover ? theme_->backgroundPanel.brighter (0.15f)
                           : theme_->backgroundPanel);
        g.fillPath (tabShape);
    }

    // FLAT segmented look: NO top outline and NO bottom rule (the depth lines
    // are eliminated — segmentation reads from the active/inactive fill contrast
    // plus the per-tab category underline drawn below, not from box outlines).

    // Label (ALL CAPS), centred; reserve room at the bottom for the active
    // underline.
    juce::Colour textCol = front ? theme_->textPrimary : theme_->textSecondary;
    if (hover)
        textCol = textCol.brighter (0.20f);

    juce::Font font (getTabButtonFont (button, activeArea.getHeight()));
    font.setUnderline (button.hasKeyboardFocus (false));
    const juce::String label = button.getButtonText().trim().toUpperCase();

    const float bottomReserve = front ? 4.0f : 2.0f;   // room for the underline
    g.setFont (font);
    drawTextUncurtained (g, label, font,
                         juce::Rectangle<float> (activeArea.getX(), activeArea.getY(),
                                                 activeArea.getWidth(), activeArea.getHeight() - bottomReserve),
                         textCol, juce::Justification::centred);

    // Per-tab CATEGORY underline: the ACTIVE tab gets a prominent full-width
    // line in its category hue; INACTIVE tabs get the same line dimmed (0.32
    // alpha). Y2K keeps only the TOP indicator strip on the active tab: the
    // bottom underline draws for every OTHER theme and is skipped on Y2K so
    // the neon stays scarce (structure neutral, indicators only).
    if (! isY2kChrome (theme_))
    {
        g.setColour (front ? catColour : catColour.withMultipliedAlpha (0.32f));
        g.fillRect (juce::Rectangle<float> (activeArea.getX(),
                                            activeArea.getBottom() - 2.0f,
                                            activeArea.getWidth(), 2.0f));
    }
}

void ParvatiLookAndFeel::drawGroupComponentOutline (juce::Graphics& g, int width, int height,
                                                     const juce::String& text,
                                                     const juce::Justification& position,
                                                     juce::GroupComponent& group)
{
    // FLAT borderless CARD: a solid rounded-rect (7px) filled with the panel
    // background, sitting LIGHTER than the window for tonal separation. NO
    // outline and NO skeuomorphic depth ring / inset shadow — those effects are
    // eliminated, so depth is implied ONLY by the tonal step between the window
    // bg and the card. The section title is a BOLD BRIGHT header at the
    // top-left (the old fieldset/legend border-break motif is gone). The card's
    // geometry (panel position / size) is unchanged.
    constexpr float corner = 7.0f;
    const float textH = 14.0f;
    // Title geometry HARMONIZED with the FX cards (2026-08-23 user request:
    // "synth module titles as spacious as the ones on the FX page"): the
    // title band sits kGroupPad (8) below the card top — the same inset
    // FxSlotCard/FxRoutingBar draw their titles at (y = kPad) — instead of
    // hugging the top edge at y=3, and the left pad matches the FX routing
    // bar's kPad+2. One module-header rhythm on both pages.
    constexpr float titleLeftPad = 10.0f;
    constexpr float titleTopPad  = 8.0f;
    juce::ignoreUnused (position);

    const auto alpha = group.isEnabled() ? 1.0f : 0.5f;
    const auto cardBounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);

    // Solid card fill only — no outline, no skeuomorphic containerShadow ring.
    // The card reads purely by its tonal step above the window background.
    // Y2K swaps that for the shared liquid-chrome card body (see
    // parvati::paintChromeCard — the FX cards and routing bar use the same
    // painter, so every module card matches).
    parvati::paintChromeCard (g, cardBounds, corner, theme_, alpha);

    // Bold bright section header, left-aligned within the card's top band
    // (GroupComponent::textColourId == theme_->textPrimary after setTheme —
    // raised from textSecondary 2026-08-20 so module headers like "FX" /
    // "Sequencer" / "Osc 1" read clearly on every dark theme; see
    // parvati_ui_typography_test for the per-theme contrast table).
    const juce::Font f = appFont (textH, juce::Font::bold);
    const juce::String displayText = text.toUpperCase();
    // The module panels went DARK grey in the Y2K hardware restyle, so the
    // standard textPrimary tier (white on Y2K) reads directly — no flip.
    const juce::Colour titleCol = group.findColour (juce::GroupComponent::textColourId)
                                      .withMultipliedAlpha (alpha);
    drawHeadingText (g, displayText, f,
                     juce::Rectangle<float> (titleLeftPad, titleTopPad,
                                             (float) width - titleLeftPad * 2.0f, textH),
                     titleCol);
}

// A small padlock glyph drawn centred at @p c (size @p sz) — the "cannot drop
// here" indicator shown on non-destination controls while a mod-source drag is
// hovered over them. Theme-agnostic (caller supplies the colour).
static void drawPadlock (juce::Graphics& g, juce::Point<float> c, float sz, juce::Colour col)
{
    const float bodyW = sz * 0.66f;
    const float bodyH = sz * 0.52f;
    const auto  body  = juce::Rectangle<float> (c.x - bodyW * 0.5f,
                                                c.y - bodyH * 0.5f + sz * 0.14f,
                                                bodyW, bodyH);
    // Shackle: a half-circle whose FEET DESCEND INTO THE BODY (2026-08-23
    // revision 2 — "make the lock LOCKED; it reads open"): the arc's centre
    // sits BELOW the body's top edge, so both ends pass through the body's
    // interior and are covered by the body fill drawn after — the shackle
    // visibly ENTERS the body on both sides, the unambiguous closed-padlock
    // reading. (Also carries the earlier BUG FIX: addCentredArc with
    // startAsNewSubPath=false on a fresh path lineTo's from (0,0) — the stray
    // top-left line; startNewSubPath at the arc start prevents it.)
    juce::Path shackle;
    const float r = bodyW * 0.38f;
    const float shackleCY = body.getY() + r * 0.34f;   // feet land INSIDE the body
    shackle.startNewSubPath (c.x - r, shackleCY);
    shackle.addCentredArc (c.x, shackleCY, r, r, 0.0f,
                           juce::MathConstants<float>::pi,
                           juce::MathConstants<float>::twoPi, false);
    g.setColour (col);
    g.strokePath (shackle, juce::PathStrokeType (juce::jmax (1.4f, sz * 0.15f),
                                                 juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
    g.fillRoundedRectangle (body, juce::jmax (1.0f, sz * 0.14f));
    // Keyhole.
    g.setColour (col.contrasting (0.6f));
    g.fillEllipse (juce::Rectangle<float> (sz * 0.13f, sz * 0.13f).withCentre (body.getCentre()));
}

void ParvatiLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y,
                                           int width, int height,
                                           float sliderPos,
                                           float rotaryStartAngle,
                                           float rotaryEndAngle,
                                           juce::Slider& slider)
{
    // A flat SOLID-arc knob (no pointer line, no end tick): a dark-gray TRACK
    // arc (full sweep) with a bright accent FILL arc from the start angle to the
    // value angle on top of it. The numeric value is drawn in the CENTRE of the
    // ring (centre readout). NOTE: relocating the value BELOW the parameter
    // label is NOT applied here — the slider's bounds ARE the dial area (the
    // caption label is a separate sibling component laid out above it, in code
    // outside these files), and reserving a value band would either shrink the
    // dial into the concentric modulation arcs or clip outside the slider. The
    // value is therefore kept centred but refined (smaller, cleaner). Disabled
    // knobs (sequencer steps past the active length) draw only the track arc.
    const auto fill    = slider.findColour (juce::Slider::rotarySliderFillColourId);      // accent arc (brand / category)
    const auto valueCol = slider.findColour (juce::Slider::textBoxTextColourId);
    const auto trackCol = slider.findColour (juce::Slider::rotarySliderOutlineColourId);  // solid dark-gray track (knobTrack)

    // LOCKED-KNOB GREY-OUT (2026-08-23, two revisions): while a mod-source
    // drag hovers this knob as a NON-destination (parvatiModLocked via
    // ParamControl::setDropLocked), EVERYTHING accent-coloured on the dial
    // greys out to the padlock's light grey — the VALUE fill arc (the
    // dominant "indicator arc around the control"), the drop-target zone
    // ring and the per-modulation arcs — so the whole control reads
    // deactivated, not just the glyph. The dark TRACK keeps its colour (it
    // already reads as inactive chrome).
    const auto* lv = slider.getProperties().getVarPointer ("parvatiModLocked");
    const bool modLocked = (lv != nullptr && lv->isBool() && (bool) *lv);
    const juce::Colour lockedGrey = juce::Colour::greyLevel (0.72f);
    const juce::Colour effFill = modLocked ? lockedGrey : fill;

    // Square dial area centred in the given bounds.
    const int dial = juce::jmin (width, height);
    const auto bounds = juce::Rectangle<int> (x + (width  - dial) / 2,
                                              y + (height - dial) / 2,
                                              dial, dial).toFloat().reduced (2.0f);
    const auto centre = bounds.getCentre();
    const float radius = juce::jmax (4.0f, juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f);

    const float toAngle = [&]
    {
        // Clamp sliderPos into [0,1] defensively (JUCE passes a normalised
        // fraction between the rotary start/end angles).
        const float p = juce::jlimit (0.0f, 1.0f, sliderPos);
        return rotaryStartAngle + p * (rotaryEndAngle - rotaryStartAngle);
    }();

    // SOLID vector arc: a single thick stroke width is shared by the dark-gray
    // track (full sweep) and the accent fill (start -> value), so the fill sits
    // flush on top of the track. NO internal indicator line / end-of-arc tick.
    const float arcWidth = juce::jmax (2.5f, radius * 0.17f);

    // Background track arc — full sweep, solid dark gray.
    g.setColour (slider.isEnabled() ? trackCol : trackCol.withMultipliedAlpha (0.5f));
    juce::Path trackArc;
    trackArc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f,
                            rotaryStartAngle, rotaryEndAngle, true);
    g.strokePath (trackArc, juce::PathStrokeType (arcWidth, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));

    if (isY2kChrome (theme_))
    {
        // Y2K only: the chrome track lining. A thin bright line runs along the
        // top half of the track groove, a dim steel line along the bottom —
        // one polished bezel, lit from above. Sits UNDER the value fill arc.
        const auto dialRect = bounds.toNearestInt();
        const float chromeW = juce::jmax (1.0f, arcWidth * 0.35f);
        const juce::Colour chrome = modLocked ? lockedGrey : fill;
        g.saveState();
        g.reduceClipRegion (dialRect.withHeight (dialRect.getHeight() / 2));
        g.setColour (chrome.withMultipliedAlpha (0.9f));
        g.strokePath (trackArc, juce::PathStrokeType (chromeW, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
        g.restoreState();
        g.saveState();
        g.reduceClipRegion (dialRect.withY (dialRect.getY() + dialRect.getHeight() / 2));
        g.setColour (chrome.darker (0.55f).withMultipliedAlpha (0.9f));
        g.strokePath (trackArc, juce::PathStrokeType (chromeW, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
        g.restoreState();
    }

    // Fill arc — start angle -> value angle, bright accent. Skipped for disabled
    // knobs so they read as inactive. While a mod-source drag hovers this knob
    // as a NON-destination (modLocked), the fill greys out with everything
    // else (2026-08-23 revision 2: "the issue still exists" — the value arc
    // IS the indicator arc the user meant; greying only the outer mod rings
    // left the knob's dominant arc accented).
    if (slider.isEnabled() && rotaryEndAngle > rotaryStartAngle)
    {
        g.setColour (modLocked ? lockedGrey : fill);
        juce::Path fillArc;
        fillArc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f,
                               rotaryStartAngle, toAngle, true);
        g.strokePath (fillArc, juce::PathStrokeType (arcWidth, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
    }

    // Centre numeric readout for ACTIVE knobs only (disabled steps show just the
    // track arc, per spec). The value indicator is the knob's primary readout, so
    // it stays on the brightest text tier (Slider::textBoxTextColourId ==
    // theme.textPrimary). The font is kept compact + auto-shrinks so long values
    // (e.g. "8800.0 Hz") stay within the dial. (See the function-header note on
    // why the readout is retained centred rather than below the label.)
    if (slider.isEnabled())
    {
        const juce::String valueText = slider.getTextFromValue (slider.getValue());
        const float maxTextW = radius * 2.0f;
        // Y2K: the dial readout uses the LABEL face (Michroma) in WHITE — the
        // same family as the captions, so the value reads as part of the label
        // tier, not a separate data readout. Every other theme keeps the app
        // sans.
        auto vfFor = [this] (float h)
        { return isY2kChrome (theme_) ? labelFont (h, juce::Font::plain) : appFont (h, juce::Font::plain); };
        juce::Font vf = vfFor (juce::jmax (11.0f, radius * 0.52f));
        const int textW = juce::GlyphArrangement::getStringWidthInt (vf, valueText);
        if ((float) textW > maxTextW && textW > 0)
            // Auto-shrink for long values, floored at 11pt (T15, iPadOS audit:
            // was 9pt — below arm's-length touch readability). An over-long
            // value that cannot fit at 11pt simply draws past the dial edge
            // rather than becoming unreadably small.
            vf = vfFor (juce::jmax (11.0f, vf.getHeight() * maxTextW / (float) textW));

        // Y2K: the readout is WHITE (the label tier), not the LCD green the
        // data screens use elsewhere.
        const juce::Colour readoutCol = isY2kChrome (theme_)
            ? juce::Colour (0xffffffff)
            : valueCol;
        const auto textRect = bounds.toNearestInt().withSizeKeepingCentre (
            juce::roundToInt (maxTextW), juce::roundToInt (vf.getHeight() * 1.7f));
        drawTextUncurtained (g, valueText, vf, textRect.toFloat(), readoutCol,
                             juce::Justification::centred);
    }

    // --- Modulation ring (per-source concentric arcs) ---
    // A knob whose paramID maps to a MOD_DST draws one OUTER concentric arc PER
    // active matrix slot routed to it, each coloured by that source's functional
    // CATEGORY (Env=cyan, LFO=magenta, Seq=green, Arp=purple; Op/Const/etc =
    // neutral). The count + per-source colour/amount are pushed onto the slider's
    // getProperties() by ParamControl::refreshModRing():
    //   "parvatiModN"      = number of active arcs
    //   "parvatiModCol"+i  = ARGB of the i-th arc's category colour
    //   "parvatiModAmt"+i  = signed amount (-63..63) of the i-th arc
    // Each arc is ANCHORED AT THE KNOB'S CURRENT VALUE ANGLE (`toAngle`) — NOT
    // the centre — extending +/- by (amount/63)*halfRange, clamped to the dial.
    // When "parvatiModHi" is set (this knob is the hovered/selected target) the
    // arcs render thicker + brighter. N<=0 draws nothing (no faint zero ring).
    // --- Drop-zone affordance (active while a mod source is being dragged) ---
    // During a parvatiModSrc drag every VALID destination knob lights up as a
    // drop target: a prominent full-sweep ring just outside the value arc, in
    // the knob's fill/category colour brightened (~0.6 alpha, ~2px, rounded).
    // Drawn FIRST so the per-source modulation arcs sit on top. Knobs with NO
    // current modulation (parvatiModN==0) still light up. The flag is pushed by
    // ParamControl::applyModDragAffordance; non-targets are dimmed via the
    // cell's alpha (setAlpha) and never reach this branch.
    const bool dragTarget = [&]
    {
        const auto* v = slider.getProperties().getVarPointer ("parvatiModDrag");
        return v != nullptr && v->isBool() && (bool) *v;
    }();
    if (dragTarget)
    {
        const float cellHalf = juce::jmin ((float) width, (float) height) * 0.5f;
        const float ringR = juce::jlimit (4.0f, juce::jmax (4.0f, cellHalf - 1.0f),
                                           radius + 2.0f);
        g.setColour ((modLocked ? lockedGrey : fill).brighter (0.25f).withAlpha (0.6f));
        juce::Path zone;
        zone.addCentredArc (centre.x, centre.y, ringR, ringR, 0.0f,
                            rotaryStartAngle, rotaryEndAngle, true);
        g.strokePath (zone, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
    }
    const auto* hiVar = slider.getProperties().getVarPointer ("parvatiModHi");
    const bool  highlighted = (hiVar != nullptr && hiVar->isBool() && (bool) *hiVar);
    const auto* nVar = slider.getProperties().getVarPointer ("parvatiModN");
    const int N = (nVar != nullptr && nVar->isInt()) ? juce::jlimit (0, 6, (int) *nVar) : 0;
    if (N > 0)
    {
        // The first arc sits just outside the value arc; arcs after it step
        // outward, all clamped to the cell half-extent (minus a 1px margin) so
        // the concentric stack never clips. Tiny knobs with no room draw none.
        const float cellHalf = juce::jmin ((float) width, (float) height) * 0.5f;
        const float baseRing = radius + 2.0f;
        const float maxSweep = (rotaryEndAngle - rotaryStartAngle) * 0.5f;
        const float step     = 2.2f;
        for (int i = 0; i < N; ++i)
        {
            const float ringR = baseRing + (float) i * step;
            if (ringR > cellHalf - 1.0f)
                break;   // cap: no more arcs fit inside the cell

            const auto* colVar = slider.getProperties().getVarPointer ("parvatiModCol" + juce::String (i));
            // LOCKED (2026-08-23): every ring paints the locked grey (the
            // knob reads fully deactivated under the cannot-drop drag); the
            // per-mod colour is only used while unlocked.
            const juce::Colour col = modLocked ? lockedGrey
                : ((colVar != nullptr && colVar->isInt())
                       ? juce::Colour ((uint32_t) (int) *colVar)
                       : effFill);   // fallback to the knob fill if no colour was pushed

            const auto* amtVar = slider.getProperties().getVarPointer ("parvatiModAmt" + juce::String (i));
            const int amt = (amtVar != nullptr && amtVar->isInt())
                ? juce::jlimit (-63, 63, (int) *amtVar) : 0;

            // Signed sweep from the CURRENT value angle, clamped to the dial.
            const float sweep = ((float) amt / 63.0f) * maxSweep;   // signed; |sweep| <= maxSweep < pi
            const float a1 = juce::jlimit (rotaryStartAngle, rotaryEndAngle,
                                           juce::jmin (toAngle, toAngle + sweep));
            const float a2 = juce::jlimit (rotaryStartAngle, rotaryEndAngle,
                                           juce::jmax (toAngle, toAngle + sweep));
            if (a2 <= a1)
                continue;   // zero-length (e.g. amount==0) draws nothing

            // Subtle full-sweep context track at this arc's radius.
            juce::Path trackPath;
            trackPath.addCentredArc (centre.x, centre.y, ringR, ringR, 0.0f,
                                     rotaryStartAngle, rotaryEndAngle, true);
            g.setColour (col.withMultipliedAlpha (0.08f));
            g.strokePath (trackPath, juce::PathStrokeType (1.0f, juce::PathStrokeType::curved,
                                                           juce::PathStrokeType::rounded));

            // The arc itself, anchored at the current value angle: full alpha,
            // 1.6px (thickened + brightened while highlighted).
            const float arcW = highlighted ? 2.0f : 1.6f;
            juce::Path arcPath;
            arcPath.addCentredArc (centre.x, centre.y, ringR, ringR, 0.0f, a1, a2, true);
            g.setColour (highlighted ? col.brighter (0.30f) : col);
            g.strokePath (arcPath, juce::PathStrokeType (arcW, juce::PathStrokeType::curved,
                                                          juce::PathStrokeType::rounded));
        }
    }

    // "Cannot drop here" padlock: shown on a NON-destination knob while a
    // mod-source drag is hovered over it (ParamControl::setDropLocked).
    {
        if (modLocked)
        {
            // 0.34 -> 0.46 (2026-08-23 user request: "increase the size of the
            // lock icon when dragging over something") and the glyph colour is
            // a LIGHT GREY (not the text colour, not white): a clear
            // mid-light grey reads as the neutral "cannot modulate this"
            // state on every theme.
            const float lsz = juce::jmin ((float) width, (float) height) * 0.46f;
            drawPadlock (g, centre, lsz, juce::Colour::greyLevel (0.72f));
        }
    }
}

void ParvatiLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                           float sliderPos, float minSliderPos, float maxSliderPos,
                                           juce::Slider::SliderStyle style, juce::Slider& slider)
{
    // FLAT linear slider (the Settings zoom slider + the pitch/mod wheels — both
    // inherit this LookAndFeel). NOTE: the Mod-Matrix depth sliders use their
    // OWN BipolarSliderLNF (MatrixViewBase.cpp), so they are NOT routed here.
    //
    // Style: a dark rounded track, an accent FILL, and a flat solid circle
    // handle — no 3D bevel/shadow. Bipolar ranges (e.g. the -1..1 pitch wheel)
    // fill from the CENTRE; unipolar ranges fill from the start.
    if (theme_ == nullptr)
    {
        juce::LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos,
                                                minSliderPos, maxSliderPos, style, slider);
        return;
    }
    juce::ignoreUnused (minSliderPos, maxSliderPos);

    const bool vertical = (style == juce::Slider::LinearVertical);
    const bool enabled  = slider.isEnabled();

    const juce::Colour trackCol = theme_->trackEmpty;          // dark rounded track
    const juce::Colour fillCol  = slider.findColour (juce::Slider::thumbColourId);  // == accent
    const juce::Colour thumbCol = fillCol;

    const float sp = vertical
                       ? juce::jlimit ((float) y, (float) (y + height), sliderPos)
                       : juce::jlimit ((float) x, (float) (x + width), sliderPos);

    // Bipolar detection: if the value range straddles zero, fill from the
    // centre; otherwise fill from the nearer end (start).
    const double mn = slider.getMinimum();
    const double mx = slider.getMaximum();
    const double span = mx - mn;
    const bool bipolar = (span > 0.0) && (mn < 0.0 && mx > 0.0);

    constexpr float trackThickness = 4.0f;
    const float trackRadius = trackThickness * 0.5f;

    if (! vertical)
    {
        const float cy = (float) y + (float) height * 0.5f;
        const float left = (float) x;
        const float right = (float) (x + width);
        const float centreX = (left + right) * 0.5f;

        // Empty track.
        g.setColour (trackCol);
        g.fillRoundedRectangle (juce::Rectangle<float> (left, cy - trackRadius,
                                                        right - left, trackThickness), trackRadius);

        // Fill (accent): from centre (bipolar) or from the left (unipolar) to sp.
        if (enabled)
        {
            g.setColour (fillCol);
            const float fromX = bipolar ? centreX : left;
            const float a = juce::jmin (fromX, sp);
            const float b = juce::jmax (fromX, sp);
            if (b > a)
                g.fillRoundedRectangle (juce::Rectangle<float> (a, cy - trackRadius,
                                                                b - a, trackThickness), trackRadius);
        }

        // Flat solid circle handle (no 3D).
        const float tr = juce::jmax (4.5f, (float) height * 0.30f);
        const auto thumbRect = juce::Rectangle<float> (tr * 2.0f, tr * 2.0f)
                                   .withCentre (juce::Point<float> (sp, cy));
        g.setColour (enabled ? thumbCol : thumbCol.withMultipliedAlpha (0.45f));
        g.fillEllipse (thumbRect);
    }
    else
    {
        const float cx = (float) x + (float) width * 0.5f;
        const float top = (float) y;
        const float bottom = (float) (y + height);
        const float centreY = (top + bottom) * 0.5f;

        // Empty track.
        g.setColour (trackCol);
        g.fillRoundedRectangle (juce::Rectangle<float> (cx - trackRadius, top,
                                                        trackThickness, bottom - top), trackRadius);

        // Fill (accent): from centre (bipolar) or from the bottom (unipolar) to sp.
        if (enabled)
        {
            g.setColour (fillCol);
            const float fromY = bipolar ? centreY : bottom;
            const float a = juce::jmin (fromY, sp);
            const float b = juce::jmax (fromY, sp);
            if (b > a)
                g.fillRoundedRectangle (juce::Rectangle<float> (cx - trackRadius, a,
                                                                trackThickness, b - a), trackRadius);
        }

        // Flat solid circle handle (no 3D).
        const float tr = juce::jmax (4.5f, (float) width * 0.30f);
        const auto thumbRect = juce::Rectangle<float> (tr * 2.0f, tr * 2.0f)
                                   .withCentre (juce::Point<float> (cx, sp));
        g.setColour (enabled ? thumbCol : thumbCol.withMultipliedAlpha (0.45f));
        g.fillEllipse (thumbRect);
    }
}

void ParvatiLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height,
                                       bool isButtonDown,
                                       int buttonX, int buttonY,
                                       int buttonW, int buttonH,
                                       juce::ComboBox& box)
{
    // DARK DROPDOWN (flat, opaque, no bevel): a UNIFORM solid dark-gray fill
    // (the darkest chassis tone on dark themes) so crisp WHITE text reads fully
    // legible over any row tint, with a minimal ▼ chevron (light token,
    // right-aligned) and NO outline / inset shadow / arrow bevel. A mod-matrix
    // SOURCE combo also carries a 4px family-colour TAG strip on its
    // far-left edge (the "parvatiComboTag" property). Inline text is laid out
    // by positionComboBoxText(), whose ~24px right reserve matches the chevron
    // so long choices never clip.
    if (theme_ == nullptr)
    {
        juce::LookAndFeel_V4::drawComboBox (g, width, height, isButtonDown,
                                            buttonX, buttonY, buttonW, buttonH, box);
        return;
    }

    // SELF-HEAL the closed-combo readout font (Y2K, 2026-08-25): ComboBox
    // only re-resolves its inline text via positionComboBoxText() inside
    // resized(), and resized() bails when the combo is still zero-sized, so a
    // combo that was not yet laid out at theme-switch time keeps the default
    // face until it is resized later. Painting is the reliable trigger every
    // combo reaches, so here we re-point the child Label at the current
    // getComboBoxFont(). The guard makes it a one-time stabilise (no loop).
    if (isY2kChrome (theme_))
        for (auto* child : box.getChildren())
            if (auto* lab = dynamic_cast<juce::Label*> (child))
                if (lab->getFont().getTypefaceName() != getComboBoxFont (box).getTypefaceName())
                {
                    lab->setFont (getComboBoxFont (box));
                    break;
                }

    // DARK DROPDOWN (flat, opaque, no bevel): a UNIFORM solid dark-gray fill so
    // crisp WHITE text reads fully legible over any row tint. The fill is the
    // darkest chassis tone on the dark themes (backgroundBase); on a light
    // theme a neutral dark gray keeps the same dark-dropdown look (the
    // per-combo background colour is intentionally ignored — every combo is the
    // same dark field). A small tonal lift on hover / while open stays dark.
    const bool hover = box.isMouseOver() || isButtonDown;
    const juce::Colour baseFill = theme_->isDark ? theme_->backgroundBase
                                                 : juce::Colour (0xff2A2E35);
    const auto fill = hover ? baseFill.brighter (0.06f) : baseFill;
    constexpr float corner = 5.0f;

    // VERTICAL VISUAL INSET ("parvatiComboVisualH" property, an int): dense
    // rows keep a compact DRAWN dropdown (24-28pt) while the combo's BOUNDS —
    // its tap / hover area — span the 44pt HIG touch minimum. Everything drawn
    // below (fill, tag, chevron, padlock) is confined to the centred visual
    // strip, so the extra hit padding is fully transparent and the desktop
    // look is pixel-identical. positionComboBoxText() insets the inline text to
    // the same strip. Combos WITHOUT the property draw exactly as before (the
    // visual box fills the whole bounds).
    const auto* vhVar = box.getProperties().getVarPointer ("parvatiComboVisualH");
    const int visualH = (vhVar != nullptr && vhVar->isInt())
                            ? juce::jlimit (1, height, (int) *vhVar)
                            : height;
    const auto r = juce::Rectangle<int> (0, (height - visualH) / 2, width, visualH)
                       .toFloat()
                       .reduced (0.5f);

    g.setColour (fill);
    g.fillRoundedRectangle (r, corner);

    // FAMILY COLOUR TAG (mod-matrix SOURCE combo only): a bright 4px vertical
    // strip on the FAR-LEFT edge, clipped to the rounded rect so it follows the
    // corner radius. The strip colour is carried in the "parvatiComboTag"
    // property (an ARGB int set by MatrixRow for the source combo, in that
    // source's family colour). It tags the family WITHOUT colouring the dark
    // dropdown fill. Combos without the property (dest combo + every other
    // combo in the editor) get no tag.
    const auto* tagVar = box.getProperties().getVarPointer ("parvatiComboTag");
    if (tagVar != nullptr && tagVar->isInt())
    {
        const juce::Colour tagCol ((uint32_t) (int) *tagVar);
        juce::Path clip;
        clip.addRoundedRectangle (r, corner);
        g.saveState();
        g.reduceClipRegion (clip);
        g.setColour (tagCol);
        g.fillRect (juce::Rectangle<float> (r.getX(), r.getY(), 4.0f, r.getHeight()));
        g.restoreState();
    }

    // CRISP clear downward ▼ chevron, vertically centred, in a light token (so
    // it pops on the dark fill on every theme). Solid flat fill (no bevel).
    const auto chevronCol = theme_->isDark ? theme_->textPrimary : juce::Colour (0xfff6f6fa);
    constexpr float chevronSize = 5.0f;
    const float cx = (float) width  - 12.0f;
    const float cy = r.getCentreY();
    juce::Path chevron;
    chevron.startNewSubPath (cx - chevronSize, cy - chevronSize * 0.5f);
    chevron.lineTo (cx + chevronSize, cy - chevronSize * 0.5f);
    chevron.lineTo (cx, cy + chevronSize * 0.5f);
    chevron.closeSubPath();
    g.setColour (chevronCol);
    g.fillPath (chevron);

    // "Cannot drop here" padlock on a NON-destination combo while a mod-source
    // drag is hovered over it (ParamControl::setDropLocked).
    {
        const auto* lv = box.getProperties().getVarPointer ("parvatiModLocked");
        if (lv != nullptr && lv->isBool() && (bool) *lv)
            // Same 2026-08-23 treatment as the knob path: LIGHT GREY (theme-
            // agnostic, clearly not white) + a larger glyph.
            drawPadlock (g, r.getCentre(),
                         r.getHeight() * 0.8f,
                         juce::Colour::greyLevel (0.72f));
    }
}

void ParvatiLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    // Inline text: left-padded, stopping before the right-aligned accent chevron
    // (6px left pad + ~18px right reserve for the chevron = 24px chrome). Kept
    // tight so a fit-to-text dropdown reads as wide as its content, not padded.
    // When the combo carries the "parvatiComboVisualH" property (44pt tap band
    // with a compact visual box — see drawComboBox), the text is inset to the
    // SAME centred visual strip so it stays vertically centred in the drawn
    // dropdown, not in the taller tap band.
    const auto* vhVar = box.getProperties().getVarPointer ("parvatiComboVisualH");
    const int visualH = (vhVar != nullptr && vhVar->isInt())
                            ? juce::jlimit (1, box.getHeight(), (int) *vhVar)
                            : box.getHeight();
    const int y = (box.getHeight() - visualH) / 2;
    label.setBounds (6, y + 1, box.getWidth() - 24, visualH - 2);
    // The INLINE (closed) text uses the SAME data font as the open list
    // (getComboBoxFont): the app sans on every theme (the VT323 console
    // readout face is retired). lookAndFeelChanged re-calls this on a theme
    // switch, so the closed value re-resolves with the theme (juce::Label
    // caches fonts).
    label.setFont (getComboBoxFont (box));
}

void ParvatiLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                               const juce::Colour& backgroundColour,
                                               bool shouldDrawButtonAsHighlighted,
                                               bool shouldDrawButtonAsDown)
{
    // FLAT borderless tonal block — 4px rounded corners, a solid fill and NO
    // stroke / bevel / shadow. State routing reuses the existing colour-ID path:
    // backgroundColour already encodes on/off (TextButton passes buttonOnColourId
    // == theme accent when toggled on, buttonColourId == panelBackground
    // otherwise), so:
    //   - toggled-on: solid accent fill;
    //   - default/off: subtle panel fill;
    //   - hover (off): fill LIGHTER; press: fill DARKER.
    // IconButton (gear/undo/redo) paints itself and bypasses this method.
    if (theme_ == nullptr)
    {
        juce::LookAndFeel_V4::drawButtonBackground (g, button, backgroundColour,
                                                    shouldDrawButtonAsHighlighted,
                                                    shouldDrawButtonAsDown);
        return;
    }

    const float enabledAlpha = button.isEnabled() ? 1.0f : 0.5f;
    const auto r = button.getLocalBounds().toFloat().reduced (0.5f);
    constexpr float corner = 4.0f;

    const bool on   = button.getToggleState();
    const bool down = shouldDrawButtonAsDown;
    const bool over = shouldDrawButtonAsHighlighted && ! on;

    // Fill (on/off aware via backgroundColour): toggled-on keeps its solid
    // accent fill; off buttons are borderless tonal blocks — LIGHTER on hover,
    // DARKER on press. No bevel / shadow.
    auto fill = backgroundColour.withMultipliedAlpha (enabledAlpha);
    if (! on)
    {
        if (down)
            fill = fill.darker (0.12f);
        else if (over)
            fill = fill.brighter (0.08f);
    }
    g.setColour (fill);
    g.fillRoundedRectangle (r, corner);   // flat, borderless

    if (isY2kChrome (theme_))
    {
        // Y2K only: the Win98 raised button rim. A light top edge and a dark
        // bottom edge sit on the tonal fill. Press inverts the rim (the
        // sunken feel); toggled-on keeps the raised chrome pill look.
        const bool sunken = down && ! on;
        drawY2kBevel (g, r, corner,
                      (sunken ? fill.darker (0.45f) : fill.brighter (0.45f))
                          .withMultipliedAlpha (enabledAlpha),
                      (sunken ? fill.brighter (0.45f) : fill.darker (0.45f))
                          .withMultipliedAlpha (enabledAlpha),
                      1.5f);
    }

    // OUTLINED chrome (2026-08-23, Patch-page export buttons): a button
    // carrying the "parvatiButtonOutlined" component property also
    // gets a 1px rounded STROKE around the tonal fill. The default flat
    // block reads as floating text on large tonal panels — the stroke + the
    // caller's accent-tinted fill/text give a proper button affordance
    // without touching the global tonal style every other button uses.
    // The stroke follows the button's OWN text colour (off state), so the
    // whole chrome derives from the two colours the caller sets and re-skins
    // with the theme automatically; hover brightens it toward the text
    // colour for feedback. Off-state only — a toggled-on accent fill needs
    // no outline.
    if (! on && button.getProperties().getVarPointer ("parvatiButtonOutlined") != nullptr)
    {
        const auto stroke = button.findColour (juce::TextButton::textColourOffId, true)
                                .withMultipliedAlpha (enabledAlpha * (over ? 0.85f : 0.60f));
        g.setColour (stroke);
        g.drawRoundedRectangle (r, corner, 1.0f);
    }
}

void ParvatiLookAndFeel::drawHeadingText (juce::Graphics& g, const juce::String& text,
                                          const juce::Font& font, juce::Rectangle<float> area,
                                          juce::Colour colour)
{
    // Emphasised label (e.g. GroupComponent section headers): a bold weight in
    // the app sans-serif. (@p font carries the height; the family/style are
    // re-resolved through appFont so the heading always uses the UI family.)
    // Routed through drawTextUncurtained so the LAST glyph is never silently
    // dropped by Graphics::drawText's internal wordWrapWidth curtailment.
    //
    // Y2K: module headers render in MICHROMA with wide letter-spacing (the
    // era's techno-panel look). JUCE has no tracking API, so the glyphs are
    // placed one by one with the tracking gap between them (the same
    // technique as the editor wordmark's trackedTextWidth).
    if (isY2kChrome (theme_))
    {
        constexpr float kHeaderTracking = 2.0f;   // px between glyphs (Michroma is already wide)
        const juce::Font hf = headerFont (juce::jmin (font.getHeight(), 13.0f));
        juce::GlyphArrangement ga;
        float x = area.getX();
        const float y = area.getY();
        for (int i = 0; i < text.length(); ++i)
        {
            ga.addLineOfText (hf, text.substring (i, i + 1), x, y);
            juce::GlyphArrangement measure;
            measure.addLineOfText (hf, text.substring (i, i + 1), 0.0f, 0.0f);
            x += measure.getBoundingBox (0, measure.getNumGlyphs(), true).getWidth() + kHeaderTracking;
        }
        // Vertically centre the drawn glyphs in the area (addLineOfText uses
        // the baseline; shift by the measured bounds delta).
        const auto bb = ga.getBoundingBox (0, ga.getNumGlyphs(), true);
        const float dy = area.getY() - bb.getY()
                         + (area.getHeight() - bb.getHeight()) * 0.5f;
        g.saveState();
        g.addTransform (juce::AffineTransform::translation (0.0f, dy));
        g.setColour (colour);
        ga.draw (g);
        g.restoreState();
        return;
    }
    drawTextUncurtained (g, text, appFont (font.getHeight(), juce::Font::bold),
                         area, colour, juce::Justification::centredLeft);
}

void ParvatiLookAndFeel::drawTextUncurtained (juce::Graphics& g, const juce::String& text,
                                               const juce::Font& font, juce::Rectangle<float> area,
                                               juce::Colour colour, juce::Justification justification)
{
    // Builds a GlyphArrangement via addLineOfText (unbounded wordWrap — no
    // curtailment, unlike Graphics::drawText's internal wordWrapWidth), then
    // translates the arrangement so its bounding box is positioned within @p
    // area per the justification flags. Underline from @p font is rendered by
    // GlyphArrangement::draw.
    juce::GlyphArrangement ga;
    ga.addLineOfText (font, text, 0.0f, 0.0f);

    const auto bb = ga.getBoundingBox (0, ga.getNumGlyphs(), true);

    float dx = area.getX() - bb.getX();
    float dy = area.getY() - bb.getY();

    const auto flags = justification.getFlags();

    if ((flags & juce::Justification::right) != 0)
        dx += area.getWidth() - bb.getWidth();
    else if ((flags & juce::Justification::horizontallyCentred) != 0)
        dx += (area.getWidth() - bb.getWidth()) * 0.5f;

    if ((flags & juce::Justification::bottom) != 0)
        dy += area.getHeight() - bb.getHeight();
    else if ((flags & juce::Justification::verticallyCentred) != 0)
        dy += (area.getHeight() - bb.getHeight()) * 0.5f;

    ga.moveRangeOfGlyphs (0, ga.getNumGlyphs(), dx, dy);

    g.setColour (colour);
    ga.draw (g);
}

//==============================================================================
// ParvatiModuleLamp — the unified module enable/disable indicator (see the
// header comment). ONE painting path for the synth mod matrix rows, the FX
// mod matrix rows and the FX slot cards' power toggles: accentPrimary fill
// when ON, textDisabled grey when OFF, outline ring brightening on hover.
void ParvatiModuleLamp::paintButton (juce::Graphics& g, bool isMouseOverButton, bool isButtonDown)
{
    const ParvatiTheme* t = parvati::themeFor (*this);

    const juce::Colour accent = t ? t->accentPrimary : parvati::parvatiFallbackAccent;
    const juce::Colour text   = t ? t->textPrimary   : parvati::kFallbackTextPrimary;
    const juce::Colour grey   = t ? t->textDisabled  : parvati::kFallbackTextDisabled;
    const juce::Colour ring   = t ? t->outline       : text.withAlpha (0.45f);

    const bool on = getToggleState() || isButtonDown;

    // Fill: the row's overridden colour while ON (the mod-matrix rows tag the
    // lamp with their modulator's category colour), else the theme accent;
    // the theme's inactive grey while bypassed/muted (visible on every theme).
    juce::Colour fill = on ? (onColour_.isTransparent() ? accent : onColour_) : grey;
    if (! isEnabled())
        fill = fill.withAlpha (0.25f);

    // Border ring: slightly brightened outline so the dot's contour stays
    // legible on every theme (user 2026-08-20: "a tiny bit" more border),
    // brightened further on hover so the dot reads as tappable (the only
    // hover affordance).
    juce::Colour border = ring.brighter (0.25f);
    if (isMouseOverButton)
        border = on ? ring.brighter (0.8f) : text.brighter (0.20f);
    if (! isEnabled())
        border = ring.withAlpha (0.30f);

    // ---- Dot: scales with the band (44pt matrix bands render a ~28-30pt
    // dot; the FX card's tighter header band clamps proportionally). The HIT
    // area stays the full bounds. An optional pinned centre keeps the card
    // header's lamp aligned with the painted title's optical middle. ----
    const auto b = getLocalBounds().toFloat();
    const float dot = lampDiameter_ > 0.0f ? lampDiameter_
                                           : dotDiameterFor (getLocalBounds());
    const auto centre = lampCentre_.x >= 0.0f
        ? juce::Point<float> (juce::jlimit (dot * 0.5f, juce::jmax (dot * 0.5f, b.getWidth() - dot * 0.5f), lampCentre_.x),
                              juce::jlimit (dot * 0.5f, juce::jmax (dot * 0.5f, b.getHeight() - dot * 0.5f), lampCentre_.y))
        : b.getCentre();
    const auto r = juce::Rectangle<float> (centre.x - dot * 0.5f, centre.y - dot * 0.5f, dot, dot);

    g.setColour (fill);
    g.fillEllipse (r);
    g.setColour (border);
    g.drawEllipse (r, kLampBorderWidth);
}

float ParvatiModuleLamp::dotDiameterFor (juce::Rectangle<int> bounds)
{
    // 0.68 of the band's short side, clamped 8..30pt: the matrix rows' 44pt
    // bands give ~28-30pt dots ("a bit bigger", user feedback), and tighter
    // bands (the FX card header) shrink proportionally without ever
    // vanishing. Pure function of the bounds — identical bands render
    // identical dots everywhere (the style-parity test pins this).
    const float s = static_cast<float> (juce::jmin (bounds.getWidth(), bounds.getHeight()));
    return juce::jlimit (8.0f, 30.0f, s * 0.68f);
}

juce::Colour ParvatiModuleLamp::resolvedOnColourForTest() const
{
    const ParvatiTheme* t = parvati::themeFor (*this);
    const juce::Colour accent = t ? t->accentPrimary : parvati::parvatiFallbackAccent;
    return onColour_.isTransparent() ? accent : onColour_;
}

// Copyright (c) 2026 Jozsef Ottucsak / Parvati.

#include "ParvatiLookAndFeel.h"

#include "../../fonts/unifont_data.h"   // embedded GNU Unifont subset (console font mode)

ParvatiLookAndFeel::ParvatiLookAndFeel()
{
    // Default to Carbon so theme_ is never null and every colour ID is set
    // before any component reads it. ParvatiEditor overrides this immediately
    // via setTheme(themeManager_.getCurrentTheme()).
    setTheme (carbonTheme());

    // Load the embedded GNU Unifont subset (ASCII + Latin-1 + symbols) once, for
    // the "Console" font mode (DOS/retro look).
    unifontTypeface_ = juce::Typeface::createSystemTypefaceFor (unifont_ttf, unifont_ttf_len);
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
    setColour (juce::Slider::textBoxOutlineColourId,           juce::Colour (0x00000000)); // borderless text box
    setColour (juce::Slider::textBoxHighlightColourId,         t.accent2);

    // ---- ComboBox ----
    setColour (juce::ComboBox::backgroundColourId,             t.panelBackground);
    setColour (juce::ComboBox::outlineColourId,                juce::Colour (0x00000000)); // borderless combo
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
    // Control-name labels (knob/combo captions, section captions) render in the
    // accent colour, not dim grey. Per-component overrides (e.g. the version
    // label) can still set a specific colour.
    setColour (juce::Label::textColourId,                      t.accent);
    setColour (juce::Label::backgroundColourId,                juce::Colour (0x00000000)); // transparent (preserve default)
    setColour (juce::Label::outlineColourId,                   juce::Colour (0x00000000)); // borderless

    // ---- ScrollBar (page Viewports) ----
    setColour (juce::ScrollBar::backgroundColourId,            t.windowBackground);
    setColour (juce::ScrollBar::thumbColourId,                 t.textDim);   // brighter than outline so the thumb reads
    setColour (juce::ScrollBar::trackColourId,                 t.panelBackground2);

    // ---- TextButton ----
    setColour (juce::TextButton::buttonColourId,               t.panelBackground);
    setColour (juce::TextButton::buttonOnColourId,             t.accent);
    setColour (juce::TextButton::textColourOffId,              t.text);
    setColour (juce::TextButton::textColourOnId,               t.windowBackground);

    // ---- TabbedComponent / TabbedButtonBar ----
    setColour (juce::TabbedComponent::backgroundColourId,      t.windowBackground);
    setColour (juce::TabbedComponent::outlineColourId,         juce::Colour (0x00000000)); // borderless
    setColour (juce::TabbedButtonBar::tabTextColourId,         t.textDim);
    setColour (juce::TabbedButtonBar::frontTextColourId,       t.accent);
    setColour (juce::TabbedButtonBar::tabOutlineColourId,      t.outline);
    setColour (juce::TabbedButtonBar::frontOutlineColourId,    t.accent);

    // ---- GroupComponent (panel headings only — no border box) ----
    setColour (juce::GroupComponent::textColourId,             t.accent);
    setColour (juce::GroupComponent::outlineColourId,          juce::Colour (0x00000000)); // no grey border around panels

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

void ParvatiLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar& scrollbar,
                                        int x, int y, int width, int height,
                                        bool isVertical,
                                        int thumbStartPosition, int thumbSize,
                                        bool isMouseOver, bool isMouseDown)
{
    juce::ignoreUnused (isMouseDown);
    // Faint track behind the thumb.
    g.setColour (scrollbar.findColour (juce::ScrollBar::trackColourId));
    g.fillRect (x, y, width, height);

    if (thumbSize <= 0)
        return;

    // A wide, rounded thumb centred in the bar — far easier to grab than the
    // V4 default thin thumb. Brightens toward the accent on hover.
    auto thumb = scrollbar.findColour (juce::ScrollBar::thumbColourId);
    if (isMouseOver)
        thumb = thumb.overlaidWith (scrollbar.findColour (juce::Slider::thumbColourId)
                                        .withAlpha (0.5f));

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
    switch (fontMode_)
    {
        case fontSerif:   // system default serif
            return juce::Font (juce::FontOptions (juce::Font::getDefaultSerifFontName(),
                                                  height, styleFlags));
        case fontSansSerif:   // system default sans-serif
            return juce::Font (juce::FontOptions (juce::Font::getDefaultSansSerifFontName(),
                                                  height, styleFlags));
        case fontConsole:
        default:
            // Console (default): embedded GNU Unifont (DOS/retro). Fall back to
            // the system monospace family if the embedded typeface failed to load.
            if (unifontTypeface_ != nullptr)
                return juce::Font (juce::FontOptions (unifontTypeface_)
                                       .withHeight (height).withStyleFlags (styleFlags));
            return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                  height, styleFlags));
    }
}

juce::Font ParvatiLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
    return appFont (14.0f, juce::Font::plain);
}

juce::Font ParvatiLookAndFeel::getTextButtonFont (juce::TextButton&, int)
{
    return appFont (14.0f, juce::Font::plain);
}

juce::Font ParvatiLookAndFeel::getPopupMenuFont()
{
    // The drop-down list of every ComboBox (and the Save format menu). Without
    // this override, PopupMenu would always render in the default sans, so the
    // family would NOT follow a font-mode switch.
    return appFont (15.0f, juce::Font::plain);
}

juce::Font ParvatiLookAndFeel::getLabelFont (juce::Label& label)
{
    // Preserve each label's own height/style and only swap the family, so the
    // mode follows live even for labels the editor does not re-apply manually.
    const auto f = label.getFont();
    return appFont (f.getHeight(), f.getStyleFlags());
}

juce::Font ParvatiLookAndFeel::getTabButtonFont (juce::TabBarButton&, float height)
{
    // Same sizing as the V4 default (height * 0.6); only the family follows the
    // mode, so tab widths / bar depth are unchanged.
    return appFont (height * 0.6f, juce::Font::plain);
}

int ParvatiLookAndFeel::getTabButtonBestWidth (juce::TabBarButton& button, int tabDepth)
{
    // Measure the label with the SAME family that drawTabButton renders it in
    // (appFont), otherwise a wide font like Unifont would render wider than the
    // measured slot and clip. Matches LookAndFeel_V2 otherwise.
    const juce::Font font = getTabButtonFont (button, (float) tabDepth);
    int width = juce::GlyphArrangement::getStringWidthInt (font, button.getButtonText().trim())
              + getTabButtonOverlap (tabDepth) * 2;

    if (auto* extraComponent = button.getExtraComponent())
        width += button.getTabbedButtonBar().isVertical() ? extraComponent->getHeight()
                                                          : extraComponent->getWidth();

    return juce::jlimit (tabDepth * 2, tabDepth * 8, width);
}

void ParvatiLookAndFeel::drawTabButton (juce::TabBarButton& button, juce::Graphics& g,
                                        bool isMouseOver, bool isMouseDown)
{
    // Faithful copy of LookAndFeel_V3::drawTabButton (V4 inherits it), with ONE
    // change: the label text layout is built through appFont() instead of V3's
    // hardcoded default-sans createTabTextLayout(), so the tab label family
    // follows the active font mode.
    const juce::Rectangle<int> activeArea (button.getActiveArea());
    const juce::TabbedButtonBar::Orientation o = button.getTabbedButtonBar().getOrientation();
    const juce::Colour bkg (button.getTabBackgroundColour());

    if (button.getToggleState())
    {
        g.setColour (bkg);
    }
    else
    {
        juce::Point<int> p1, p2;
        switch (o)
        {
            case juce::TabbedButtonBar::TabsAtBottom:   p1 = activeArea.getBottomLeft(); p2 = activeArea.getTopLeft();    break;
            case juce::TabbedButtonBar::TabsAtTop:      p1 = activeArea.getTopLeft();    p2 = activeArea.getBottomLeft(); break;
            case juce::TabbedButtonBar::TabsAtRight:    p1 = activeArea.getTopRight();   p2 = activeArea.getTopLeft();    break;
            case juce::TabbedButtonBar::TabsAtLeft:     p1 = activeArea.getTopLeft();    p2 = activeArea.getTopRight();   break;
            default:                                    jassertfalse; break;
        }
        g.setGradientFill (juce::ColourGradient (bkg.brighter (0.2f), p1.toFloat(),
                                                 bkg.darker (0.1f),   p2.toFloat(), false));
    }
    g.fillRect (activeArea);

    g.setColour (button.findColour (juce::TabbedButtonBar::tabOutlineColourId));
    juce::Rectangle<int> r (activeArea);
    if (o != juce::TabbedButtonBar::TabsAtBottom)   g.fillRect (r.removeFromTop (1));
    if (o != juce::TabbedButtonBar::TabsAtTop)      g.fillRect (r.removeFromBottom (1));
    if (o != juce::TabbedButtonBar::TabsAtRight)    g.fillRect (r.removeFromLeft (1));
    if (o != juce::TabbedButtonBar::TabsAtLeft)     g.fillRect (r.removeFromRight (1));

    const float alpha = button.isEnabled() ? ((isMouseOver || isMouseDown) ? 1.0f : 0.8f) : 0.3f;
    juce::Colour col (bkg.contrasting().withMultipliedAlpha (alpha));

    if (auto* bar = button.findParentComponentOfClass<juce::TabbedButtonBar>())
    {
        const juce::TabbedButtonBar::ColourIds colID = button.isFrontTab() ? juce::TabbedButtonBar::frontTextColourId
                                                                            : juce::TabbedButtonBar::tabTextColourId;
        if (bar->isColourSpecified (colID))
            col = bar->findColour (colID);
        else if (isColourSpecified (colID))
            col = findColour (colID);
    }

    const juce::Rectangle<float> area (button.getTextArea().toFloat());
    float length = area.getWidth();
    float depth  = area.getHeight();
    if (button.getTabbedButtonBar().isVertical())
        std::swap (length, depth);

    // *** the only deviation from V3: route the label through appFont(). ***
    juce::Font font (getTabButtonFont (button, depth));
    font.setUnderline (button.hasKeyboardFocus (false));
    juce::AttributedString s;
    s.setJustification (juce::Justification::centred);
    s.append (button.getButtonText().trim(), font, col);
    juce::TextLayout textLayout;
    textLayout.createLayout (s, length);

    juce::AffineTransform t;
    switch (o)
    {
        case juce::TabbedButtonBar::TabsAtLeft:   t = t.rotated (juce::MathConstants<float>::pi * -0.5f).translated (area.getX(), area.getBottom()); break;
        case juce::TabbedButtonBar::TabsAtRight:  t = t.rotated (juce::MathConstants<float>::pi *  0.5f).translated (area.getRight(), area.getY()); break;
        case juce::TabbedButtonBar::TabsAtTop:
        case juce::TabbedButtonBar::TabsAtBottom: t = t.translated (area.getX(), area.getY()); break;
        default:                                  jassertfalse; break;
    }
    g.addTransform (t);
    textLayout.draw (g, juce::Rectangle<float> (length, depth));
}

void ParvatiLookAndFeel::drawGroupComponentOutline (juce::Graphics& g, int width, int height,
                                                     const juce::String& text,
                                                     const juce::Justification& position,
                                                     juce::GroupComponent& group)
{
    // Mirrors LookAndFeel_V2::drawGroupComponentOutline, but the title font is
    // resolved through appFont() so panel headings ("Osc 1", "Mixer", ...) follow
    // the active font mode. The outline colour is transparent (borderless), so
    // only the title text is visible.
    const float textH = 15.0f;
    const float indent = 3.0f;
    const float textEdgeGap = 4.0f;
    auto cs = 5.0f;

    const juce::Font f = appFont (textH, juce::Font::plain);

    juce::Path p;
    auto x = indent;
    auto y = f.getAscent() - 3.0f;
    auto w = juce::jmax (0.0f, (float) width - x * 2.0f);
    auto h = juce::jmax (0.0f, (float) height - y - indent);
    cs = juce::jmin (cs, w * 0.5f, h * 0.5f);
    auto cs2 = 2.0f * cs;

    auto textW = text.isEmpty() ? 0.0f
                                : juce::jlimit (0.0f,
                                                juce::jmax (0.0f, w - cs2 - textEdgeGap * 2),
                                                (float) juce::GlyphArrangement::getStringWidthInt (f, text)
                                                    + textEdgeGap * 2.0f);
    auto textX = cs + textEdgeGap;

    if (position.testFlags (juce::Justification::horizontallyCentred))
        textX = cs + (w - cs2 - textW) * 0.5f;
    else if (position.testFlags (juce::Justification::right))
        textX = w - cs - textW - textEdgeGap;

    p.startNewSubPath (x + textX + textW, y);
    p.lineTo (x + w - cs, y);

    p.addArc (x + w - cs2, y, cs2, cs2, 0, juce::MathConstants<float>::halfPi);
    p.lineTo (x + w, y + h - cs);

    p.addArc (x + w - cs2, y + h - cs2, cs2, cs2,
              juce::MathConstants<float>::halfPi, juce::MathConstants<float>::pi);
    p.lineTo (x + cs, y + h);

    p.addArc (x, y + h - cs2, cs2, cs2,
              juce::MathConstants<float>::pi, juce::MathConstants<float>::pi * 1.5f);
    p.lineTo (x, y + cs);

    p.addArc (x, y, cs2, cs2,
              juce::MathConstants<float>::pi * 1.5f, juce::MathConstants<float>::twoPi);
    p.lineTo (x + textX, y);

    const auto alpha = group.isEnabled() ? 1.0f : 0.5f;

    g.setColour (group.findColour (juce::GroupComponent::outlineColourId)
                    .withMultipliedAlpha (alpha));
    g.strokePath (p, juce::PathStrokeType (2.0f));

    g.setColour (group.findColour (juce::GroupComponent::textColourId)
                    .withMultipliedAlpha (alpha));
    g.setFont (f);
    g.drawText (text,
                juce::roundToInt (x + textX), 0,
                juce::roundToInt (textW),
                juce::roundToInt (textH),
                juce::Justification::centred, true);
}

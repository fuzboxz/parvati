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

    // ---- ComboBox (dark container, 1px outline, amber chevron) ----
    setColour (juce::ComboBox::backgroundColourId,             t.panelBackground2);
    setColour (juce::ComboBox::outlineColourId,                t.outline);   // 1px container border (visible)
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
    setColour (juce::ScrollBar::thumbColourId,                 t.accent);    // accent-coloured thumb
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

    // ---- GroupComponent (1px bordered panel cards; title embedded in the top
    // border by drawGroupComponentOutline) ----
    setColour (juce::GroupComponent::textColourId,             t.accent);
    setColour (juce::GroupComponent::outlineColourId,          t.outline);   // 1px panel border (visible)

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
    // Faint track behind the thumb.
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
    // Smaller than the V4 default (height * 0.6) so the short all-caps labels fit
    // the tab width comfortably; only the family follows the active font mode.
    return appFont (height * 0.33f, juce::Font::plain);
}

int ParvatiLookAndFeel::getTabButtonBestWidth (juce::TabBarButton& button, int tabDepth)
{
    // Measure the SAME all-caps label that drawTabButton renders, otherwise a
    // wide font like Unifont would render wider than the measured slot and clip.
    // Matches LookAndFeel_V2 otherwise.
    const juce::Font font = getTabButtonFont (button, (float) tabDepth);
    int width = juce::GlyphArrangement::getStringWidthInt (font, button.getButtonText().trim().toUpperCase())
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

    // *** the deviation from V3: render the label in ALL CAPS through appFont(),
    // so the family follows the active font mode and the labels read cleanly. ***
    juce::Font font (getTabButtonFont (button, depth));
    font.setUnderline (button.hasKeyboardFocus (false));
    juce::AttributedString s;
    s.setJustification (juce::Justification::centred);
    s.append (button.getButtonText().trim().toUpperCase(), font, col);
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
    // A 1px RECTANGULAR (sharp-cornered) panel border whose TOP-LEFT edge is
    // broken by the section title text — the classic fieldset/legend look:
    //   ┌── [ OSC 1 ] ─────────────────┐
    //   │                              │
    //   │  controls                    │
    //   └──────────────────────────────┘
    // The title sits IN the top border line (anchored top-left) so it labels
    // the panel without consuming a vertical caption band. Title font follows
    // the active font mode and renders in ALL CAPS.
    const float textH = 13.0f;
    const float textPad = 6.0f;   // gap either side of the title inside the border break
    // The top border line runs at yTop so the title text straddles it (the
    // brackets sit ON the line, like ┌── [ OSC 1 ] ──┐).
    const float yTop = (float) juce::roundToInt (textH * 0.5f);
    juce::ignoreUnused (position);   // panels are always anchored top-left

    const juce::Font f = appFont (textH, juce::Font::plain);
    const juce::String displayText = text.toUpperCase();   // panel headings render in ALL CAPS
    const auto alpha = group.isEnabled() ? 1.0f : 0.5f;

    const int textW = displayText.isEmpty()
                        ? 0
                        : juce::GlyphArrangement::getStringWidthInt (f, displayText);

    // The title break occupies [breakX0 .. breakX1) on the top edge. Anchored
    // top-left (matches the GroupComponent's top|left justification).
    const float breakX0 = textPad;
    const float breakX1 = breakX0 + (float) textW + textPad;
    const float x0 = 0.5f;            // pixel-snapped inset so the 1px line is crisp
    const float x1 = (float) width  - 0.5f;
    const float y1 = (float) height - 0.5f;

    const juce::Colour outlineCol = group.findColour (juce::GroupComponent::outlineColourId)
                                        .withMultipliedAlpha (alpha);
    g.setColour (outlineCol);

    // Top edge as two segments with the title gap in between.
    g.drawHorizontalLine (juce::roundToInt (yTop), x0, breakX0);
    g.drawHorizontalLine (juce::roundToInt (yTop), breakX1, x1);
    // The remaining three edges (sides run from the top line down).
    g.drawVerticalLine   (juce::roundToInt (x1 - 0.5f), yTop, y1);
    g.drawHorizontalLine (juce::roundToInt (y1 - 0.5f), x0, x1);
    g.drawVerticalLine   (juce::roundToInt (x0), yTop, y1);

    // Title text drawn centred ON the top border line (in the break).
    g.setColour (group.findColour (juce::GroupComponent::textColourId)
                    .withMultipliedAlpha (alpha));
    g.setFont (f);
    g.drawText (displayText,
                juce::roundToInt (breakX0),
                juce::roundToInt (yTop - textH * 0.5f),
                textW,
                juce::roundToInt (textH),
                juce::Justification::centredLeft, true);
}

void ParvatiLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y,
                                           int width, int height,
                                           float sliderPos,
                                           float rotaryStartAngle,
                                           float rotaryEndAngle,
                                           juce::Slider& slider)
{
    // A thin amber arc-ring knob (no pointer line): a dim empty track arc plus
    // a bright fill arc from the start angle to the value angle. The numeric
    // value is drawn in the CENTRE of the ring (centre readout), eliminating
    // the dedicated value box underneath. Disabled knobs (sequencer steps past
    // the active length) draw only the dim track arc.
    const auto fill    = slider.findColour (juce::Slider::rotarySliderFillColourId);      // == accent (amber)
    const auto valueCol = slider.findColour (juce::Slider::textBoxTextColourId);
    const auto trackCol = fill.withAlpha (0.22f);   // dim amber empty track (1px amber ring)

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

    // The empty track is a literal 1px amber ring (per spec); the fill arc is a
    // touch thicker so the active sweep stays legible against it.
    const float trackW = 1.0f;
    const float fillW  = juce::jmax (1.5f, radius * 0.12f);

    // Empty (background) track arc — full sweep, dim amber.
    g.setColour (slider.isEnabled() ? trackCol : trackCol.withMultipliedAlpha (0.5f));
    juce::Path trackArc;
    trackArc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f,
                            rotaryStartAngle, rotaryEndAngle, true);
    g.strokePath (trackArc, juce::PathStrokeType (trackW, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));

    // Fill arc — start angle -> value angle, bright amber. Skipped for disabled
    // knobs so they read as inactive.
    if (slider.isEnabled() && rotaryEndAngle > rotaryStartAngle)
    {
        g.setColour (fill);
        juce::Path fillArc;
        fillArc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f,
                               rotaryStartAngle, toAngle, true);
        g.strokePath (fillArc, juce::PathStrokeType (fillW, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));

        // Subtle value-position tick at the end of the fill arc.
        const float tickR = radius - fillW;
        const juce::Point<float> tick (centre.x + tickR * std::cos (toAngle - juce::MathConstants<float>::halfPi),
                                       centre.y + tickR * std::sin (toAngle - juce::MathConstants<float>::halfPi));
        g.setColour (fill);
        g.fillEllipse (juce::Rectangle<float> (fillW * 1.4f, fillW * 1.4f).withCentre (tick));
    }

    // Centre numeric readout for ACTIVE knobs only (disabled steps show just the
    // dim track, per spec). The font auto-shrinks and the text is clipped to the
    // inner ring so long values (e.g. "8800.0 Hz") never spill past the arc.
    // Base size is +2px over the geometric default for legibility, with the
    // auto-shrink floor raised by the same 2px.
    if (slider.isEnabled())
    {
        const juce::String valueText = slider.getTextFromValue (slider.getValue());
        const float maxTextW = radius * 1.5f;
        juce::Font vf = appFont (juce::jmax (7.0f, radius * 0.42f) + 2.0f, juce::Font::plain);
        const int textW = juce::GlyphArrangement::getStringWidthInt (vf, valueText);
        if ((float) textW > maxTextW && textW > 0)
            vf = appFont (juce::jmax (8.0f, vf.getHeight() * maxTextW / (float) textW), juce::Font::plain);

        const auto textRect = bounds.toNearestInt().withSizeKeepingCentre (
            juce::roundToInt (maxTextW), juce::roundToInt (vf.getHeight() * 1.7f));
        g.setColour (valueCol);
        g.setFont (vf);
        g.drawText (valueText, textRect, juce::Justification::centred, false);
    }
}

void ParvatiLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height,
                                       bool /*isButtonDown*/,
                                       int /*buttonX*/, int /*buttonY*/,
                                       int /*buttonW*/, int /*buttonH*/,
                                       juce::ComboBox& box)
{
    // Flat dark container with a 1px outline and an amber chevron (▾)
    // right-aligned. Inline text is laid out by positionComboBoxText().
    const auto bg = box.findColour (juce::ComboBox::backgroundColourId);
    const auto outline = box.findColour (juce::ComboBox::outlineColourId);
    const auto arrow = box.findColour (juce::ComboBox::arrowColourId);   // == accent (amber)

    const auto r = juce::Rectangle<int> (0, 0, width, height).toFloat().reduced (0.5f);
    g.setColour (bg);
    g.fillRect (r);
    g.setColour (outline);
    g.drawRect (r, 1.0f);

    // Right-aligned amber chevron (a small downward triangle), vertically centred.
    constexpr float chevronSize = 5.0f;
    const float cx = (float) width  - 10.0f;
    const float cy = (float) height * 0.5f;
    juce::Path chevron;
    chevron.startNewSubPath (cx - chevronSize, cy - chevronSize * 0.5f);
    chevron.lineTo (cx + chevronSize, cy - chevronSize * 0.5f);
    chevron.lineTo (cx, cy + chevronSize * 0.5f);
    chevron.closeSubPath();
    g.setColour (arrow);
    g.fillPath (chevron);
}

void ParvatiLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    // Inline text: left-padded, stopping before the right-aligned amber chevron
    // (8px left pad + ~18px right reserve for the chevron = 26px chrome).
    label.setBounds (8, 1, box.getWidth() - 26, box.getHeight() - 2);
    label.setFont (appFont (14.0f, juce::Font::plain));
}

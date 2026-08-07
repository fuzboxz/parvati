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
    setColour (juce::Slider::textBoxTextColourId,              t.textValue);  // bright knob centre readout (the knob's primary readout)
    setColour (juce::Slider::textBoxBackgroundColourId,        t.panelBackground);
    setColour (juce::Slider::textBoxOutlineColourId,           juce::Colour (0x00000000)); // borderless text box
    setColour (juce::Slider::textBoxHighlightColourId,         t.accent2);

    // ---- ComboBox (dark container, 1px outline, amber chevron) ----
    setColour (juce::ComboBox::backgroundColourId,             t.panelBackground2);
    setColour (juce::ComboBox::outlineColourId,                juce::Colour (0x00000000));   // borderless (drawComboBox draws no outline)
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
    // Control-name labels (knob/combo captions) render in the low-contrast
    // labelText tier (regular-weight gray), NOT the accent. Per-component
    // overrides (e.g. the version/status labels, section headings) still set a
    // specific colour.
    setColour (juce::Label::textColourId,                      t.labelText);
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
    setColour (juce::TabbedComponent::outlineColourId,         juce::Colour (0x00000000));   // no card outline (flat / borderless)
    setColour (juce::TabbedButtonBar::tabTextColourId,         t.textDim);
    setColour (juce::TabbedButtonBar::frontTextColourId,       t.accent);
    setColour (juce::TabbedButtonBar::tabOutlineColourId,      t.outline);
    setColour (juce::TabbedButtonBar::frontOutlineColourId,    t.accent);

    // ---- GroupComponent (borderless solid rounded-rect panel CARDS; the title
    // is a bold muted-gray header drawn by drawGroupComponentOutline) ----
    setColour (juce::GroupComponent::textColourId,             t.panelHeader);
    setColour (juce::GroupComponent::outlineColourId,          juce::Colour (0x00000000));   // no outline (borderless cards)

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
    return appFont (14.0f, juce::Font::plain);
}

juce::Font ParvatiLookAndFeel::getTextButtonFont (juce::TextButton&, int)
{
    return appFont (14.0f, juce::Font::plain);
}

juce::Font ParvatiLookAndFeel::getPopupMenuFont()
{
    // The drop-down list of every ComboBox (and the Save format menu). Without
    // this override, PopupMenu would always render in the JUCE default font;
    // this routes it through the app sans-serif.
    return appFont (15.0f, juce::Font::plain);
}

juce::Font ParvatiLookAndFeel::getLabelFont (juce::Label& label)
{
    // Preserve each label's own height/style and only swap the family to the
    // app sans-serif (juce::Label caches its font, so getLabelFont re-resolves
    // the family whenever the label is laid out).
    const auto f = label.getFont();
    return appFont (f.getHeight(), f.getStyleFlags());
}

juce::Font ParvatiLookAndFeel::getTabButtonFont (juce::TabBarButton&, float height)
{
    // +40% over the legacy factor (0.33) so the redesigned button tabs are
    // prominent and easy to click; the family is the app sans-serif.
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
    //     accent for a clear on-brand gold segment) + bright text + the prominent
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
    // (ENV=cyan, LFO=magenta, ARP=purple, SEQ=green, MOD*=amber).
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
        else                                 catColour = theme_->tabUnderline;
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

    if (front)
    {
        g.setColour (theme_->tabSelectedBg);
        g.fillPath (tabShape);
        g.setColour (catColour.withMultipliedAlpha (0.22f));
        g.fillPath (tabShape);
    }
    else
    {
        g.setColour (hover ? theme_->panelBackground.brighter (0.15f)
                           : theme_->panelBackground);
        g.fillPath (tabShape);
    }

    // FLAT segmented look: NO top outline and NO bottom rule (the depth lines
    // are eliminated — segmentation reads from the active/inactive fill contrast
    // plus the per-tab category underline drawn below, not from box outlines).

    // Label (ALL CAPS), centred; reserve room at the bottom for the active
    // underline.
    juce::Colour textCol = front ? theme_->text : theme_->textDim;
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
    // alpha) so every tab carries its function colour at a glance even when not
    // selected. Sits on top of the bottom rule, marking the segment's lower
    // edge. (Keeps the contiguous segmented shape — still no per-tab vertical
    // outlines between adjacent tabs.)
    g.setColour (front ? catColour : catColour.withMultipliedAlpha (0.32f));
    g.fillRect (juce::Rectangle<float> (activeArea.getX(),
                                        activeArea.getBottom() - 2.0f,
                                        activeArea.getWidth(), 2.0f));
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
    // bg and the card. The section title is a BOLD muted-gray header at the
    // top-left (the old fieldset/legend border-break motif is gone). The card's
    // geometry (panel position / size) is unchanged.
    constexpr float corner = 7.0f;
    const float textH = 14.0f;
    constexpr float titleLeftPad = 9.0f;   // a touch more breathing room than the old 6px
    juce::ignoreUnused (position);

    const auto alpha = group.isEnabled() ? 1.0f : 0.5f;
    const auto cardBounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);

    // Solid card fill only — no outline, no skeuomorphic containerShadow ring.
    // The card reads purely by its tonal step above the window background.
    if (theme_ != nullptr)
    {
        g.setColour (theme_->containerFill.withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (cardBounds, corner);
    }

    // Bold muted-gray section header, left-aligned within the card's top band
    // (GroupComponent::textColourId == theme_->panelHeader after setTheme).
    const juce::Font f = appFont (textH, juce::Font::bold);
    const juce::String displayText = text.toUpperCase();
    const juce::Colour titleCol = group.findColour (juce::GroupComponent::textColourId)
                                      .withMultipliedAlpha (alpha);
    drawHeadingText (g, displayText, f,
                     juce::Rectangle<float> (titleLeftPad, 3.0f,
                                             (float) width - titleLeftPad * 2.0f, textH),
                     titleCol);
}

// A small padlock glyph drawn centred at @p c (size @p sz) — the "can't drop
// here" indicator shown on non-destination controls while a mod-source drag is
// hovered over them. Theme-agnostic (caller supplies the colour).
static void drawPadlock (juce::Graphics& g, juce::Point<float> c, float sz, juce::Colour col)
{
    const float bodyW = sz * 0.66f;
    const float bodyH = sz * 0.52f;
    const auto  body  = juce::Rectangle<float> (c.x - bodyW * 0.5f,
                                                c.y - bodyH * 0.5f + sz * 0.14f,
                                                bodyW, bodyH);
    // Shackle: top half-arc resting on the body's top edge.
    juce::Path shackle;
    const float r = bodyW * 0.34f;
    shackle.addCentredArc (c.x, body.getY(), r, r, 0.0f,
                           juce::MathConstants<float>::pi,
                           juce::MathConstants<float>::twoPi, false);
    g.setColour (col);
    g.strokePath (shackle, juce::PathStrokeType (juce::jmax (1.0f, sz * 0.12f),
                                                 juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
    g.fillRoundedRectangle (body, juce::jmax (1.0f, sz * 0.12f));
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
    const auto fill    = slider.findColour (juce::Slider::rotarySliderFillColourId);      // accent arc (amber / category)
    const auto valueCol = slider.findColour (juce::Slider::textBoxTextColourId);
    const auto trackCol = slider.findColour (juce::Slider::rotarySliderOutlineColourId);  // solid dark-gray track (knobTrack)

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

    // Fill arc — start angle -> value angle, bright accent. Skipped for disabled
    // knobs so they read as inactive.
    if (slider.isEnabled() && rotaryEndAngle > rotaryStartAngle)
    {
        g.setColour (fill);
        juce::Path fillArc;
        fillArc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f,
                               rotaryStartAngle, toAngle, true);
        g.strokePath (fillArc, juce::PathStrokeType (arcWidth, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
    }

    // Centre numeric readout for ACTIVE knobs only (disabled steps show just the
    // track arc, per spec). The value indicator is the knob's primary readout, so
    // it stays on the brightest text tier (Slider::textBoxTextColourId ==
    // theme.textValue). The font is kept compact + auto-shrinks so long values
    // (e.g. "8800.0 Hz") stay within the dial. (See the function-header note on
    // why the readout is retained centred rather than below the label.)
    if (slider.isEnabled())
    {
        const juce::String valueText = slider.getTextFromValue (slider.getValue());
        const float maxTextW = radius * 2.0f;
        juce::Font vf = appFont (juce::jmax (11.0f, radius * 0.52f), juce::Font::plain);
        const int textW = juce::GlyphArrangement::getStringWidthInt (vf, valueText);
        if ((float) textW > maxTextW && textW > 0)
            vf = appFont (juce::jmax (9.0f, vf.getHeight() * maxTextW / (float) textW), juce::Font::plain);

        const auto textRect = bounds.toNearestInt().withSizeKeepingCentre (
            juce::roundToInt (maxTextW), juce::roundToInt (vf.getHeight() * 1.7f));
        drawTextUncurtained (g, valueText, vf, textRect.toFloat(), valueCol,
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
        g.setColour (fill.brighter (0.25f).withAlpha (0.6f));
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
        // The first arc sits just outside the value arc; subsequent arcs step
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
            const juce::Colour col = (colVar != nullptr && colVar->isInt())
                ? juce::Colour ((uint32_t) (int) *colVar)
                : fill;   // fallback to the knob fill if no colour was pushed

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

    // "Can't drop here" padlock: shown on a NON-destination knob while a
    // mod-source drag is hovered over it (ParamControl::setDropLocked).
    {
        const auto* lv = slider.getProperties().getVarPointer ("parvatiModLocked");
        if (lv != nullptr && lv->isBool() && (bool) *lv)
        {
            const float lsz = juce::jmin ((float) width, (float) height) * 0.34f;
            drawPadlock (g, centre, lsz,
                         slider.findColour (juce::Slider::textBoxTextColourId).withAlpha (0.9f));
        }
    }
}

void ParvatiLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                           float sliderPos, float minSliderPos, float maxSliderPos,
                                           juce::Slider::SliderStyle style, juce::Slider& slider)
{
    // FLAT linear slider (the Settings zoom slider + the pitch/mod wheels — both
    // inherit this LookAndFeel). NOTE: the Mod-Matrix depth sliders use their
    // OWN BipolarSliderLNF (ModMatrixView.cpp), so they are NOT routed here.
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

    const juce::Colour trackCol = theme_->knobTrack;          // dark rounded track
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
    // FLAT semi-opaque button chip — a 5px rounded fill (panelBackground2) with
    // NO outline, NO inset shadow and NO arrow bevel (the bulky recessed look is
    // gone). The fill lifts slightly while the drop-down is open for a tonal
    // affordance. The chevron is a minimal ▼ in a subtle token colour (textDim,
    // lifted toward text while open). Inline text is laid out by
    // positionComboBoxText(), whose ~24px right reserve matches the chevron so
    // long choice text never clips.
    if (theme_ == nullptr)
    {
        juce::LookAndFeel_V4::drawComboBox (g, width, height, isButtonDown,
                                            buttonX, buttonY, buttonW, buttonH, box);
        return;
    }

    const auto bg = box.findColour (juce::ComboBox::backgroundColourId);   // panelBackground2
    constexpr float corner = 5.0f;
    const auto r = juce::Rectangle<int> (0, 0, width, height).toFloat().reduced (0.5f);

    // Flat borderless fill (no inset shadow, no arrow bevel). Lifts slightly
    // while the drop-down is open (isButtonDown) for a tonal hover affordance.
    g.setColour (isButtonDown ? bg.brighter (0.10f) : bg);
    g.fillRoundedRectangle (r, corner);

    // Minimal right-aligned ▼ chevron, vertically centred. Subtle textDim,
    // lifted toward the brighter text token while the drop-down is open for a
    // light affordance (NOT always-bright amber).
    const auto chevronCol = isButtonDown ? theme_->text : theme_->textDim;
    constexpr float chevronSize = 4.0f;
    const float cx = (float) width  - 12.0f;
    const float cy = (float) height * 0.5f;
    juce::Path chevron;
    chevron.startNewSubPath (cx - chevronSize, cy - chevronSize * 0.5f);
    chevron.lineTo (cx + chevronSize, cy - chevronSize * 0.5f);
    chevron.lineTo (cx, cy + chevronSize * 0.5f);
    chevron.closeSubPath();
    g.setColour (chevronCol);
    g.fillPath (chevron);

    // "Can't drop here" padlock on a NON-destination combo while a mod-source
    // drag is hovered over it (ParamControl::setDropLocked).
    {
        const auto* lv = box.getProperties().getVarPointer ("parvatiModLocked");
        if (lv != nullptr && lv->isBool() && (bool) *lv)
            drawPadlock (g, juce::Point<float> ((float) width * 0.5f, (float) height * 0.5f),
                         (float) height * 0.7f,
                         box.findColour (juce::ComboBox::textColourId).withAlpha (0.9f));
    }
}

void ParvatiLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    // Inline text: left-padded, stopping before the right-aligned amber chevron
    // (6px left pad + ~18px right reserve for the chevron = 24px chrome). Kept
    // tight so a fit-to-text dropdown reads as wide as its content, not padded.
    label.setBounds (6, 1, box.getWidth() - 24, box.getHeight() - 2);
    label.setFont (appFont (14.0f, juce::Font::plain));
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

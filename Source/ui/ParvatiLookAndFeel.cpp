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
    setColour (juce::ComboBox::outlineColourId,                t.outline);   // flat chip border (drawComboBox strokes theme_->outline)
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
    setColour (juce::TabbedComponent::outlineColourId,         t.outline);   // 1px card border for the nested ENV/LFO + MOD tab cards (top edge from the tab baseline)
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

    // TOP outline only (no vertical sides, no bottom): the rounded corner arcs
    // (where present) + the flat top edge. The cubic-Bezier arcs use the SAME
    // 0.45 control points as Path::addRoundedRectangle above, so the 1px outline
    // sits exactly on the fill's corner edge. (An interior inactive tab has both
    // corners square, so its top is a plain full-width line — the segments join
    // into one continuous top rule.)
    g.setColour (theme_->outline);
    juce::Path topEdge;
    {
        const float tx = activeArea.getX();
        const float tr = activeArea.getRight();
        const float ty = activeArea.getY();
        const float c45 = kTabCorner * 0.45f;
        topEdge.startNewSubPath (tx, roundTL ? (ty + kTabCorner) : ty);
        if (roundTL)
            topEdge.cubicTo (tx, ty + c45, tx + c45, ty, tx + kTabCorner, ty);  // top-left arc
        topEdge.lineTo (roundTR ? (tr - kTabCorner) : tr, ty);                  // flat top
        if (roundTR)
            topEdge.cubicTo (tr - c45, ty, tr, ty + c45, tr, ty + kTabCorner);  // top-right arc
    }
    g.strokePath (topEdge, juce::PathStrokeType (1.0f));

    // Bottom rule: full-width 1px line (the segment's flush lower edge).
    g.setColour (theme_->outline);
    g.drawHorizontalLine (juce::roundToInt (activeArea.getBottom() - 1.0f),
                          activeArea.getX(), activeArea.getRight());

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
    // A sharp-cornered panel card with a subtle interior fill + faint inset
    // depth, whose TOP-LEFT edge is broken by the section title text — the
    // classic fieldset/legend look:
    //   ┌── [ OSC 1 ] ─────────────────┐
    //   │                              │
    //   │  controls                    │
    //   └──────────────────────────────┘
    // The fill + inset "lift" the card off the pure background (R1 mitigation:
    // depth is drawn within bounds, not as an outer Gaussian shadow). The 1px
    // outline + title-break geometry are unchanged; the title sits IN the top
    // border line and renders in ALL CAPS with extra weight (see drawHeadingText).
    const float textH = 14.3f;   // +10% over the legacy 13px section header
    const float textPad = 6.0f;   // gap either side of the title inside the border break
    // The top border line runs at yTop so the title text straddles it.
    const float yTop = (float) juce::roundToInt (textH * 0.5f);
    juce::ignoreUnused (position);   // panels are always anchored top-left

    // Measure the title with the SAME bold weight used to render it
    // (drawHeadingText re-resolves to bold). Plain glyphs are narrower, so a
    // plain measurement left the bold title clipped to a too-narrow break.
    const juce::Font f = appFont (textH, juce::Font::bold);
    const juce::String displayText = text.toUpperCase();   // panel headings render in ALL CAPS
    const auto alpha = group.isEnabled() ? 1.0f : 0.5f;

    const int textW = displayText.isEmpty()
                        ? 0
                        : juce::GlyphArrangement::getStringWidthInt (f, displayText);

    // The title break occupies [breakX0 .. breakX1) on the top edge. Anchored
    // top-left (matches the GroupComponent's top|left justification).
    // kTitleSlack widens BOTH the draw rect and the border break so that even
    // a drawText wordWrapWidth curtailment (sub-pixel kerning vs the measured
    // width) cannot drop the LAST glyph. (drawHeadingText now routes through
    // drawTextUncurtained, but the slack is kept as belt-and-suspenders.)
    constexpr float kTitleSlack = 6.0f;
    const float breakX0 = textPad;
    const float breakX1 = breakX0 + (float) textW + kTitleSlack;
    const float x0 = 0.5f;            // pixel-snapped inset so the 1px line is crisp
    const float x1 = (float) width  - 0.5f;
    const float y1 = (float) height - 0.5f;

    // --- container depth: subtle interior fill ("lifts" the card off the bg) +
    // a faint inset depth ring just inside the outline (pseudo drop/bevel). The
    // fill covers the FULL card bounds (uniform lift); both read from the theme
    // so the 5 themes (incl. the light Paper theme) adapt. ---
    if (theme_ != nullptr)
    {
        g.setColour (theme_->containerFill.withMultipliedAlpha (alpha));
        g.fillRect (juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height));

        g.setColour (theme_->containerShadow.withMultipliedAlpha (0.5f * alpha));
        g.drawRect (juce::Rectangle<float> (1.0f, 1.0f,
                                            (float) width - 2.0f, (float) height - 2.0f), 1.0f);
    }

    // --- 1px outline: top edge split around the title break, then 3 sides. ---
    const juce::Colour outlineCol = group.findColour (juce::GroupComponent::outlineColourId)
                                        .withMultipliedAlpha (alpha);
    g.setColour (outlineCol);
    g.drawHorizontalLine (juce::roundToInt (yTop), x0, breakX0);
    g.drawHorizontalLine (juce::roundToInt (yTop), breakX1, x1);
    g.drawVerticalLine   (juce::roundToInt (x1 - 0.5f), yTop, y1);
    g.drawHorizontalLine (juce::roundToInt (y1 - 0.5f), x0, x1);
    g.drawVerticalLine   (juce::roundToInt (x0), yTop, y1);

    // --- weighty section title, centred ON the top border line (in the break). ---
    const juce::Colour titleCol = group.findColour (juce::GroupComponent::textColourId)
                                      .withMultipliedAlpha (alpha);
    drawHeadingText (g, displayText, f,
                     juce::Rectangle<float> (breakX0, yTop - textH * 0.5f,
                                             (float) textW + kTitleSlack, textH),
                     titleCol);
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
    // Base size is enlarged by ~50% for legibility (the value indicator is the
    // knob's primary readout); maxTextW widens with it so typical values don't
    // shrink. Colour reads Slider::textBoxTextColourId (== theme.textValue, the
    // brightest tier) for maximum contrast.
    if (slider.isEnabled())
    {
        const juce::String valueText = slider.getTextFromValue (slider.getValue());
        const float maxTextW = radius * 2.2f;
        juce::Font vf = appFont (juce::jmax (14.0f, radius * 0.93f), juce::Font::plain);
        const int textW = juce::GlyphArrangement::getStringWidthInt (vf, valueText);
        if ((float) textW > maxTextW && textW > 0)
            vf = appFont (juce::jmax (12.0f, vf.getHeight() * maxTextW / (float) textW), juce::Font::plain);

        const auto textRect = bounds.toNearestInt().withSizeKeepingCentre (
            juce::roundToInt (maxTextW), juce::roundToInt (vf.getHeight() * 1.7f));
        drawTextUncurtained (g, valueText, vf, textRect.toFloat(), valueCol,
                             juce::Justification::centred);
    }
}

void ParvatiLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height,
                                       bool isButtonDown,
                                       int buttonX, int buttonY,
                                       int buttonW, int buttonH,
                                       juce::ComboBox& box)
{
    // FLAT selection chip — a 4px rounded frame matching the pill buttons, a
    // thin 1px outline stroke (the subtle outline token, NOT the always-accent
    // border) and NO innerShadow inset bevel (that was the bulky recessed look).
    // The chevron is a minimal ▼ drawn in a subtle token colour (textDim, lifted
    // toward text while the drop-down is open) instead of always-bright amber.
    // Inline text is laid out by positionComboBoxText(), whose ~24px right
    // reserve matches the chevron so long choice text never clips.
    if (theme_ == nullptr)
    {
        juce::LookAndFeel_V4::drawComboBox (g, width, height, isButtonDown,
                                            buttonX, buttonY, buttonW, buttonH, box);
        return;
    }

    const auto bg = box.findColour (juce::ComboBox::backgroundColourId);   // panelBackground2
    constexpr float corner = 4.0f;
    const auto r = juce::Rectangle<int> (0, 0, width, height).toFloat().reduced (0.5f);

    // Subtle dark fill (flat — no inset bevel).
    g.setColour (bg);
    g.fillRoundedRectangle (r, corner);

    // Thin 1px outline stroke (subtle outline token, matching the pill buttons).
    g.setColour (theme_->outline);
    g.drawRoundedRectangle (r, corner, 1.0f);

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
    // FLAT pill button — 4px rounded corners, a thin 1px stroke (the subtle
    // outline token, NOT accent-on-every-button) and NO innerShadow inset bevel
    // (that was the bulky 3D look to flatten). State routing reuses the existing
    // colour-ID path: backgroundColour already encodes on/off (TextButton passes
    // buttonOnColourId == theme accent when toggled on, buttonColourId ==
    // panelBackground otherwise), so:
    //   - toggled-on: solid accent fill + bright text-coloured border;
    //   - default/off: subtle panel fill + outline stroke;
    //   - hover (off): fill lifts (glow) + border brightens toward the text
    //     colour for affordance.
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

    // Fill (on/off aware via backgroundColour; slight glow on hover/press).
    auto fill = backgroundColour.withMultipliedAlpha (enabledAlpha);
    if (! on)
    {
        if (down)
            fill = fill.brighter (0.10f);
        else if (over)
            fill = fill.brighter (0.06f);
    }
    g.setColour (fill);
    g.fillRoundedRectangle (r, corner);   // 4px rounded, flat

    // Thin 1px stroke: outline by default, brightened toward the bright text
    // colour on hover/press, and the full bright text colour when toggled on.
    juce::Colour stroke;
    if (on)
        stroke = theme_->text;
    else if (over || down)
        stroke = theme_->outline.interpolatedWith (theme_->text, 0.55f);
    else
        stroke = theme_->outline;
    g.setColour (stroke.withMultipliedAlpha (enabledAlpha));
    g.drawRoundedRectangle (r, corner, 1.0f);
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

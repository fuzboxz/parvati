// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.
//
// IconButton — a small square button that draws its glyph with juce::Path (no
// font/unicode dependency), so Undo/Redo/Settings render correctly on every OS
// (unicode arrows U+21B6/21B7 show as "…" on font stacks that lack them).
// Colours come from the editor-wide HellcatLookAndFeel (inherited). The gear
// honours the toggle state (Settings "on" = accent) like the old TextButton.

#pragma once

#include <cmath>

#include <juce_gui_basics/juce_gui_basics.h>

#include "HellcatLookAndFeel.h"
#include "HellcatTheme.h"

class IconButton : public juce::Button
{
public:
    enum class Icon { Undo, Redo, Gear, Close };

    // Accessible name for each glyph (screen readers see only this — the icon
    // itself is pure Path drawing). "Undo"/"Redo"/"Settings" are existing
    // chrome translation keys (Translations.cpp), so the fallback chain is
    // localized at no extra cost; English elsewhere. "Delete modulation" names the
    // mod-matrix row X (the Close glyph's introducing consumer — a reuse can
    // override the title with setTitle()).
    static juce::String iconTitle (Icon icon)
    {
        switch (icon)
        {
            case Icon::Undo:  return TRANS ("Undo");
            case Icon::Redo:  return TRANS ("Redo");
            case Icon::Gear:  return TRANS ("Settings");
            case Icon::Close: return TRANS ("Delete modulation");
        }
        return {};
    }

    explicit IconButton (Icon icon) : juce::Button ({}), icon_ (icon)
    {
        // Accessibility-only: the default Button accessibility handler reads
        // Component::getTitle() first (falling back to the empty button text),
        // so an explicit title names the glyph-drawn button. Does not affect
        // painting (only the Path glyph is drawn) or the component name.
        setTitle (iconTitle (icon));
    }

    void setIcon (Icon icon) { icon_ = icon; setTitle (iconTitle (icon)); repaint(); }

    // Visual compactness for a large hit target: the glyph is drawn inside the
    // bounds reduced by @p inset px (default 4 — the historical look of the
    // header Undo/Redo/Gear icons). The mod-matrix row delete X uses a larger
    // inset so a 44pt HIG hit target renders a compact glyph (the
    // FxSlotCard PowerToggle pinning idiom, expressed as an inset).
    void setGlyphInset (float inset) { glyphInset_ = juce::jmax (1.0f, inset); repaint(); }

    void paintButton (juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override
    {
        const HellcatTheme* t = hellcat::themeFor (*this);
        const juce::Colour text   = t ? t->textPrimary   : hellcat::kFallbackTextPrimary;
        const juce::Colour accent = t ? t->accentPrimary : hellcat::hellcatFallbackAccent;

        juce::Colour c = text;
        if (! isEnabled())            c = text.withAlpha (0.30f);
        else if (getToggleState() || isButtonDown)  c = accent;   // e.g. Settings "on" or pressed
        else if (isMouseOverButton)   c = text.brighter (0.20f);

        g.setColour (c);
        const auto r = getLocalBounds().toFloat().reduced (glyphInset_);
        if (icon_ == Icon::Gear)       drawGear (g, r);
        else if (icon_ == Icon::Close) drawClose (g, r);
        else                           drawCurvedArrow (g, r, icon_ == Icon::Redo);
    }

private:
    // A compact "X": two rounded-cap strokes corner to corner. Pure Path
    // drawing (no font dependency), themed like the other glyphs. Stroke
    // bolded 2.0 -> 2.6 (2026-08-3 vector-boldness pass).
    static void drawClose (juce::Graphics& g, juce::Rectangle<float> r)
    {
        const auto inner = r.reduced (juce::jmin (r.getWidth(), r.getHeight()) * 0.18f);
        juce::Path x;
        x.startNewSubPath (inner.getTopLeft());
        x.lineTo (inner.getBottomRight());
        x.startNewSubPath (inner.getTopRight());
        x.lineTo (inner.getBottomLeft());
        g.strokePath (x, juce::PathStrokeType (2.6f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
    }

    // The "rainbow" arc with an arrowhead at one foot (2026-08-23 revision
    // 3: back to the RAINBOW silhouette — the true half circle read bad —
    // but built from a REAL ELLIPSE quarter pair so the curve is properly
    // round instead of the old quadratic's pointed dome: a shallow wide
    // ellipse whose top HALF spans foot-to-foot (flatten ~0.78), giving the
    // classic rounded-rainbow with vertically-departing feet the arrowhead
    // tangent needs. Undo => head at the LEFT foot; Redo => mirror.
    // The head is filled + stroked with the SAME stroke object as the arc
    // (one solid weight; see the colour note below).
    static void drawCurvedArrow (juce::Graphics& g, juce::Rectangle<float> r, bool clockwise)
    {
        // 2026-08-23 revision 4 (user spec: "the vector should start from
        // center left and point to center right with an arrow on the end, or
        // the other way"): a single OVER-THE-TOP arc whose FEET sit at the
        // cell's vertical CENTER (left-center -> right-center), with a
        // HORIZONTAL chevron arrowhead at the destination foot pointing
        // outward (Undo => head at the LEFT foot pointing LEFT; Redo =>
        // mirrored). The whole glyph — arc + both barbs — is ONE stroked
        // path with round caps/joins and NO fills: a pure-stroke glyph has
        // bit-identical colour on every segment (the persistent "arc vs
        // arrow look different colours / translucent" report was the
        // fill-vs-stroke AA seam, which cannot exist without a fill).
        const auto c = r.getCentre();
        const float rad = juce::jmin (r.getWidth(), r.getHeight()) * 0.40f;
        const float ry  = rad * 0.85f;              // round dome, not pointed
        const float footY  = c.y;                   // feet at the vertical CENTER
        const float leftX  = c.x - rad;
        const float rightX = c.x + rad;

        juce::Path p;
        // The arc: left-center up over the round dome to right-center.
        p.startNewSubPath (leftX, footY);
        p.addCentredArc (c.x, footY, rad, ry, 0.0f,
                         juce::MathConstants<float>::pi,
                         juce::MathConstants<float>::twoPi, false);

        // Horizontal chevron head at the DESTINATION foot: tip one head-length
        // OUT along x, barbs sweeping back past the foot. Overlapping the arc
        // end is intentional — the classic ↶ / ↷ glyph.
        const float ah  = rad * 0.52f;              // head size
        const float dir = clockwise ? 1.0f : -1.0f;
        const float ax  = clockwise ? rightX : leftX;
        const float tipX = ax + dir * ah;
        const float backX = ax - dir * ah * 0.18f;
        p.startNewSubPath (tipX, footY);
        p.lineTo (backX, footY - ah * 0.62f);
        p.startNewSubPath (tipX, footY);
        p.lineTo (backX, footY + ah * 0.62f);

        g.strokePath (p, juce::PathStrokeType (2.8f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
    }

    // A gear: 8 radial teeth + an outer ring + an inner hole, all stroked.
    // Strokes bolded (teeth/ring 2.0 -> 2.6, hole 1.6 -> 2.2) for the
    // 2026-08-23 vector-boldness pass.
    static void drawGear (juce::Graphics& g, juce::Rectangle<float> r)
    {
        const auto c = r.getCentre();
        const float outer = juce::jmin (r.getWidth(), r.getHeight()) * 0.30f;
        const float inner = outer * 0.50f;
        const int   teeth = 8;
        for (int i = 0; i < teeth; ++i)
        {
            const float a = juce::MathConstants<float>::twoPi * float (i) / float (teeth);
            const float dx = std::cos (a), dy = std::sin (a);
            g.drawLine (juce::Line<float> (c.x + dx * outer,        c.y + dy * outer,
                                            c.x + dx * outer * 1.30f, c.y + dy * outer * 1.30f),
                        2.6f);
        }
        g.drawEllipse (c.x - outer, c.y - outer, outer * 2.0f, outer * 2.0f, 2.6f);
        g.drawEllipse (c.x - inner, c.y - inner, inner * 2.0f, inner * 2.0f, 2.2f);
    }

    Icon icon_;
    float glyphInset_ = 4.0f;
};

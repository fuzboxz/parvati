// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// IconButton — a small square button that draws its glyph with juce::Path (no
// font/unicode dependency), so Undo/Redo/Settings render correctly on every OS
// (unicode arrows U+21B6/21B7 show as "…" on font stacks that lack them).
// Colours come from the editor-wide ParvatiLookAndFeel (inherited). The gear
// honours the toggle state (Settings "on" = accent) like the old TextButton.

#pragma once

#include <cmath>

#include <juce_gui_basics/juce_gui_basics.h>

#include "ParvatiLookAndFeel.h"
#include "ParvatiTheme.h"

class IconButton : public juce::Button
{
public:
    enum class Icon { Undo, Redo, Gear };

    // Accessible name for each glyph (screen readers see only this — the icon
    // itself is pure Path drawing). "Undo"/"Redo"/"Settings" are existing
    // chrome translation keys (Translations.cpp), so the fallback chain is
    // localized for free; English elsewhere.
    static juce::String iconTitle (Icon icon)
    {
        switch (icon)
        {
            case Icon::Undo: return TRANS ("Undo");
            case Icon::Redo: return TRANS ("Redo");
            case Icon::Gear: return TRANS ("Settings");
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

    void paintButton (juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override
    {
        const ParvatiTheme* t = nullptr;
        if (auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel()))
            t = lnf->getTheme();
        const juce::Colour text   = t ? t->textPrimary   : juce::Colour (0xffe8e8ee);
        const juce::Colour accent = t ? t->accentPrimary : parvati::parvatiFallbackAccent;

        juce::Colour c = text;
        if (! isEnabled())            c = text.withAlpha (0.30f);
        else if (getToggleState() || isButtonDown)  c = accent;   // e.g. Settings "on" or pressed
        else if (isMouseOverButton)   c = text.brighter (0.20f);

        g.setColour (c);
        const auto r = getLocalBounds().toFloat().reduced (4.0f);
        if (icon_ == Icon::Gear) drawGear (g, r);
        else                     drawCurvedArrow (g, r, icon_ == Icon::Redo);
    }

private:
    // A "rainbow" arc with an arrowhead at one foot. Undo => head at the LEFT
    // foot pointing left; Redo => mirror (head at the RIGHT foot pointing right).
    static void drawCurvedArrow (juce::Graphics& g, juce::Rectangle<float> r, bool clockwise)
    {
        const auto c = r.getCentre();
        const float rad = juce::jmin (r.getWidth(), r.getHeight()) * 0.34f;
        const float leftX  = c.x - rad;
        const float rightX = c.x + rad;
        const float footY  = c.y + rad * 0.45f;

        juce::Path arc;
        arc.startNewSubPath (leftX, footY);
        arc.quadraticTo (c.x, footY - rad * 2.1f, rightX, footY);
        g.strokePath (arc, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved));

        const float ah  = rad * 0.55f;                 // arrowhead size
        const float ax  = clockwise ? rightX : leftX;
        const float dir = clockwise ? 1.0f : -1.0f;
        juce::Path head;
        head.startNewSubPath (ax + dir * ah, footY);
        head.lineTo (ax, footY - ah * 0.7f);
        head.lineTo (ax, footY + ah * 0.7f);
        head.closeSubPath();
        g.fillPath (head);
    }

    // A gear: 8 radial teeth + an outer ring + an inner hole, all stroked.
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
                        2.0f);
        }
        g.drawEllipse (c.x - outer, c.y - outer, outer * 2.0f, outer * 2.0f, 2.0f);
        g.drawEllipse (c.x - inner, c.y - inner, inner * 2.0f, inner * 2.0f, 1.6f);
    }

    Icon icon_;
};

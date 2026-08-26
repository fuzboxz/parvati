// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.
//
// VectorTrace — the single modern vector recipe shared by the three
// data-visualization canvases (EnvelopeDisplay, OscPreviewDisplay,
// FilterResponseDisplay). It replaces the former chunky dot-matrix / 3px-LCD-cell
// rasterization with smooth, anti-aliased stroked `juce::Path`s plus a
// translucent vertical gradient area fill, matching the smooth rotary-arc style
// in `HellcatLookAndFeel::drawRotarySlider` (Path + PathStrokeType(curved,
// rounded)).
//
// THEME-SAFE by contract: NO colour literals live here. Every colour is supplied
// by the caller from `HellcatTheme` tokens. The gradient endpoints derive from
// the single `trace` colour via `.withAlpha()` only (peak end brightened, the
// baseline end fully transparent), so the whole graph re-tints on a category /
// theme change with no extra wiring.
//
// The DATA / math (envelope eases, oscillator DSP sampling, filter magnitude)
// stays in each component; only the rasterization lives here.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace hellcat::vectorTrace
{
//==============================================================================
// How `levelAt(xf)` is interpreted.
enum class Mode
{
    unipolar,   // level 0..1  (1 = top of plot, 0 = bottom baseline)
    bipolar     // level -1..1 (1 = top, -1 = bottom, 0 = vertical centre baseline)
};

//------------------------------------------------------------------------------
// Map a normalized level to a Y within `area`.
inline float levelToY (juce::Rectangle<float> area, Mode mode, float level)
{
    const float top = area.getY();
    const float H   = area.getHeight();

    if (mode == Mode::unipolar)
        return top + (1.0f - juce::jlimit (0.0f, 1.0f, level)) * H;

    const float l = juce::jlimit (-1.0f, 1.0f, level);
    return top + (1.0f - (l + 1.0f) * 0.5f) * H;
}

// The baseline Y the area fill closes to (bottom for unipolar, centre for bipolar).
inline float baselineY (juce::Rectangle<float> area, Mode mode)
{
    return mode == Mode::bipolar ? area.getCentre().y : area.getBottom();
}

//==============================================================================
// Faint CLEAN grid: thin 1px lines (NOT dots, NOT cells) covering `area`.
// Vertical + horizontal lines spaced `spacing` px apart in `lineColour`
// (typically `divider.withAlpha(0.10f)` or `accent.withAlpha(0.06f)`).
inline void drawGrid (juce::Graphics& g, juce::Rectangle<float> area,
                      juce::Colour lineColour, float spacing)
{
    if (spacing < 2.0f)
        return;

    g.setColour (lineColour);
    const float x0 = area.getX();
    const float x1 = area.getRight();
    const float y0 = area.getY();
    const float y1 = area.getBottom();

    const int numV = juce::jmax (0, juce::roundToInt ((x1 - x0) / spacing) - 1);
    for (int i = 1; i <= numV; ++i)
    {
        const float x = x0 + static_cast<float> (i) * spacing;
        g.drawVerticalLine (juce::roundToInt (x), y0, y1);
    }
    const int numH = juce::jmax (0, juce::roundToInt ((y1 - y0) / spacing) - 1);
    for (int i = 1; i <= numH; ++i)
    {
        const float y = y0 + static_cast<float> (i) * spacing;
        g.drawHorizontalLine (juce::roundToInt (y), x0, x1);
    }
}

//==============================================================================
// Build the TRACE path through sampled levels. `levelAt(xf)` is evaluated for
// xf in [0,1] at `count` evenly spaced columns (so `count`+1 points).
//   staircase == true  -> explicit horizontal-hold + vertical-step segments, so
//                          a sample-and-hold LFO keeps its crisp staircase.
//   staircase == false -> straight segments between samples; the caller strokes
//                          with PathStrokeType::curved/rounded for a smooth curve.
inline juce::Path buildTrace (juce::Rectangle<float> area, int count,
                              const std::function<float(float)>& levelAt,
                              Mode mode, bool staircase)
{
    juce::Path p;
    const int n = juce::jmax (1, count);
    const float x0 = area.getX();
    const float W  = area.getWidth();

    const auto colX = [&] (int i)
    {
        return x0 + (static_cast<float> (i) / static_cast<float> (n)) * W;
    };

    p.startNewSubPath (x0, levelToY (area, mode, levelAt (0.0f)));

    if (staircase)
    {
        // Hold each sample's level across its column, then step vertically at the
        // next boundary (true sample-and-hold geometry).
        float prevY = levelToY (area, mode, levelAt (0.0f));
        for (int i = 1; i <= n; ++i)
        {
            const float xf = static_cast<float> (i) / static_cast<float> (n);
            const float x  = colX (i);
            const float y  = levelToY (area, mode, levelAt (xf));
            p.lineTo (x, prevY);   // horizontal hold of the previous level
            p.lineTo (x, y);       // vertical step to the new level
            prevY = y;
        }
    }
    else
    {
        for (int i = 1; i <= n; ++i)
        {
            const float xf = static_cast<float> (i) / static_cast<float> (n);
            p.lineTo (colX (i), levelToY (area, mode, levelAt (xf)));
        }
    }
    return p;
}

//==============================================================================
// Build the AREA-FILL path: the trace closed down to the baseline.
inline juce::Path buildArea (juce::Rectangle<float> area, int count,
                             const std::function<float(float)>& levelAt,
                             Mode mode, bool staircase)
{
    juce::Path p = buildTrace (area, count, levelAt, mode, staircase);
    const float baseY = baselineY (area, mode);
    p.lineTo (area.getRight(), baseY);
    p.lineTo (area.getX(), baseY);
    p.closeSubPath();
    return p;
}

//==============================================================================
// The vertical gradient used to fill the area. BOTH endpoints derive from
// `trace` only (no new colours): the peak end is `trace.withAlpha(glowAlpha)`,
// the baseline end is fully transparent.
//   unipolar -> glow at the TOP edge fading to transparent at the bottom baseline.
//   bipolar  -> glow at BOTH the top and bottom peaks fading to transparent at
//               the centre baseline (a symmetric glow around the midline).
inline juce::ColourGradient fillGradient (juce::Rectangle<float> area,
                                          juce::Colour trace,
                                          float glowAlpha, Mode mode)
{
    const float midX = area.getCentre().x;
    const float topY = area.getY();
    const float botY = area.getBottom();
    const juce::Colour glow  = trace.withAlpha (glowAlpha);
    const juce::Colour clear = trace.withAlpha (0.0f);

    if (mode == Mode::bipolar)
    {
        juce::ColourGradient grad (glow, midX, topY, glow, midX, botY, false);
        grad.addColour (0.5, clear);
        return grad;
    }
    return juce::ColourGradient (glow, midX, topY, clear, midX, botY, false);
}

//==============================================================================
// Render one trace: translucent gradient-filled area + stroked trace. The caller
// draws the panel background, grid, per-component reference lines and labels.
//   strokeW   - trace stroke width (1.5f matches the rotary arc).
//   glowAlpha - gradient peak alpha (0.22f; <= 0.18f for the 42px filter).
inline void render (juce::Graphics& g, juce::Rectangle<float> area, int count,
                    const std::function<float(float)>& levelAt,
                    juce::Colour trace, Mode mode, bool staircase,
                    float strokeW = 1.5f, float glowAlpha = 0.22f)
{
    {
        juce::Path areaPath = buildArea (area, count, levelAt, mode, staircase);
        g.setGradientFill (fillGradient (area, trace, glowAlpha, mode));
        g.fillPath (areaPath);
    }
    {
        juce::Path tracePath = buildTrace (area, count, levelAt, mode, staircase);
        g.setColour (trace);
        if (staircase)
            g.strokePath (tracePath,
                          juce::PathStrokeType (strokeW,
                                                juce::PathStrokeType::mitered,
                                                juce::PathStrokeType::butt));
        else
            g.strokePath (tracePath,
                          juce::PathStrokeType (strokeW,
                                                juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
    }
}

}  // namespace hellcat::vectorTrace

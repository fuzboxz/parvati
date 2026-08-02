// Copyright (c) 2024 805LABS / Parvati.  See EnvelopeDisplay.h.

#include "EnvelopeDisplay.h"

#include <cmath>

//==============================================================================
EnvelopeDisplay::EnvelopeDisplay (juce::String title,
                                  std::function<float()> getAttack,
                                  std::function<float()> getDecay,
                                  std::function<float()> getSustain,
                                  std::function<float()> getRelease)
    : title_ (std::move (title)),
      getAttack_   (std::move (getAttack)),
      getDecay_    (std::move (getDecay)),
      getSustain_  (std::move (getSustain)),
      getRelease_  (std::move (getRelease))
{
    // A getter that was not supplied reads as 0 (so a default-constructed /
    // partially-bound display still renders a sane shape).
    if (! getAttack_)  getAttack_  = [] { return 0.0f; };
    if (! getDecay_)   getDecay_   = [] { return 0.0f; };
    if (! getSustain_) getSustain_ = [] { return 0.0f; };
    if (! getRelease_) getRelease_ = [] { return 0.0f; };

    // Accessibility name/description (read by the default handler). The title is
    // also mirrored onto the Component so screen readers announce e.g. "Env 1".
    juce::Component::setTitle (title_);
    setDescription ("ADSR envelope preview");

    startTimerHz (30);
}

EnvelopeDisplay::~EnvelopeDisplay()
{
    stopTimer();
}

//==============================================================================
float EnvelopeDisplay::fetch (const std::function<float()>& f) const
{
    const float v = (f ? f() : 0.0f);
    return juce::jlimit (0.0f, 1.0f, v);
}

void EnvelopeDisplay::timerCallback()
{
    const float a = fetch (getAttack_);
    const float d = fetch (getDecay_);
    const float s = fetch (getSustain_);
    const float r = fetch (getRelease_);

    constexpr float eps = 1.0f / 512.0f;   // ~0.002: ignore sub-knob jitter
    if (std::fabs (a - lastA_) > eps || std::fabs (d - lastD_) > eps
        || std::fabs (s - lastS_) > eps || std::fabs (r - lastR_) > eps)
    {
        lastA_ = a; lastD_ = d; lastS_ = s; lastR_ = r;
        repaint();
    }
}

//==============================================================================
void EnvelopeDisplay::paint (juce::Graphics& g)
{
    // Read the active theme from the component's LookAndFeel (null-safe: a few
    // sensible fallback colours are used if there is no ParvatiLookAndFeel, so
    // the component also renders correctly in a plain host / test harness).
    auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel());
    const ParvatiTheme* t = lnf ? lnf->getTheme() : nullptr;

    const auto panelBg  = t ? t->panelBackground : juce::Colour (0xff24242e);
    const auto outline  = t ? t->outline         : juce::Colour (0xff3c3c4a);
    const auto accent   = t ? t->accent          : juce::Colour (0xffe8b84b);
    const auto trackCol = t ? t->knobTrack       : outline;
    const auto textDim  = t ? t->textDim         : juce::Colour (0xff9a9aa8);

    const auto bounds = getLocalBounds().toFloat();
    const float corner = 4.0f;

    g.setColour (panelBg);
    g.fillRoundedRectangle (bounds, corner);

    g.setColour (outline);
    g.drawRoundedRectangle (bounds.reduced (0.5f), corner, 1.0f);

    // Title (top-left).
    g.setColour (textDim);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (title_,
                bounds.reduced (9.0f, 4.0f).removeFromTop (16),
                juce::Justification::topLeft);

    // Plot area (below the title, with side/bottom padding).
    auto plot = bounds.reduced (8.0f, 0.0f);
    plot.removeFromTop (22.0f);
    plot.removeFromBottom (8.0f);
    const float left = plot.getX();
    const float W = plot.getWidth();
    const float top = plot.getY();
    const float bottom = plot.getY() + plot.getHeight();
    const float H = plot.getHeight();

    // Faint horizontal grid at 25/50/75 %.
    g.setColour (trackCol.withAlpha (0.25f));
    for (int i = 1; i < 4; ++i)
    {
        const float y = top + H * (static_cast<float> (i) / 4.0f);
        g.drawHorizontalLine (juce::roundToInt (y), left, left + W);
    }

    const float a = lastA_ >= 0.0f ? lastA_ : fetch (getAttack_);
    const float d = lastD_ >= 0.0f ? lastD_ : fetch (getDecay_);
    const float s = lastS_ >= 0.0f ? lastS_ : fetch (getSustain_);
    const float r = lastR_ >= 0.0f ? lastR_ : fetch (getRelease_);

    // Segment widths: a small base so a 0 value is still visible, plus the
    // knob's contribution. The sustain plateau is a fixed middle portion.
    const float baseW = 0.06f;
    const float rangeW = 0.30f;
    const float wA = baseW + a * rangeW;
    const float wD = baseW + d * rangeW;
    const float wS = 0.16f;                 // sustain plateau (fixed)
    const float wR = baseW + r * rangeW;
    const float total = wA + wD + wS + wR;

    const float fracA = wA / total;
    const float fracD = wD / total;
    const float fracS = wS / total;
    const float fracR = wR / total;
    const float xEndA = fracA;
    const float xEndD = fracA + fracD;
    const float xEndS = fracA + fracD + fracS;

    auto xAt = [&] (float frac) { return left + frac * W; };
    auto yAt = [&] (float level) { return bottom - level * H; };   // level 0..1

    // Build the curve as a smooth path of sampled points.
    const int stepsPerSeg = 24;
    juce::Path curve;
    curve.startNewSubPath (xAt (0.0f), yAt (0.0f));

    // Attack: 0 -> 1, exponential ease-out rise (fast then settling).
    for (int i = 1; i <= stepsPerSeg; ++i)
    {
        const float tt = static_cast<float> (i) / stepsPerSeg;
        const float x = tt * fracA;
        const float level = 1.0f - std::pow (1.0f - tt, 2.0f);
        curve.lineTo (xAt (x), yAt (level));
    }
    // Decay: 1 -> sustain, exponential settle.
    for (int i = 1; i <= stepsPerSeg; ++i)
    {
        const float tt = static_cast<float> (i) / stepsPerSeg;
        const float x = fracA + tt * fracD;
        const float level = s + (1.0f - s) * std::pow (1.0f - tt, 2.0f);
        curve.lineTo (xAt (x), yAt (level));
    }
    // Sustain plateau.
    curve.lineTo (xAt (xEndD), yAt (s));
    curve.lineTo (xAt (xEndS), yAt (s));
    // Release: sustain -> 0, exponential decay.
    for (int i = 1; i <= stepsPerSeg; ++i)
    {
        const float tt = static_cast<float> (i) / stepsPerSeg;
        const float x = xEndS + tt * fracR;
        const float level = s * std::pow (1.0f - tt, 2.0f);
        curve.lineTo (xAt (x), yAt (level));
    }

    // Fill under the curve (translucent accent).
    juce::Path fill (curve);
    fill.lineTo (xAt (fracA + fracD + fracS + fracR), yAt (0.0f));
    fill.lineTo (xAt (0.0f), yAt (0.0f));
    fill.closeSubPath();
    g.setColour (accent.withAlpha (0.16f));
    g.fillPath (fill);

    // Curve stroke.
    g.setColour (accent);
    g.strokePath (curve, juce::PathStrokeType (1.8f));

    // Junction dots: peak (end of attack), sustain start (end of decay),
    // release start (end of sustain plateau).
    auto dot = [&] (float frac, float level)
    {
        const juce::Point<float> p (xAt (frac), yAt (level));
        g.setColour (accent);
        g.fillEllipse (p.x - 2.6f, p.y - 2.6f, 5.2f, 5.2f);
        g.setColour (panelBg);
        g.fillEllipse (p.x - 1.1f, p.y - 1.1f, 2.2f, 2.2f);
    };
    dot (xEndA, 1.0f);
    dot (xEndD, s);
    dot (xEndS, s);
}

//==========================================================================
std::unique_ptr<juce::AccessibilityHandler> EnvelopeDisplay::createAccessibilityHandler()
{
    // Role `group`: a labelled preview readout. The name (the envelope title,
    // e.g. "Env 1") and description ("ADSR envelope preview") are announced by
    // the default handler from the Component's title/description.
    return std::make_unique<juce::AccessibilityHandler> (*this,
            juce::AccessibilityRole::group);
}

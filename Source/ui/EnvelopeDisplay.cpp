// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See EnvelopeDisplay.h.

#include "EnvelopeDisplay.h"

#include <cmath>

//==============================================================================
EnvelopeDisplay::EnvelopeDisplay (juce::String title,
                                 std::function<float()> getAttack,
                                 std::function<float()> getDecay,
                                 std::function<float()> getSustain,
                                 std::function<float()> getRelease,
                                 std::function<float()> getShape)
    : title_ (std::move (title)),
      getAttack_   (std::move (getAttack)),
      getDecay_    (std::move (getDecay)),
      getSustain_  (std::move (getSustain)),
      getRelease_  (std::move (getRelease)),
      getShape_    (std::move (getShape))
{
    // A getter that was not supplied reads as 0 (so a default-constructed /
    // partially-bound display still renders a sane shape).
    if (! getAttack_)  getAttack_  = [] { return 0.0f; };
    if (! getDecay_)   getDecay_   = [] { return 0.0f; };
    if (! getSustain_) getSustain_ = [] { return 0.0f; };
    if (! getRelease_) getRelease_ = [] { return 0.0f; };
    if (! getShape_)   getShape_   = [] { return 0.0f; };

    // Accessibility name/description (read by the default handler). The title is
    // also mirrored onto the Component so screen readers announce e.g. "Env 1".
    juce::Component::setTitle (title_);
    setDescription ("ADSR envelope / LFO waveform preview");

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
    const float sh = fetch (getShape_);

    constexpr float eps = 1.0f / 512.0f;   // ~0.002: ignore sub-knob jitter
    if (std::fabs (a - lastA_) > eps || std::fabs (d - lastD_) > eps
        || std::fabs (s - lastS_) > eps || std::fabs (r - lastR_) > eps
        || std::fabs (sh - lastShape_) > eps)
    {
        lastA_ = a; lastD_ = d; lastS_ = s; lastR_ = r; lastShape_ = sh;
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
    g.setFont (lnf ? lnf->appFont (13.0f, juce::Font::plain)
                   : juce::Font (juce::FontOptions (13.0f)));
    g.drawText (title_,
                bounds.reduced (9.0f, 4.0f).removeFromTop (16),
                juce::Justification::topLeft);

    // ---- Plot area: a blocky lo-fi "LCD pixel" display ----
    // The envelope/LFO shape is drawn as a grid of square "pixels" (lit = the
    // theme accent) instead of a smooth vector curve, for a retro 64-pixel-LCD
    // look. A faint backdrop lights every cell (in the knob-track colour) so the
    // pixel grid reads even where the wave is absent.
    auto plot = bounds.reduced (8.0f, 0.0f);
    plot.removeFromTop (22.0f);
    plot.removeFromBottom (8.0f);
    const float left = plot.getX();
    const float W    = plot.getWidth();
    const float top  = plot.getY();
    const float H    = plot.getHeight();

    constexpr int kCell  = 3;   // LCD pixel size (px)
    constexpr int kPitch = 4;   // pixel size + 1px grid gap
    const int cols = juce::jmax (1, juce::roundToInt (W / static_cast<float> (kPitch)));
    const int rows = juce::jmax (1, juce::roundToInt (H / static_cast<float> (kPitch)));

    auto cell = [&] (int c, int r)
    {
        const float x = left + static_cast<float> (c * kPitch);
        const float y = top  + static_cast<float> (r * kPitch);
        return juce::Rectangle<float> (x, y, static_cast<float> (kCell), static_cast<float> (kCell));
    };

    // Faint backdrop grid (every cell dimly lit).
    g.setColour (trackCol.withAlpha (0.12f));
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c)
            g.fillRect (cell (c, r));

    // Light one column: cells lo..hi dim (accent), the peak cell (targetRow) bright.
    auto lightColumn = [&] (int c, int targetRow, int lo, int hi)
    {
        g.setColour (accent.withAlpha (0.16f));
        for (int r = lo; r <= hi; ++r)
            g.fillRect (cell (c, r));
        g.setColour (accent);
        g.fillRect (cell (c, targetRow));
    };

    // ---- LFO waveform preview (previewMode_ == 1): bipolar, around the midline ----
    if (previewMode_ == 1)
    {
        const float sh = lastShape_ >= 0.0f ? lastShape_ : fetch (getShape_);
        const int shapeIdx = juce::jlimit (0, 3, juce::roundToInt (sh * 3.0f));   // 0..3: Tri/Sq/S&H/Ramp

        constexpr int kBlocksPerCycle = 8;
        constexpr int cycles = 2;
        const float periodFrac = 1.0f / static_cast<float> (cycles);

        // Stable S&H staircase (fixed seed so it does not flicker between frames).
        float shLevels[cycles * kBlocksPerCycle] = {};
        if (shapeIdx == 2)
        {
            uint32_t lcg = 0x9e3779b9u;
            for (int i = 0; i < cycles * kBlocksPerCycle; ++i)
            {
                lcg = lcg * 1664525u + 1013904223u;
                shLevels[i] = (lcg >> 8) * (1.0f / 16777216.0f) * 2.0f - 1.0f;
            }
        }

        auto lfoLevel = [&] (float xf) -> float   // xf 0..1 -> -1..1
        {
            const float ph = xf / periodFrac;
            const float f  = ph - std::floor (ph);
            switch (shapeIdx)
            {
                case 0:  return (f < 0.5f) ? (4.0f * f - 1.0f) : (3.0f - 4.0f * f);   // triangle
                case 1:  return (f < 0.5f) ? 1.0f : -1.0f;                            // square
                case 2:                                                                  // sample & hold
                {
                    const int cycle = juce::jlimit (0, cycles - 1, static_cast<int> (ph));
                    const int block = juce::jlimit (0, kBlocksPerCycle - 1, static_cast<int> (f * kBlocksPerCycle));
                    return shLevels[cycle * kBlocksPerCycle + block];
                }
                default: return 2.0f * f - 1.0f;                                       // ramp / saw
            }
        };

        // Render: each column's bipolar level lights cells from the midline to
        // the peak (a blocky waveform centred on the display).
        const int midRow  = (rows - 1) / 2;
        const int lastRow = rows - 1;
        for (int c = 0; c < cols; ++c)
        {
            const float xf = (static_cast<float> (c) + 0.5f) / static_cast<float> (cols);
            const float v  = juce::jlimit (-1.0f, 1.0f, lfoLevel (xf));
            int targetRow = juce::roundToInt ((1.0f - v) * 0.5f * static_cast<float> (lastRow));
            targetRow = juce::jlimit (0, lastRow, targetRow);
            lightColumn (c, targetRow, juce::jmin (midRow, targetRow), juce::jmax (midRow, targetRow));
        }

        g.setColour (textDim);
        g.setFont (lnf ? lnf->appFont (11.0f, juce::Font::plain)
                       : juce::Font (juce::FontOptions (11.0f)));
        g.drawText ("(LFO)",
                    bounds.reduced (9.0f, 4.0f).removeFromTop (16).removeFromRight (50),
                    juce::Justification::topRight);
        return;
    }

    // ---- ADSR envelope (previewMode_ == 0): unipolar, filled from the baseline ----
    const float a = lastA_ >= 0.0f ? lastA_ : fetch (getAttack_);
    const float d = lastD_ >= 0.0f ? lastD_ : fetch (getDecay_);
    const float s = lastS_ >= 0.0f ? lastS_ : fetch (getSustain_);
    const float r = lastR_ >= 0.0f ? lastR_ : fetch (getRelease_);

    // Segment widths: a small base so a 0 value is still visible, plus the
    // knob's contribution; the sustain plateau is a fixed middle portion.
    const float baseW = 0.06f, rangeW = 0.30f;
    const float wA = baseW + a * rangeW;
    const float wD = baseW + d * rangeW;
    const float wS = 0.16f;
    const float wR = baseW + r * rangeW;
    const float total = wA + wD + wS + wR;
    const float fracA = wA / total, fracD = wD / total, fracS = wS / total, fracR = wR / total;
    const float xEndA = fracA;
    const float xEndD = fracA + fracD;
    const float xEndS = fracA + fracD + fracS;

    // Envelope level (0..1) at a normalized x position (0..1), using the same
    // exponential attack/decay/release eases as the smooth curve did.
    auto envLevel = [&] (float xf) -> float
    {
        if (xf <= xEndA)
        {
            const float tt = fracA > 0.0f ? xf / fracA : 1.0f;
            return 1.0f - std::pow (1.0f - tt, 2.0f);                 // attack ease-out
        }
        if (xf <= xEndD)
        {
            const float tt = fracD > 0.0f ? (xf - xEndA) / fracD : 1.0f;
            return s + (1.0f - s) * std::pow (1.0f - tt, 2.0f);        // decay settle
        }
        if (xf <= xEndS)
            return s;                                                  // sustain plateau
        const float tt = fracR > 0.0f ? (xf - xEndS) / fracR : 1.0f;
        return s * std::pow (1.0f - tt, 2.0f);                         // release decay
    };

    // Render: each column's level becomes a column of lit LCD pixels filled from
    // the baseline up to the peak cell (the envelope shape as a blocky skyline).
    const int lastRow = rows - 1;
    for (int c = 0; c < cols; ++c)
    {
        const float xf = (static_cast<float> (c) + 0.5f) / static_cast<float> (cols);
        const float v  = juce::jlimit (0.0f, 1.0f, envLevel (xf));
        int targetRow = juce::roundToInt ((1.0f - v) * static_cast<float> (lastRow));
        targetRow = juce::jlimit (0, lastRow, targetRow);
        lightColumn (c, targetRow, targetRow, lastRow);
    }
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

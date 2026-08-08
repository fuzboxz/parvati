// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See FxSlotVisualizer.h.

#include "FxSlotVisualizer.h"

#include <cmath>

#include "ParvatiLookAndFeel.h"
#include "VectorTrace.h"
#include "dsp/fx/FxTypes.h"   // FxType (None..Chorus)

//==============================================================================
namespace
{
    // Sub-knob jitter epsilon for the change gates (matches the sibling
    // previews). The discrete FxType is gated on the resolved int (a snap).
    constexpr float kEps = 1.0f / 512.0f;

    // Resolve the FxType choice (0..1 normalized across None..Chorus) to its
    // integer index, clamped to the enum range.
    inline int typeIndex (float t)
    {
        return juce::jlimit (0, static_cast<int> (FxType::Count) - 1,
                             juce::roundToInt (juce::jlimit (0.0f, 1.0f, t)
                                               * static_cast<float> (static_cast<int> (FxType::Count) - 1)));
    }
}

//==============================================================================
FxSlotVisualizer::FxSlotVisualizer (Getter getType, Getter getP0, Getter getP1,
                                    Getter getP2, Getter getP3, Getter getDryWet)
    : getType_   (std::move (getType)),
      getP0_     (std::move (getP0)),
      getP1_     (std::move (getP1)),
      getP2_     (std::move (getP2)),
      getP3_     (std::move (getP3)),
      getDryWet_ (std::move (getDryWet))
{
    if (! getType_)   getType_   = [] { return 0.0f; };
    if (! getP0_)     getP0_     = [] { return 0.0f; };
    if (! getP1_)     getP1_     = [] { return 0.0f; };
    if (! getP2_)     getP2_     = [] { return 0.0f; };
    if (! getP3_)     getP3_     = [] { return 0.0f; };
    if (! getDryWet_) getDryWet_ = [] { return 0.0f; };

    juce::Component::setTitle ("FX Slot Visualizer");
    setDescription ("Per-slot effect graphic preview");

    // Seed the displayed values so the first paint (before the 30 Hz tick) shows
    // the current state rather than an empty panel.
    dispType_   = fetch (getType_);
    dispP0_     = fetch (getP0_);
    dispP1_     = fetch (getP1_);
    dispP2_     = fetch (getP2_);
    dispP3_     = fetch (getP3_);
    dispDryWet_ = fetch (getDryWet_);

    startTimerHz (30);
}

FxSlotVisualizer::~FxSlotVisualizer()
{
    stopTimer();
}

//==============================================================================
float FxSlotVisualizer::fetch (const Getter& f) const
{
    const float v = (f ? f() : 0.0f);
    return juce::jlimit (0.0f, 1.0f, std::isfinite (v) ? v : 0.0f);
}

void FxSlotVisualizer::timerCallback()
{
    const float t = fetch (getType_);
    const float p0 = fetch (getP0_);
    const float p1 = fetch (getP1_);
    const float p2 = fetch (getP2_);
    const float p3 = fetch (getP3_);
    const float dw = fetch (getDryWet_);

    // Track the live APVTS target EXACTLY so the preview is accurate under
    // automation (no smoothing lag). Detect a change vs the previously-shown
    // value to drive the repaint gate (eps gate: no constant repaint when idle).
    const bool changed = std::fabs (t  - dispType_)   > kEps
                      || std::fabs (p0 - dispP0_)     > kEps
                      || std::fabs (p1 - dispP1_)     > kEps
                      || std::fabs (p2 - dispP2_)     > kEps
                      || std::fabs (p3 - dispP3_)     > kEps
                      || std::fabs (dw - dispDryWet_) > kEps;

    dispType_   = t;
    dispP0_     = p0;
    dispP1_     = p1;
    dispP2_     = p2;
    dispP3_     = p3;
    dispDryWet_ = dw;

    // Chorus animates in real time (its sine phase advances every tick), so it
    // repaints continuously; every other type only repaints on a param/type
    // change.
    const bool animate = typeIndex (t) == static_cast<int> (FxType::Chorus);

    if (changed || animate)
        repaint();
}

//==============================================================================
float FxSlotVisualizer::wetAlpha (float wet) noexcept
{
    // Dry (0) reads dimmer, wet (1) reads vivid. A 0.42 floor keeps even a
    // fully-dry slot's trace legible.
    return 0.42f + 0.58f * juce::jlimit (0.0f, 1.0f, wet);
}

//==============================================================================
void FxSlotVisualizer::paint (juce::Graphics& g)
{
    auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel());
    const ParvatiTheme* t = lnf ? lnf->getTheme() : nullptr;

    const auto panelBg = t ? t->backgroundPanel  : juce::Colour (0xff24242e);
    const auto outline = t ? t->outline          : juce::Colour (0xff3c3c4a);
    const auto accent  = t ? t->accentSecondary  : juce::Colour (0xff7aa2ff);   // FX/bypass accent
    const auto dimText = t ? t->textSecondary    : juce::Colour (0xff9a9aa8);
    const auto gridCol = t ? t->divider.withAlpha (0.10f) : accent.withAlpha (0.06f);

    // Resolved trace hue: caller's category colour, else the live FX accent.
    const auto trace = (hasCategoryColour_ ? categoryColour_ : accent);

    const auto bounds = getLocalBounds().toFloat();
    g.setColour (panelBg);
    g.fillRect (bounds);

    // Faint clean grid backdrop (thin 1px lines) shared by every state.
    parvati::vectorTrace::drawGrid (g, bounds.reduced (0.5f), gridCol, 16.0f);

    g.setColour (outline);
    g.drawRect (bounds.reduced (0.5f), 1.0f);

    // Compact plot area (no title row: maximize the graphic).
    const auto plot = bounds.reduced (4.0f, 3.0f);

    // Resolve the current displayed params (-1 => first paint: fetch fresh).
    const float tN  = juce::jlimit (0.0f, 1.0f, dispType_   >= 0.0f ? dispType_   : fetch (getType_));
    const float p0  = juce::jlimit (0.0f, 1.0f, dispP0_     >= 0.0f ? dispP0_     : fetch (getP0_));
    const float p1  = juce::jlimit (0.0f, 1.0f, dispP1_     >= 0.0f ? dispP1_     : fetch (getP1_));
    const float p2  = juce::jlimit (0.0f, 1.0f, dispP2_     >= 0.0f ? dispP2_     : fetch (getP2_));
    const float p3  = juce::jlimit (0.0f, 1.0f, dispP3_     >= 0.0f ? dispP3_     : fetch (getP3_));
    const float wet = juce::jlimit (0.0f, 1.0f, dispDryWet_ >= 0.0f ? dispDryWet_ : fetch (getDryWet_));

    const int ti = typeIndex (tN);
    const auto passive = t ? t->textDisabled : dimText;

    switch (ti)
    {
        case static_cast<int> (FxType::GainPan):
            drawGainPan (g, plot, lnf, trace, dimText, p0, p1, wet);
            break;
        case static_cast<int> (FxType::Delay):
            drawDelay (g, plot, lnf, trace, dimText, p0, p1, p2, wet);
            break;
        case static_cast<int> (FxType::Reverb):
            drawReverb (g, plot, lnf, trace, accent, dimText, p0, p1, p2, p3, wet);
            break;
        case static_cast<int> (FxType::Chorus):
            drawChorus (g, plot, trace, p0, p1, wet);
            break;
        case static_cast<int> (FxType::None):
        default:
            drawNone (g, plot, lnf, passive, dimText);
            break;
    }
}

//==============================================================================
void FxSlotVisualizer::drawNone (juce::Graphics& g, juce::Rectangle<float> plot,
                                 ParvatiLookAndFeel* lnf,
                                 juce::Colour passive, juce::Colour dimText)
{
    // Empty-slot state: a dimmed passive rounded-rect outline centred in the
    // plot + a faint em-dash glyph — clearly inactive (uses textDisabled).
    const auto box = plot.withSizeKeepingCentre (juce::jmin (plot.getWidth()  * 0.7f, 90.0f),
                                                 juce::jmin (plot.getHeight() * 0.55f, 40.0f));
    g.setColour (passive.withAlpha (0.30f));
    g.drawRoundedRectangle (box, 4.0f, 1.0f);

    g.setColour (dimText.withAlpha (0.40f));
    g.setFont (lnf ? lnf->appFont (juce::jmin (16.0f, plot.getHeight() * 0.5f), juce::Font::plain)
                   : juce::Font (juce::FontOptions (juce::jmin (16.0f, plot.getHeight() * 0.5f))));
    g.drawText ("\xe2\x80\x94", plot, juce::Justification::centred);   // em-dash
}

//==============================================================================
void FxSlotVisualizer::drawGainPan (juce::Graphics& g, juce::Rectangle<float> plot,
                                    ParvatiLookAndFeel* lnf,
                                    juce::Colour trace, juce::Colour dimText,
                                    float gain, float pan, float wet)
{
    const float a = wetAlpha (wet);

    // ---- Right-edge vertical GAIN meter (filled from the bottom to `gain`) ----
    const float meterW = juce::jmax (3.0f, plot.getWidth() * 0.06f);
    auto meter = plot.removeFromRight (meterW + 4.0f).reduced (2.0f);
    g.setColour (trace.withAlpha (0.18f * a));
    g.drawRoundedRectangle (meter, 2.0f, 1.0f);
    auto fill = meter.withTop (meter.getY() + meter.getHeight() * (1.0f - gain));
    g.setColour (trace.withAlpha (0.85f * a));
    g.fillRoundedRectangle (fill, 2.0f);

    // ---- Lower pan track with a position dot ----
    auto track = plot.removeFromBottom (juce::jmin (plot.getHeight() * 0.42f, 22.0f));
    const float ty = track.getCentre().y;

    // Tick marks L / R + the track line.
    g.setColour (dimText.withAlpha (0.65f));
    g.setFont (lnf ? lnf->appFont (8.5f, juce::Font::plain) : juce::Font (juce::FontOptions (8.5f)));
    g.drawText ("L", track.removeFromLeft (10.0f), juce::Justification::centredLeft);
    g.drawText ("R", track.removeFromRight (10.0f), juce::Justification::centredRight);

    g.setColour (trace.withAlpha (0.25f * a));
    g.drawHorizontalLine (juce::roundToInt (ty), track.getX(), track.getRight());
    // Centre tick (pan = centre).
    g.setColour (trace.withAlpha (0.35f * a));
    g.drawVerticalLine (juce::roundToInt (track.getCentre().x), ty - 3.0f, ty + 3.0f);

    // Pan position dot: pan 0 => far left, 0.5 => centre, 1 => far right.
    const float dotX = track.getX() + pan * track.getWidth();
    g.setColour (trace.withAlpha (0.95f * a));
    g.fillEllipse (dotX - 3.5f, ty - 3.5f, 7.0f, 7.0f);
}

//==============================================================================
void FxSlotVisualizer::drawDelay (juce::Graphics& g, juce::Rectangle<float> plot,
                                  ParvatiLookAndFeel* lnf,
                                  juce::Colour trace, juce::Colour dimText,
                                  float time, float feedback, float spread, float wet)
{
    const float a   = wetAlpha (wet);
    const float midY = plot.getCentre().y;
    const float halfH = plot.getHeight() * 0.46f;

    // Source pulse on the left edge (the dry impulse entering the delay line).
    const float srcX = plot.getX() + 2.0f;
    g.setColour (trace.withAlpha (0.9f * a));
    g.drawVerticalLine (juce::roundToInt (srcX), midY - halfH, midY + halfH);

    // Tap spacing scales with TIME (more time => wider gaps => fewer taps).
    // Base gap covers ~1/6 of the plot at full time, shrinking toward 0.
    const float usableW = plot.getRight() - srcX - 4.0f;
    const float gap = juce::jmax (3.0f, usableW * (0.05f + 0.16f * time));

    // Feedback scales the per-tap amplitude decay (0 => dies instantly,
    // 1 => near-constant). Kept strictly < 1 so the train always decays.
    const float fb = 0.06f + 0.90f * feedback;

    // Stereo SPREAD offsets a fainter ghost row (R) to the right of the main (L)
    // train so width is visible (spread 0 => mono, trains overlap).
    const float spreadShift = spread * gap * 0.6f;

    // Draw the main (L) tap train + the ghost (R) train.
    auto drawTrain = [&] (float xShift, float alpha)
    {
        for (int k = 1; k <= 12; ++k)
        {
            const float x  = srcX + static_cast<float> (k) * gap + xShift;
            if (x > plot.getRight() - 1.0f)
                break;
            const float amp = halfH * std::pow (fb, static_cast<float> (k));
            if (amp < 0.75f)
                break;   // below ~1px: stop
            g.setColour (trace.withAlpha (juce::jlimit (0.0f, 1.0f, alpha * (0.35f + 0.65f * (amp / halfH)))));
            g.drawVerticalLine (juce::roundToInt (x), midY - amp, midY + amp);
        }
    };

    drawTrain (0.0f, 0.95f * a);
    if (spreadShift > 0.5f)
        drawTrain (spreadShift, 0.45f * a);

    // Left/Right row hint labels.
    g.setColour (dimText.withAlpha (0.55f));
    g.setFont (lnf ? lnf->appFont (8.0f, juce::Font::plain) : juce::Font (juce::FontOptions (8.0f)));
    g.drawText ("delay", plot.withTrimmedRight (2).withTrimmedBottom (1), juce::Justification::bottomRight);
}

//==============================================================================
void FxSlotVisualizer::drawReverb (juce::Graphics& g, juce::Rectangle<float> plot,
                                   ParvatiLookAndFeel* lnf,
                                   juce::Colour trace, juce::Colour accent,
                                   juce::Colour dimText,
                                   float size, float damp, float level, float width, float wet)
{
    const float a = wetAlpha (wet);

    // Impulse-response tail: an exponential decay envelope across the plot.
    // SIZE lengthens the visible tail (slower decay), DAMP steepens it. The
    // effective decay rate blends the two so both knobs move the curve.
    const float decay = juce::jlimit (0.4f, 9.0f, (0.6f + damp * 5.0f) / (0.25f + size * 1.4f));

    auto envLevel = [&] (float xf) -> float
    {
        return std::exp (-xf * decay);
    };

    // Smooth unipolar vector trace + translucent gradient fill (the reverb tail).
    const int sampleCount = juce::jmax (48, juce::roundToInt (plot.getWidth() * 2.0f));
    parvati::vectorTrace::render (g, plot, sampleCount, envLevel,
                                  trace.withMultipliedAlpha (a),
                                  parvati::vectorTrace::Mode::unipolar,
                                  false, 1.5f, 0.14f * a);

    // WET-LEVEL band: a faint horizontal line at the reverb wet-level height.
    const float levelY = plot.getY() + (1.0f - level) * plot.getHeight();
    g.setColour (accent.withAlpha (0.45f * a));
    g.drawHorizontalLine (juce::roundToInt (levelY), plot.getX(), plot.getRight());

    // STEREO-WIDTH bracket at the top: a small bracket spanning `width` of the
    // plot (narrow => mono-ish, full => wide).
    const float bw = juce::jmax (8.0f, plot.getWidth() * (0.12f + 0.8f * width));
    const float bx = plot.getCentre().x - bw * 0.5f;
    const float by = plot.getY() + 1.5f;
    g.setColour (dimText.withAlpha (0.6f));
    g.drawHorizontalLine (juce::roundToInt (by), bx, bx + bw);
    g.drawVerticalLine (juce::roundToInt (bx), by, by + 3.0f);
    g.drawVerticalLine (juce::roundToInt (bx + bw), by, by + 3.0f);

    g.setFont (lnf ? lnf->appFont (8.0f, juce::Font::plain) : juce::Font (juce::FontOptions (8.0f)));
    g.setColour (dimText.withAlpha (0.55f));
    g.drawText ("reverb", plot.withTrimmedRight (2).withTrimmedBottom (1), juce::Justification::bottomRight);
}

//==============================================================================
void FxSlotVisualizer::drawChorus (juce::Graphics& g, juce::Rectangle<float> plot,
                                   juce::Colour trace, float rate, float depth, float wet)
{
    const float a = wetAlpha (wet);

    // Two delayed traces (L/R) that wobble with a sine whose RATE scales with
    // `rate` and whose AMPLITUDE scales with `depth`. The phase advances in real
    // time so the graphic animates (the 30 Hz timer drives the repaints).
    const float rateHz = 0.25f + rate * 6.0f;                 // ~0.25..6.25 Hz
    const float amp    = (0.18f + 0.62f * depth);             // bipolar amplitude 0..1
    const float cycles = 3.0f;                                // wobbles across the plot
    const float phase  = static_cast<float> (juce::Time::getMillisecondCounter()) * 0.001f
                       * rateHz * juce::MathConstants<float>::twoPi;

    auto wob = [&] (float xf, float phaseOffset) -> float
    {
        return amp * std::sin (juce::MathConstants<float>::twoPi * cycles * xf + phase + phaseOffset);
    };

    // Stroke two smooth bipolar traces (L solid, R phase-shifted); NO area fill
    // (two overlapping fills would clutter the compact canvas).
    const int sampleCount = juce::jmax (64, juce::roundToInt (plot.getWidth() * 2.0f));

    {
        juce::Path pL = parvati::vectorTrace::buildTrace (plot, sampleCount,
                          [&] (float xf) { return wob (xf, 0.0f); },
                          parvati::vectorTrace::Mode::bipolar, false);
        g.setColour (trace.withAlpha (0.9f * a));
        g.strokePath (pL, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    {
        juce::Path pR = parvati::vectorTrace::buildTrace (plot, sampleCount,
                          [&] (float xf) { return wob (xf, juce::MathConstants<float>::pi); },
                          parvati::vectorTrace::Mode::bipolar, false);
        g.setColour (trace.withAlpha (0.45f * a));
        g.strokePath (pR, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Centre midline reference so the bipolar centre reads.
    g.setColour (trace.withAlpha (0.18f * a));
    g.drawHorizontalLine (juce::roundToInt (plot.getCentre().y), plot.getX(), plot.getRight());
}

//==========================================================================
std::unique_ptr<juce::AccessibilityHandler> FxSlotVisualizer::createAccessibilityHandler()
{
    // Mirrors OscPreviewDisplay/FilterResponseDisplay: expose the canvas as an
    // accessibility group (a labelled decorative graphic).
    return std::make_unique<juce::AccessibilityHandler> (*this,
            juce::AccessibilityRole::group);
}

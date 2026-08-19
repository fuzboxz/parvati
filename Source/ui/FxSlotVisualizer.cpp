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

    // Deterministic 0..1 hash for paint-time scatter (Diffuser smear / Clouds-reverb
    // diffusion texture) — reproducible + allocation-free (no heap, no state).
    inline float hash01 (int n) noexcept
    {
        const float x = std::sin (static_cast<float> (n) * 127.1f + 311.7f) * 43758.5453f;
        return x - std::floor (x);
    }
}

//==============================================================================
FxSlotVisualizer::FxSlotVisualizer (Getter getType, Getter getP0, Getter getP1,
                                    Getter getP2, Getter getP3, Getter getP4, Getter getDryWet)
    : getType_   (std::move (getType)),
      getP0_     (std::move (getP0)),
      getP1_     (std::move (getP1)),
      getP2_     (std::move (getP2)),
      getP3_     (std::move (getP3)),
      getP4_     (std::move (getP4)),
      getDryWet_ (std::move (getDryWet))
{
    if (! getType_)   getType_   = [] { return 0.0f; };
    if (! getP0_)     getP0_     = [] { return 0.0f; };
    if (! getP1_)     getP1_     = [] { return 0.0f; };
    if (! getP2_)     getP2_     = [] { return 0.0f; };
    if (! getP3_)     getP3_     = [] { return 0.0f; };
    if (! getP4_)     getP4_     = [] { return 0.0f; };
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
    dispP4_     = fetch (getP4_);
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
    const float t  = fetch (getType_);
    const float p0 = fetch (getP0_);
    const float p1 = fetch (getP1_);
    const float p2 = fetch (getP2_);
    const float p3 = fetch (getP3_);
    const float p4 = fetch (getP4_);
    const float dw = fetch (getDryWet_);

    // Track the live APVTS target EXACTLY so the preview is accurate under
    // automation (no smoothing lag). Detect a change vs the previously-shown
    // value to drive the repaint gate (eps gate: no constant repaint when idle).
    const bool changed = std::fabs (t  - dispType_)   > kEps
                      || std::fabs (p0 - dispP0_)     > kEps
                      || std::fabs (p1 - dispP1_)     > kEps
                      || std::fabs (p2 - dispP2_)     > kEps
                      || std::fabs (p3 - dispP3_)     > kEps
                      || std::fabs (p4 - dispP4_)     > kEps
                      || std::fabs (dw - dispDryWet_) > kEps;

    dispType_   = t;
    dispP0_     = p0;
    dispP1_     = p1;
    dispP2_     = p2;
    dispP3_     = p3;
    dispP4_     = p4;
    dispDryWet_ = dw;

    // Every type is STATIC: the repaint gate fires only on a param/type change
    // (no constant repaint when idle).
    if (changed)
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
        case static_cast<int> (FxType::Diffuser):
            drawDiffuser (g, plot, lnf, trace, dimText, 1.0f, wet);   // amount hardcoded full-wet post-dedup; Dry/Wet gates visibility
            break;
        case static_cast<int> (FxType::PitchShifter):
            drawPitchShifter (g, plot, lnf, trace, dimText, p0, p1, wet);
            break;
        case static_cast<int> (FxType::Reverb):
            drawReverb (g, plot, lnf, trace, accent, dimText, 1.0f, p2, p3, p1, wet);   // amount=full-wet (dedup); time=p2, tone=p3, diffusion=p1 (signal-path reorder)
            break;
        case static_cast<int> (FxType::LoopingDelay):
            drawLoopingDelay (g, plot, lnf, trace, accent, dimText, p0, p1, p2, p3, wet);
            break;
        case static_cast<int> (FxType::WSOLAStretch):
            drawWSOLAStretch (g, plot, lnf, trace, dimText, p0, p1, p2, wet);
            break;
        case static_cast<int> (FxType::Spectral):
            drawSpectral (g, plot, lnf, trace, accent, dimText, p0, p1, p2, p3, wet);
            break;
        case static_cast<int> (FxType::Wavefolder):
            drawWavefolder (g, plot, lnf, trace, dimText, p1, p2, wet);   // fold=p1, bias=p2 (signal-path reorder: Drive,Fold,Bias,Tone)
            break;
        case static_cast<int> (FxType::FrequencyShifter):
            drawFrequencyShifter (g, plot, lnf, trace, accent, dimText, p0, p2, p3, wet);   // shift=p0, feedback=p2, spread=p3 (signal-path reorder: Shift,Shape,Feedback,Spread)
            break;
        case static_cast<int> (FxType::RingModulator):
            drawRingModulator (g, plot, lnf, trace, accent, dimText, p0, p1, p2, wet);
            break;
        case static_cast<int> (FxType::Resonator):
            drawResonator (g, plot, lnf, trace, accent, dimText, p0, p1, p2, p3, wet);
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
    // CharPointer_UTF8: the em-dash (\xe2\x80\x94) makes this a UTF-8 literal; the
    // implicit juce::String conversion asserts (ASCII validity) every paint.
    g.drawText (juce::CharPointer_UTF8 ("\xe2\x80\x94"), plot, juce::Justification::centred);   // em-dash
}

//==============================================================================
void FxSlotVisualizer::drawDiffuser (juce::Graphics& g, juce::Rectangle<float> plot,
                                     ParvatiLookAndFeel* lnf,
                                     juce::Colour trace, juce::Colour dimText,
                                     float amount, float wet)
{
    const float a     = wetAlpha (wet);
    const float midY  = plot.getCentre().y;
    const float halfH = plot.getHeight() * 0.46f;

    // Dry impulse entering the diffusion network (left edge), like the Delay.
    const float srcX = plot.getX() + 2.0f;
    g.setColour (trace.withAlpha (0.9f * a));
    g.drawVerticalLine (juce::roundToInt (srcX), midY - halfH, midY + halfH);

    // A DIFFUSION SMEAR: a deterministic cloud of translucent vertical streaks
    // spread across the whole plot (energy scattered, NOT a decaying train like
    // Delay). The streak COUNT and per-streak alpha grow with Amount (0 => just
    // the impulse, 1 => a dense smeared cloud).
    const int   nStreaks = juce::roundToInt (amount * 16.0f);
    const float baseA    = 0.12f + 0.55f * amount;
    for (int k = 0; k < nStreaks; ++k)
    {
        const float xf = 0.06f + 0.92f * hash01 (k * 7 + 3);            // spread 0.06..0.98
        const float h  = halfH * (0.28f + 0.72f * hash01 (k * 13 + 5)); // varied heights
        const float x  = plot.getX() + xf * plot.getWidth();
        const float aa = baseA * (0.4f + 0.6f * hash01 (k * 19 + 1)) * a;
        g.setColour (trace.withAlpha (juce::jlimit (0.0f, 1.0f, aa)));
        g.drawVerticalLine (juce::roundToInt (x), midY - h, midY + h);
    }

    g.setColour (dimText.withAlpha (0.55f));
    g.setFont (lnf ? lnf->appFont (8.0f, juce::Font::plain) : juce::Font (juce::FontOptions (8.0f)));
    g.drawText ("diffuse", plot.withTrimmedRight (2).withTrimmedBottom (1), juce::Justification::bottomRight);
}

void FxSlotVisualizer::drawWavefolder (juce::Graphics& g, juce::Rectangle<float> plot,
                                       ParvatiLookAndFeel* lnf,
                                       juce::Colour trace, juce::Colour dimText,
                                       float fold, float bias, float wet)
{
    const float a     = wetAlpha (wet);
    const float midY  = plot.getCentre().y;
    const float halfH = plot.getHeight() * 0.46f;
    const float x0    = plot.getX();
    const float w     = plot.getWidth();

    // Parametric approximation of the bipolar-fold TRANSFER CURVE: input x sweeps
    // left..right; the output zig-zags (folds) more as Fold grows, skewed by Bias.
    auto foldOf = [] (float x) {
        // reflect the real line into [-1, 1] (a clean bipolar fold / triangle wave)
        x = std::fmod (x, 4.0f);
        if (x < 0.0f) x += 4.0f;                 // x in [0,4)
        if (x > 3.0f) return x - 4.0f;           // 3..4 -> -1..0
        if (x > 1.0f) return 2.0f - x;           // 1..3 -> 1..-1
        return x;                                // 0..1 -> 0..1
    };
    const float scale   = 1.0f + fold * 6.0f;     // more folds as Fold grows
    const float biasOff = (bias - 0.5f) * 2.0f;   // horizontal skew

    juce::Path p;
    const int N = juce::jmax (1, juce::roundToInt (w));
    for (int i = 0; i <= N; ++i)
    {
        const float xn = -1.0f + 2.0f * static_cast<float> (i) / static_cast<float> (N);
        const float yn = foldOf (xn * scale + biasOff);
        const float px = x0 + static_cast<float> (i);
        const float py = midY - yn * halfH;
        if (i == 0) p.startNewSubPath (px, py);
        else        p.lineTo (px, py);
    }
    g.setColour (trace.withAlpha (0.95f * a));
    g.strokePath (p, juce::PathStrokeType (1.4f));

    // faint identity (y = x) reference line.
    g.setColour (dimText.withAlpha (0.22f * a));
    g.drawLine (x0, midY + halfH, x0 + w, midY - halfH, 0.8f);

    g.setColour (dimText.withAlpha (0.55f));
    g.setFont (lnf ? lnf->appFont (8.0f, juce::Font::plain) : juce::Font (juce::FontOptions (8.0f)));
    g.drawText ("fold", plot.withTrimmedRight (2).withTrimmedBottom (1), juce::Justification::bottomRight);
}

//==============================================================================
void FxSlotVisualizer::drawPitchShifter (juce::Graphics& g, juce::Rectangle<float> plot,
                                         ParvatiLookAndFeel* lnf,
                                         juce::Colour trace, juce::Colour dimText,
                                         float ratio, float size, float wet)
{
    const float a = wetAlpha (wet);

    // A dual-tap pitch shifter: two parallel signal traces (the two delayed
    // taps) offset vertically by the pitch AMOUNT (|ratio-0.5|*2; 0.5 = unison
    // => the taps overlap on the centre line). They span a window whose width
    // scales with Size (small size => a short concentrated window).
    const float shift = std::fabs (ratio - 0.5f) * 2.0f;            // 0..1 pitch magnitude
    const float dcOff = shift * 0.46f;                              // bipolar DC offset per tap
    const float winW  = plot.getWidth() * (0.32f + 0.68f * size);
    const auto  win   = plot.withSizeKeepingCentre (winW, plot.getHeight());

    const float amp    = 0.10f + 0.16f * (1.0f - shift);            // less wiggle at extreme shift
    const float cycles = 3.5f;                                     // a few signal cycles across the window
    const int   sampleCount = juce::jmax (48, juce::roundToInt (win.getWidth() * 2.0f));

    auto drawTap = [&] (float dc, float ph, float alpha)
    {
        auto levelAt = [&, dc, ph] (float xf) -> float
        {
            return juce::jlimit (-1.0f, 1.0f,
                                 dc + amp * std::sin (juce::MathConstants<float>::twoPi * cycles * xf + ph));
        };
        juce::Path p = parvati::vectorTrace::buildTrace (win, sampleCount, levelAt,
                                                         parvati::vectorTrace::Mode::bipolar, false);
        g.setColour (trace.withAlpha (alpha * a));
        g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    };
    drawTap (+dcOff, 0.0f, 0.9f);
    drawTap (-dcOff, juce::MathConstants<float>::pi, 0.5f);

    // Centre reference line so the vertical offset reads.
    g.setColour (trace.withAlpha (0.18f * a));
    g.drawHorizontalLine (juce::roundToInt (plot.getCentre().y), plot.getX(), plot.getRight());

    g.setColour (dimText.withAlpha (0.55f));
    g.setFont (lnf ? lnf->appFont (8.0f, juce::Font::plain) : juce::Font (juce::FontOptions (8.0f)));
    g.drawText ("pitch", plot.withTrimmedRight (2).withTrimmedBottom (1), juce::Justification::bottomRight);
}

//==============================================================================
void FxSlotVisualizer::drawReverb (juce::Graphics& g, juce::Rectangle<float> plot,
                                         ParvatiLookAndFeel* lnf,
                                         juce::Colour trace, juce::Colour accent, juce::Colour dimText,
                                         float amount, float time, float tone, float diffusion, float wet)
{
    const float a = wetAlpha (wet);

    // Decaying impulse-response TAIL (Reverb-style): Time lengthens it, Tone (LP)
    // brightens it (a brighter tank rings longer => slower decay). Amount scales
    // the tail's peak vividness.
    const float decay = juce::jlimit (0.4f, 9.0f, (0.6f + (1.0f - tone) * 4.0f) / (0.25f + time * 1.4f));
    auto envLevel = [&] (float xf) -> float { return amount * std::exp (-xf * decay); };
    const int sampleCount = juce::jmax (48, juce::roundToInt (plot.getWidth() * 2.0f));
    parvati::vectorTrace::render (g, plot, sampleCount, envLevel,
                                  trace.withMultipliedAlpha (a),
                                  parvati::vectorTrace::Mode::unipolar,
                                  false, 1.5f, 0.14f * a);

    // Diffusion SMEAR overlay: a faint cloud of vertical streaks clustered at the
    // tail's start (the input diffuser) whose DENSITY scales with Diffusion.
    const float midY  = plot.getCentre().y;
    const float halfH = plot.getHeight() * 0.46f;
    const int   nStreaks = juce::roundToInt (diffusion * 10.0f);
    for (int k = 0; k < nStreaks; ++k)
    {
        const float xf = 0.04f + 0.30f * hash01 (k * 7 + 2);          // clustered near the start
        const float h  = halfH * (0.20f + 0.50f * hash01 (k * 11 + 4)) * amount;
        const float x  = plot.getX() + xf * plot.getWidth();
        g.setColour (accent.withAlpha (0.10f * diffusion * a));
        g.drawVerticalLine (juce::roundToInt (x), midY - h, midY + h);
    }

    g.setColour (dimText.withAlpha (0.55f));
    g.setFont (lnf ? lnf->appFont (8.0f, juce::Font::plain) : juce::Font (juce::FontOptions (8.0f)));
    g.drawText ("clouds", plot.withTrimmedRight (2).withTrimmedBottom (1), juce::Justification::bottomRight);
}

//==============================================================================
void FxSlotVisualizer::drawLoopingDelay (juce::Graphics& g, juce::Rectangle<float> plot,
                                         ParvatiLookAndFeel* lnf,
                                         juce::Colour trace, juce::Colour accent, juce::Colour dimText,
                                         float position, float size, float pitch, float freeze, float wet)
{
    const float a      = wetAlpha (wet);
    const bool  frozen = freeze >= 0.5f;
    const float midY   = plot.getCentre().y;

    // Loop-buffer RAIL: a faint line spanning the plot (the recording buffer).
    g.setColour (trace.withAlpha (0.22f * a));
    g.drawHorizontalLine (juce::roundToInt (midY), plot.getX(), plot.getRight());

    // The loop WINDOW: width scales with Size, horizontal position with Position
    // (it slides L<->R). Filled translucent; accent-bordered when frozen.
    const float winW = juce::jmax (10.0f, plot.getWidth() * (0.14f + 0.5f * size));
    const float lo = plot.getX() + winW * 0.5f;
    const float hi = plot.getRight() - winW * 0.5f;
    const float cx = lo + (hi - lo) * juce::jlimit (0.0f, 1.0f, position);
    const auto  win = juce::Rectangle<float> (cx - winW * 0.5f, plot.getY() + 2.0f, winW, plot.getHeight() - 4.0f);
    g.setColour (trace.withAlpha ((frozen ? 0.30f : 0.18f) * a));
    g.fillRoundedRectangle (win, 3.0f);
    g.setColour ((frozen ? accent : trace).withAlpha (0.75f * a));
    g.drawRoundedRectangle (win, 3.0f, 1.0f);

    // PLAYHEAD dot at the window's left edge.
    g.setColour ((frozen ? accent : trace).withAlpha (0.95f * a));
    g.fillEllipse (cx - winW * 0.5f - 2.5f, midY - 2.5f, 5.0f, 5.0f);

    // PITCH: a diagonal stroke inside the window whose slope tracks the pitch
    // shift (rising => higher playback, falling => lower).
    const float slope = (pitch - 0.5f) * 2.0f;   // -1..1
    g.setColour (trace.withAlpha (0.5f * a));
    juce::Path sl;
    sl.startNewSubPath (win.getX() + 2.0f, midY + slope * win.getHeight() * 0.32f);
    sl.lineTo (win.getRight() - 2.0f, midY - slope * win.getHeight() * 0.32f);
    g.strokePath (sl, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // FROZEN indicator: a small locked-square glyph at the window's top-right.
    if (frozen)
    {
        constexpr float gs = 5.0f;
        const auto glyph = juce::Rectangle<float> (win.getRight() - gs - 2.0f, win.getY() + 2.0f, gs, gs);
        g.setColour (accent.withAlpha (0.9f * a));
        g.drawRoundedRectangle (glyph, 1.0f, 1.0f);
    }

    g.setColour (dimText.withAlpha (0.55f));
    g.setFont (lnf ? lnf->appFont (8.0f, juce::Font::plain) : juce::Font (juce::FontOptions (8.0f)));
    g.drawText ("loop", plot.withTrimmedRight (2).withTrimmedBottom (1), juce::Justification::bottomRight);
}

//==============================================================================
void FxSlotVisualizer::drawWSOLAStretch (juce::Graphics& g, juce::Rectangle<float> plot,
                                         ParvatiLookAndFeel* lnf,
                                         juce::Colour trace, juce::Colour dimText,
                                         float pitch, float position, float size, float wet)
{
    const float a = wetAlpha (wet);

    // Overlapping Hann-window GRAIN bumps: the count grows with Size (more grains
    // => more overlap => more stretch). Position slides the cluster, Pitch tilts
    // the envelope (rising => higher). Rendered as one summed unipolar trace.
    const int   nGrains = juce::jmax (2, juce::roundToInt (2.0f + size * 5.0f));
    const float spacing = 1.0f / static_cast<float> (nGrains);
    const float gw      = spacing * 1.7f;                          // overlap factor
    const float offset  = (position - 0.5f) * spacing;             // cluster slide
    const float tilt    = (pitch - 0.5f) * 2.0f;                   // -1..1

    auto hann = [] (float t)   // Hann window, 0 outside [0,1].
    {
        return (t > 0.0f && t < 1.0f) ? (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * t)) : 0.0f;
    };

    auto levelAt = [&] (float xf) -> float
    {
        float sum = 0.0f;
        for (int i = 0; i < nGrains; ++i)
        {
            const float centre = (static_cast<float> (i) + 0.5f) * spacing + offset;
            sum += hann ((xf - centre) / gw + 0.5f);
        }
        const float ramp = 0.55f + 0.45f * (1.0f + tilt * (xf - 0.5f));   // pitch tilt
        return juce::jlimit (0.0f, 1.0f, sum * ramp);
    };

    const int sampleCount = juce::jmax (64, juce::roundToInt (plot.getWidth() * 2.0f));
    parvati::vectorTrace::render (g, plot, sampleCount, levelAt,
                                  trace.withMultipliedAlpha (a),
                                  parvati::vectorTrace::Mode::unipolar,
                                  false, 1.5f, 0.16f * a);

    g.setColour (dimText.withAlpha (0.55f));
    g.setFont (lnf ? lnf->appFont (8.0f, juce::Font::plain) : juce::Font (juce::FontOptions (8.0f)));
    g.drawText ("stretch", plot.withTrimmedRight (2).withTrimmedBottom (1), juce::Justification::bottomRight);
}

//==============================================================================
void FxSlotVisualizer::drawSpectral (juce::Graphics& g, juce::Rectangle<float> plot,
                                     ParvatiLookAndFeel* lnf,
                                     juce::Colour trace, juce::Colour accent, juce::Colour dimText,
                                     float pitch, float warp, float position, float blur, float wet)
{
    const float a = wetAlpha (wet);
    constexpr int kBins = 28;
    const float barSlot = plot.getWidth() / static_cast<float> (kBins);
    const float barW    = juce::jmax (2.0f, barSlot * 0.66f);

    // A deterministic decaying magnitude shape with seeded formant peaks.
    auto baseMag = [] (float idx)
    {
        const float decay = std::exp (-idx / static_cast<float> (kBins) * 2.4f);
        return decay * (0.55f + 0.45f * std::pow (std::sin (idx * 1.27f + 0.8f), 2.0f));
    };

    const float pitchShift = (pitch - 0.5f) * static_cast<float> (kBins);  // bins shifted by pitch
    const float warpPow    = (warp - 0.5f) * 2.4f;                         // <0.5 emphasize lows, >0.5 highs
    const float blurAmt    = juce::jlimit (0.0f, 1.0f, blur);

    for (int i = 0; i < kBins; ++i)
    {
        // PITCH shifts the spectrum content (sample a neighbour bin).
        const float idx = juce::jlimit (0.0f, static_cast<float> (kBins - 1),
                                        static_cast<float> (i) + pitchShift);
        float m = baseMag (idx);
        // WARP skews the distribution (tilt magnitudes low<->high).
        m *= std::pow ((static_cast<float> (i) + 1.0f) / static_cast<float> (kBins), warpPow);
        // POSITION pans a moving spectral highlight (a Gaussian peak).
        const float d = (static_cast<float> (i) + 0.5f) / static_cast<float> (kBins) - position;
        m += 0.5f * std::exp (-d * d * 60.0f);
        // BLUR compresses toward the mean (softens contrast / smears edges).
        m = 0.18f + (1.0f - blurAmt * 0.7f) * (m - 0.18f);
        m = juce::jlimit (0.0f, 1.0f, m);

        const float h = m * plot.getHeight() * 0.92f;
        const float x = plot.getX() + static_cast<float> (i) * barSlot + (barSlot - barW) * 0.5f;
        const auto bar = juce::Rectangle<float> (x, plot.getBottom() - h, barW, h);
        g.setColour (trace.withAlpha ((0.35f + 0.55f * m) * a));
        g.fillRoundedRectangle (bar, blurAmt * barW * 0.5f);   // blur rounds the tops
    }

    // A faint position marker line at the highlight pan.
    const float px = plot.getX() + position * plot.getWidth();
    g.setColour (accent.withAlpha (0.35f * a));
    g.drawVerticalLine (juce::roundToInt (px), plot.getY(), plot.getBottom());

    g.setColour (dimText.withAlpha (0.55f));
    g.setFont (lnf ? lnf->appFont (8.0f, juce::Font::plain) : juce::Font (juce::FontOptions (8.0f)));
    g.drawText ("spectral", plot.withTrimmedRight (2).withTrimmedBottom (1), juce::Justification::bottomRight);
}

//==========================================================================
// FxFrequencyShifter — two sideband sines (up/down) with a shift arrow.
void FxSlotVisualizer::drawFrequencyShifter (juce::Graphics& g, juce::Rectangle<float> plot,
                                             ParvatiLookAndFeel* lnf,
                                             juce::Colour trace, juce::Colour accent, juce::Colour dimText,
                                             float shift, float feedback, float spread, float wet)
{
    const float a     = wetAlpha (wet);
    const float midY  = plot.getCentre().y;
    const float halfH = plot.getHeight() * 0.40f;
    const float x0    = plot.getX();
    const float w     = plot.getWidth();
    const int   N     = juce::jmax (2, juce::roundToInt (w));

    // shift 0.5 = 0 Hz; bipolar signed offset drives the up/down sines apart.
    const float sOff  = (shift - 0.5f) * 6.0f;
    const float fbAmp = 0.5f + feedback * 0.6f;            // feedback grows the lobes
    const float spr   = juce::jlimit (0.0f, 1.0f, spread); // 0..1: blend toward the opposite sideband

    auto sine = [] (float ph) { return std::sin (ph); };
    juce::Path upP, downP;
    for (int i = 0; i <= N; ++i)
    {
        const float t = static_cast<float> (i) / static_cast<float> (N);
        const float ph = t * 2.0f * 3.14159265f * (3.0f + sOff);
        const float yU = sine (ph) * fbAmp;
        const float yD = sine (-ph) * fbAmp * (0.4f + 0.6f * spr);   // down sideband, grows with spread
        const float px = x0 + static_cast<float> (i);
        if (i == 0) { upP.startNewSubPath (px, midY - yU * halfH); downP.startNewSubPath (px, midY + yD * halfH); }
        else        { upP.lineTo (px, midY - yU * halfH); downP.lineTo (px, midY + yD * halfH); }
    }
    g.setColour (accent.withAlpha (0.92f * a));
    g.strokePath (upP, juce::PathStrokeType (1.4f));
    g.setColour (trace.withAlpha (0.75f * a));
    g.strokePath (downP, juce::PathStrokeType (1.2f));

    // centre baseline + a shift-direction arrow (left for -, right for +).
    g.setColour (dimText.withAlpha (0.20f * a));
    g.drawLine (x0, midY, x0 + w, midY, 0.7f);
    const float ax = x0 + w * 0.5f;
    const float adir = shift >= 0.5f ? 1.0f : -1.0f;
    g.setColour (dimText.withAlpha (0.6f * a));
    g.drawLine (ax - 5.0f * adir, midY, ax + 5.0f * adir, midY, 1.0f);
    g.drawLine (ax + 5.0f * adir, midY, ax + 2.0f * adir, midY - 2.5f, 1.0f);
    g.drawLine (ax + 5.0f * adir, midY, ax + 2.0f * adir, midY + 2.5f, 1.0f);

    g.setColour (dimText.withAlpha (0.55f));
    g.setFont (lnf ? lnf->appFont (8.0f, juce::Font::plain) : juce::Font (juce::FontOptions (8.0f)));
    g.drawText ("shift", plot.withTrimmedRight (2).withTrimmedBottom (1), juce::Justification::bottomRight);
}

//==========================================================================
// FxRingModulator — carrier x signal sines with a ring-mod ("x") node.
void FxSlotVisualizer::drawRingModulator (juce::Graphics& g, juce::Rectangle<float> plot,
                                          ParvatiLookAndFeel* lnf,
                                          juce::Colour trace, juce::Colour accent, juce::Colour dimText,
                                          float carrier, float shape, float amount, float wet)
{
    const float a     = wetAlpha (wet);
    const float midY  = plot.getCentre().y;
    const float halfH = plot.getHeight() * 0.40f;
    const float x0    = plot.getX();
    const float w     = plot.getWidth();
    const int   N     = juce::jmax (2, juce::roundToInt (w));

    const float cFreq = 4.0f + carrier * 10.0f;       // carrier cycles across the plot
    const float sFreq = 2.0f;                          // signal sine
    const float amt   = juce::jlimit (0.0f, 1.0f, amount);
    const float shp   = juce::jlimit (0.0f, 1.0f, shape);

    auto sig = [shp] (float ph)
    {
        // sine -> richer harmonics as Shape grows (parametric, not a real wavetable)
        const float s = std::sin (ph);
        return s + shp * 0.30f * std::sin (3.0f * ph);
    };

    // carrier trace (accent) + the ring-modulated product (trace, the carrier x signal envelope).
    juce::Path cP, prodP;
    for (int i = 0; i <= N; ++i)
    {
        const float t = static_cast<float> (i) / static_cast<float> (N);
        const float pc = t * 2.0f * 3.14159265f * cFreq;
        const float ps = t * 2.0f * 3.14159265f * sFreq;
        const float carrierY = 0.5f * std::sin (pc);
        const float prodY     = 0.85f * amt * sig (ps) * std::sin (pc);   // product (ring mod)
        const float px = x0 + static_cast<float> (i);
        if (i == 0) { cP.startNewSubPath (px, midY - carrierY * halfH); prodP.startNewSubPath (px, midY - prodY * halfH); }
        else        { cP.lineTo (px, midY - carrierY * halfH); prodP.lineTo (px, midY - prodY * halfH); }
    }
    g.setColour (accent.withAlpha (0.55f * a));
    g.strokePath (cP, juce::PathStrokeType (1.0f));
    g.setColour (trace.withAlpha (0.92f * a));
    g.strokePath (prodP, juce::PathStrokeType (1.4f));

    // a small ring-mod "x" node at the centre to convey the multiplication.
    const float cx = x0 + w * 0.5f;
    g.setColour (dimText.withAlpha (0.7f * a));
    const float r = 3.0f;
    g.drawLine (cx - r, midY - r, cx + r, midY + r, 1.0f);
    g.drawLine (cx - r, midY + r, cx + r, midY - r, 1.0f);

    g.setColour (dimText.withAlpha (0.55f));
    g.setFont (lnf ? lnf->appFont (8.0f, juce::Font::plain) : juce::Font (juce::FontOptions (8.0f)));
    g.drawText ("ring", plot.withTrimmedRight (2).withTrimmedBottom (1), juce::Justification::bottomRight);
}

//==========================================================================
void FxSlotVisualizer::drawResonator (juce::Graphics& g, juce::Rectangle<float> plot,
                                     ParvatiLookAndFeel* lnf,
                                     juce::Colour trace, juce::Colour accent, juce::Colour dimText,
                                     float pitch, float decay, float bright, float timbre, float wet)
{
    const float a     = wetAlpha (wet);
    const float midY  = plot.getCentre().y;
    const float halfH = plot.getHeight() * 0.42f;
    const float x0    = plot.getX();
    const float w     = plot.getWidth();

    // The resonator is a bank of tuned band-pass filters. Visualize it as a
    // comb of vertical "modal" bars: the fundamental + its partials. Higher
    // Pitch packs more partials into the plot; Timbre (structure/inharmonicity)
    // spreads their spacing; Bright controls the tallest peak height; Decay
    // shapes an exponential ring envelope drawn as a curve overlay.
    const float p   = juce::jlimit (0.0f, 1.0f, pitch);
    const float d   = juce::jlimit (0.0f, 1.0f, decay);
    const float b   = juce::jlimit (0.0f, 1.0f, bright);
    const float t   = juce::jlimit (0.0f, 1.0f, timbre);

    // Number of visible modes grows with Pitch (more partials fit at higher pitch).
    const int numBars = juce::jlimit (3, 12, juce::roundToInt (3.0f + p * 9.0f));

    // Inharmonicity: Timbre above 0.5 stretches the partials; below compresses.
    const float stretch = 1.0f + (t - 0.5f) * 0.6f;

    // Draw the modal comb.
    for (int i = 0; i < numBars; ++i)
    {
        // Harmonic-ish positions with stretch; the fundamental is tallest.
        const float pos  = std::pow (static_cast<float> (i + 1) / numBars, stretch);
        const float px   = x0 + pos * w;
        const float bw   = juce::jmax (1.0f, w / numBars * 0.35f);

        // Peak height: fundamental is full, higher modes decay; Bright lifts
        // the upper modes (more energy in the harmonics).
        const float heightFrac = (1.0f - static_cast<float> (i) / numBars)
                                 * (0.4f + 0.6f * b);
        const float bh = heightFrac * halfH;

        juce::Rectangle<float> bar (px - bw * 0.5f, midY - bh, bw, bh * 2.0f);
        g.setColour ((i == 0 ? accent : trace).withAlpha ((0.9f - 0.05f * i) * a));
        g.fillRoundedRectangle (bar, bw * 0.3f);
    }

    // Decay envelope curve (exponential ring-down overlay).
    const float tau = 0.1f + d * 0.8f;   // longer ring with more Decay
    juce::Path env;
    const int N = juce::jmax (2, juce::roundToInt (w));
    for (int i = 0; i <= N; ++i)
    {
        const float u = static_cast<float> (i) / N;
        const float envY = std::exp (-u / tau) * halfH;
        const float px = x0 + u * w;
        if (i == 0) env.startNewSubPath (px, midY - envY);
        else        env.lineTo (px, midY - envY);
    }
    g.setColour (dimText.withAlpha (0.45f * a));
    g.strokePath (env, juce::PathStrokeType (1.0f, juce::PathStrokeType::curved));

    g.setColour (dimText.withAlpha (0.55f));
    g.setFont (lnf ? lnf->appFont (8.0f, juce::Font::plain) : juce::Font (juce::FontOptions (8.0f)));
    g.drawText ("modes", plot.withTrimmedRight (2).withTrimmedBottom (1), juce::Justification::bottomRight);
}

//==========================================================================
std::unique_ptr<juce::AccessibilityHandler> FxSlotVisualizer::createAccessibilityHandler()
{
    // Mirrors OscPreviewDisplay/FilterResponseDisplay: expose the canvas as an
    // accessibility group (a labelled decorative graphic).
    return std::make_unique<juce::AccessibilityHandler> (*this,
            juce::AccessibilityRole::group);
}

void FxSlotVisualizer::visibilityChanged()
{
    // F-ios-perf-3 (iOS hunt 2026-08-19): run the 30 Hz poll only while this
    // display is actually showing (its page is current / the editor is on a
    // desktop). visibilityChanged fires on tab-page unparent (the
    // TabbedComponent removes non-current content) and on the initial
    // add-to-parent; the constructor's startTimerHz stays for the
    // first-show case (stopTimer on an already-stopped timer is a no-op).
    if (isShowing())
        startTimerHz (30);
    else
        stopTimer();
}

// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See FilterResponseDisplay.h.

#include "FilterResponseDisplay.h"

#include <cmath>

#include "ParvatiLookAndFeel.h"
#include "VectorTrace.h"

// Same cutoff byte->Hz mapping as the runtime filter
// (AnalogFilter::cutoffByteToHz in dsp/analog_filter.cpp): exponential across
// 20 Hz .. 16 kHz. Replicated here so this component stays independent of the
// runtime filter path (read-only; the DSP runtime is never touched).
namespace
{
    constexpr float kMinHz = 20.0f;
    constexpr float kMaxHz = 16000.0f;

    inline float cutoffByteToHz (uint8_t cutoffByte)
    {
        const float t = static_cast<float> (cutoffByte) / 255.0f;
        return juce::jlimit (kMinHz, kMaxHz, kMinHz * std::pow (kMaxHz / kMinHz, t));
    }

    constexpr float kTopDb    =  18.0f;   // +peak headroom (Q amplification at fc)
    constexpr float kBottomDb = -42.0f;   // far-attenuation floor
}

//==============================================================================
FilterResponseDisplay::FilterResponseDisplay (juce::String title,
                                              std::function<float()> getCutoff,
                                              std::function<float()> getReso,
                                              std::function<float()> getMode)
    : title_ (std::move (title)),
      getCutoff_ (std::move (getCutoff)),
      getReso_   (std::move (getReso)),
      getMode_   (std::move (getMode))
{
    if (! getCutoff_) getCutoff_ = [] { return 0.5f; };
    if (! getReso_)   getReso_   = [] { return 0.0f; };
    if (! getMode_)   getMode_   = [] { return 0.0f; };

    juce::Component::setTitle (title_);
    setDescription ("Filter frequency response preview");

    startTimerHz (30);
}

FilterResponseDisplay::~FilterResponseDisplay()
{
    stopTimer();
}

//==============================================================================
float FilterResponseDisplay::fetch (const std::function<float()>& f) const
{
    const float v = (f ? f() : 0.0f);
    return juce::jlimit (0.0f, 1.0f, v);
}

float FilterResponseDisplay::magnitudeSq (float f, float fc, float K, int mode)
{
    // 4-pole RESONANT LADDER model (Moog-style). Closed loop of four cascaded
    // one-pole low-pass sections with negative feedback K (0 = none, -> 4 =
    // self-oscillation):
    //     H(s) = 1 / ((1+s)^4 + K),   s = jw,  w = f/fc.
    // Expanding (1+jw)^4 = (1 - 6w^2 + w^4) + j*4w(1 - w^2)  =>  Re = A, Im = B,
    //     |H(jw)|^2 = 1 / ((A+K)^2 + B^2).
    // This single expression yields everything requested:
    //   * a 24 dB/oct skirt (4-pole) — much steeper than the former 2-pole;
    //   * a resonance PEAK near fc that grows as K -> 4;
    //   * a passband DROOP — DC gain = 1/(1+K)^2, so high resonance sinks the
    //     unattenuated part lower while the peak grows (classic analog ladder:
    //     resonance steals gain from the passband).
    // LP/BP/HP/Notch share this denominator; only the numerator (4-pole = squared
    // 2-pole prototype numerator) changes, so all four stay consistent.
    const float w  = (fc > 0.0f) ? (f / fc) : 0.0f;
    const float w2 = w * w;
    const float A  = 1.0f - 6.0f * w2 + w2 * w2;        // Re((1+jw)^4)
    const float B  = 4.0f * w * (1.0f - w2);             // Im((1+jw)^4)
    const float den4 = (A + K) * (A + K) + B * B;        // |(1+jw)^4 + K|^2

    float num;
    switch (mode)
    {
        case 1:  num = w2 * w2;                                            break;  // BP  (peaks at fc)
        case 2:  num = w2 * w2 * w2 * w2;                                  break;  // HP
        case 3:  num = (1.0f - w2) * (1.0f - w2)
                       * (1.0f - w2) * (1.0f - w2);                        break;  // Notch (deep at fc)
        default: num = 1.0f;                                               break;  // LP
    }
    return juce::jmax (1e-12f, num / den4);
}

void FilterResponseDisplay::timerCallback()
{
    const float c = fetch (getCutoff_);
    const float r = fetch (getReso_);
    const float m = fetch (getMode_);

    constexpr float eps    = 1.0f / 512.0f;

    // Track the live APVTS target EXACTLY so the preview is accurate under
    // automation (no smoothing lag). Detect a change vs the previously-shown
    // value to drive the repaint gate (eps gate: no constant repaint when idle).
    // Mode is discrete and snaps.
    const bool paramChanged = std::fabs (c - dispC_) > eps || std::fabs (r - dispR_) > eps;
    dispC_ = c;
    dispR_ = r;

    const bool modeChanged = std::fabs (m - lastM_) > eps;
    if (modeChanged) lastM_ = m;

    if (modeChanged || paramChanged)
        repaint();
}

//==============================================================================
void FilterResponseDisplay::paint (juce::Graphics& g)
{
    auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel());
    const ParvatiTheme* t = lnf ? lnf->getTheme() : nullptr;

    const auto panelBg  = t ? t->panelBackground : juce::Colour (0xff24242e);
    const auto outline  = t ? t->outline         : juce::Colour (0xff3c3c4a);
    const auto accent   = t ? t->accent          : juce::Colour (0xffe8b84b);
    const auto textDim  = t ? t->textDim         : juce::Colour (0xff9a9aa8);
    const auto trace    = hasCategoryColour_ ? categoryColour_ : accent;
    const auto gridCol  = t ? t->divider.withAlpha (0.10f) : accent.withAlpha (0.06f);

    const auto bounds = getLocalBounds().toFloat();
    g.setColour (panelBg);
    g.fillRect (bounds);

    // Faint clean grid backdrop (thin 1px lines).
    parvati::vectorTrace::drawGrid (g, bounds.reduced (0.5f), gridCol, 18.0f);

    g.setColour (outline);
    g.drawRect (bounds.reduced (0.5f), 1.0f);

    // Compact plot (decoration under "Filter 1": plot-focused, tiny corner label).
    auto plot = bounds.reduced (4.0f, 3.0f);
    const float plotTop = plot.getY();
    const float plotH   = plot.getHeight();

    // ---- Resolve the current filter params (cutoff/resonance are SMOOTHED
    //      displayed values; mode is discrete/snapped) ----
    const float cN = juce::jlimit (0.0f, 1.0f, dispC_ >= 0.0f ? dispC_ : fetch (getCutoff_));
    const float rN = juce::jlimit (0.0f, 1.0f, dispR_ >= 0.0f ? dispR_ : fetch (getReso_));
    const float mN = lastM_ >= 0.0f ? lastM_ : fetch (getMode_);

    const uint8_t cutoffByte = static_cast<uint8_t> (juce::roundToInt (cN * 255.0f));
    const float fc = cutoffByteToHz (cutoffByte);
    // Resonance as ladder feedback K: 0 at none, -> ~3.85 (just shy of
    // self-oscillation) at full. A quadratic ease so the resonance peak reads
    // through the knob range (taller peak sooner) while the passband droops.
    const float K = 3.85f * (1.0f - (1.0f - rN) * (1.0f - rN));
    const int mode = juce::jlimit (0, 3, juce::roundToInt (mN * 3.0f));

    // Map a magnitude in dB to a normalized unipolar level (1 = +kTopDb at top,
    // 0 = kBottomDb at the bottom baseline) for the shared vector recipe.
    auto dbToLevel = [&] (float db) -> float
    {
        const float level = (db - kBottomDb) / (kTopDb - kBottomDb);
        return juce::jlimit (0.0f, 1.0f, level);
    };

    // Log-spaced frequency for a column fraction in [0,1]: 20 Hz .. 16 kHz.
    auto freqAt = [] (float frac) -> float
    {
        return kMinHz * std::pow (kMaxHz / kMinHz, frac);
    };

    // Magnitude level (0..1 unipolar) at a column fraction.
    auto magLevel = [&] (float frac) -> float
    {
        const float f  = freqAt (frac);
        const float h2 = magnitudeSq (f, fc, K, mode);
        const float db = 10.0f * std::log10 (h2);   // == 20*log10(|H|)
        return dbToLevel (db);
    };

    // Smooth vector trace + translucent gradient fill (unipolar; the area fills
    // below the curve — the pass-band skirt). Peak alpha kept ~0.12 so the
    // vertical gradient (accent at the curve fading to 0% near the baseline)
    // stays subtle and the curve stays legible at the 42px decoration height.
    const int sampleCount = juce::jmax (64, juce::roundToInt (plot.getWidth() * 2.0f));
    parvati::vectorTrace::render (g, plot, sampleCount, magLevel,
                                  trace, parvati::vectorTrace::Mode::unipolar,
                                  false, 1.5f, 0.12f);

    // Cutoff vertical reference line (clean 1px).
    const float fcT = (fc > kMinHz) ? std::log (fc / kMinHz) / std::log (kMaxHz / kMinHz) : 0.0f;
    const float fcX = plot.getX() + juce::jlimit (0.0f, 1.0f, fcT) * plot.getWidth();
    g.setColour (accent.withAlpha (0.55f));
    g.drawVerticalLine (juce::roundToInt (fcX), plotTop, plot.getBottom());

    // 0 dB reference line (clean 1px).
    const float zeroLevel = dbToLevel (0.0f);
    const float zeroY = plotTop + (1.0f - zeroLevel) * plotH;
    g.setColour (accent.withAlpha (0.18f));
    g.drawHorizontalLine (juce::roundToInt (zeroY), plot.getX(), plot.getRight());

    // Tiny mode label (corner).
    static const char* const kModeName[] = { "LP", "BP", "HP", "NOTCH" };
    g.setColour (textDim);
    g.setFont (lnf ? lnf->appFont (10.0f, juce::Font::plain)
                   : juce::Font (juce::FontOptions (10.0f)));
    g.drawText (kModeName[mode],
                plot.withTrimmedRight (2).withTrimmedBottom (1),
                juce::Justification::bottomRight);
}

//==========================================================================
std::unique_ptr<juce::AccessibilityHandler> FilterResponseDisplay::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler> (*this,
            juce::AccessibilityRole::group);
}

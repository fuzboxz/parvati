// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See FxMasterEqCurve.h.

#include "FxMasterEqCurve.h"

#include <cmath>

#include "ParvatiLookAndFeel.h"
#include "ParvatiTheme.h"
#include "VectorTrace.h"

//==============================================================================
namespace
{
    // Representative sample rate (matches the FxChain default; the magnitude
    // shape at audio band frequencies is essentially rate-independent).
    constexpr double kFs    = 44100.0;
    constexpr double kTwoPi = 6.28318530717958647692;
    constexpr float  kEps   = 1.0f / 512.0f;   // change-gate epsilon

    // Plot frequency span (log-spaced X axis).
    constexpr double kFLo = 20.0;
    constexpr double kFHi = 20000.0;
    // Plot dB span (0 dB centred).
    constexpr float kDBSpan = 18.0f;

    // |H(e^jw)|^2 (in dB) for a biquad with NORMALIZED coeffs (a0 folded out).
    float bandMagDB (double f, double b0, double b1, double b2, double a1, double a2)
    {
        const double w  = kTwoPi * f / kFs;
        const double cw = std::cos (w);
        const double c2 = std::cos (2.0 * w);
        const double num = b0 * b0 + b1 * b1 + b2 * b2
                         + 2.0 * (b0 * b1 + b1 * b2) * cw + 2.0 * b0 * b2 * c2;
        const double den = 1.0 + a1 * a1 + a2 * a2
                         + 2.0 * (a1 + a1 * a2) * cw + 2.0 * a2 * c2;
        if (den <= 1e-30)
            return 0.0f;
        return (float) (10.0 * std::log10 (num / den));
    }

    // Composite master-EQ response (dB) at frequency @p f for the 3 params
    // (0..127). This REPLICATES FxChain::updateEqCoeffs exactly: the same RBJ
    // cookbook formulas, the same freq/gain mapping, the same a0 normalisation,
    // so the drawn curve matches the actual EQ bit-for-bit (shape).
    float eqResponseDB (double f, int lo, int mid, int hi)
    {
        float db = 0.0f;

        // Each band: compute the RAW RBJ coeffs, normalise by a0, sum the dB.
        auto addBand = [&] (double b0r, double b1r, double b2r,
                            double a0,  double a1r, double a2r)
        {
            const double inv = 1.0 / a0;
            db += bandMagDB (f, b0r * inv, b1r * inv, b2r * inv, a1r * inv, a2r * inv);
        };

        // ---- Low-cut: high-pass, 20 Hz..~1.5 kHz exponential across 1..127 (0 = off) ----
        if (lo != 0)
        {
            const double t    = (double) (lo - 1) / 126.0;
            const double freq = 20.0 * std::pow (1500.0 / 20.0, t);
            const double w0 = kTwoPi * freq / kFs, cw = std::cos (w0), sw = std::sin (w0);
            const double alpha = sw / (2.0 * 0.70710678);   // Q = 1/sqrt(2)
            addBand ((1.0 + cw) * 0.5, -(1.0 + cw), (1.0 + cw) * 0.5,
                     1.0 + alpha, -2.0 * cw, 1.0 - alpha);
        }

        // ---- Mid: peaking at 1 kHz, Q=1, gain (mid-64)/64 * +/-12 dB ----
        {
            const double w0 = kTwoPi * 1000.0 / kFs, cw = std::cos (w0), sw = std::sin (w0);
            const double gainDB = ((double) mid - 64.0) / 64.0 * 12.0;
            const double A      = std::pow (10.0, gainDB / 40.0);
            const double alpha  = sw / 2.0;   // Q=1
            addBand (1.0 + alpha * A, -2.0 * cw, 1.0 - alpha * A,
                     1.0 + alpha / A, -2.0 * cw, 1.0 - alpha / A);
        }

        // ---- High: shelf at 5 kHz, gain (high-64)/64 * +/-12 dB, slope S=1 ----
        {
            const double w0 = kTwoPi * 5000.0 / kFs, cw = std::cos (w0), sw = std::sin (w0);
            const double gainDB = ((double) hi - 64.0) / 64.0 * 12.0;
            const double A      = std::pow (10.0, gainDB / 40.0);
            const double sqA    = std::sqrt (A);
            const double alpha  = sw * 0.70710678;   // S=1 => sw/2 * sqrt(2)
            const double a0     = (A + 1.0) - (A - 1.0) * cw + 2.0 * sqA * alpha;
            addBand (A * ((A + 1.0) + (A - 1.0) * cw + 2.0 * sqA * alpha),
                     -2.0 * A * ((A - 1.0) + (A + 1.0) * cw),
                     A * ((A + 1.0) + (A - 1.0) * cw - 2.0 * sqA * alpha),
                     a0,
                     2.0 * ((A - 1.0) - (A + 1.0) * cw),
                     (A + 1.0) - (A - 1.0) * cw - 2.0 * sqA * alpha);
        }

        return db;
    }

    // Log-frequency fraction (0..1) for @p f across [kFLo..kFHi].
    inline float xFracForFreq (double f)
    {
        return (float) ((std::log10 (f) - std::log10 (kFLo))
                        / (std::log10 (kFHi) - std::log10 (kFLo)));
    }
} // namespace

//==============================================================================
FxMasterEqCurve::FxMasterEqCurve (Getter getLow, Getter getMid, Getter getHigh)
    : getLow_ (std::move (getLow)),
      getMid_ (std::move (getMid)),
      getHigh_ (std::move (getHigh))
{
    if (! getLow_)  getLow_  = [] { return 0.0f; };
    if (! getMid_)  getMid_  = [] { return 64.0f / 127.0f; };
    if (! getHigh_) getHigh_ = [] { return 64.0f / 127.0f; };

    juce::Component::setTitle ("FX Master EQ Curve");
    setDescription ("Composite low-cut / mid / high-shelf EQ response");

    dispLow_  = fetch (getLow_);
    dispMid_  = fetch (getMid_);
    dispHigh_ = fetch (getHigh_);

    startTimerHz (30);
}

FxMasterEqCurve::~FxMasterEqCurve()
{
    stopTimer();
}

//==============================================================================
float FxMasterEqCurve::fetch (const Getter& f) const
{
    const float v = (f ? f() : 0.0f);
    return juce::jlimit (0.0f, 1.0f, std::isfinite (v) ? v : 0.0f);
}

void FxMasterEqCurve::timerCallback()
{
    const float lo = fetch (getLow_);
    const float mid = fetch (getMid_);
    const float hi = fetch (getHigh_);

    const bool changed = std::fabs (lo - dispLow_)   > kEps
                      || std::fabs (mid - dispMid_) > kEps
                      || std::fabs (hi - dispHigh_) > kEps;
    dispLow_ = lo;  dispMid_ = mid;  dispHigh_ = hi;
    if (changed)
        repaint();
}

//==============================================================================
void FxMasterEqCurve::paint (juce::Graphics& g)
{
    auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel());
    const ParvatiTheme* t = lnf ? lnf->getTheme() : nullptr;

    const auto panelBg = t ? t->backgroundPanel : juce::Colour (0xff24242e);
    const auto outline = t ? t->outline         : juce::Colour (0xff3c3c4a);
    const auto accent  = t ? t->accentPrimary   : juce::Colour (0xffe8b84b);
    const auto dimText = t ? t->textSecondary   : juce::Colour (0xff9a9aa8);
    const auto gridCol = t ? t->divider.withAlpha (0.10f) : accent.withAlpha (0.06f);
    const auto trace   = hasCategoryColour_ ? categoryColour_ : accent;

    const auto bounds = getLocalBounds().toFloat();
    g.setColour (panelBg);
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (outline);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);

    // Reserve a slim band at the bottom for the three band labels.
    const float labelH = juce::jmin (12.0f, bounds.getHeight() * 0.18f);
    auto plot = bounds.reduced (4.0f, 3.0f).withTrimmedBottom (labelH);

    parvati::vectorTrace::drawGrid (g, plot, gridCol, 16.0f);

    // 0 dB centre line.
    const float cy = plot.getCentre().y;
    g.setColour (dimText.withAlpha (0.35f));
    g.drawHorizontalLine (juce::roundToInt (cy), plot.getX(), plot.getRight());

    // Resolve the displayed params (-1 => first paint: fetch fresh).
    auto toByte = [] (float n01) -> int
    {
        return juce::jlimit (0, 127, juce::roundToInt (juce::jlimit (0.0f, 1.0f, n01) * 127.0f));
    };
    const int lo  = toByte (dispLow_  >= 0.0f ? dispLow_  : fetch (getLow_));
    const int mid = toByte (dispMid_  >= 0.0f ? dispMid_  : fetch (getMid_));
    const int hi  = toByte (dispHigh_ >= 0.0f ? dispHigh_ : fetch (getHigh_));

    // Sample the composite response across a log-frequency span.
    const int cols = juce::jmax (40, juce::roundToInt (plot.getWidth() / 4.0f));
    juce::Path curve;
    bool first = true;
    for (int i = 0; i <= cols; ++i)
    {
        const float xf = (float) i / (float) cols;
        const double f = kFLo * std::pow (kFHi / kFLo, (double) xf);
        const float db = juce::jlimit (-kDBSpan, kDBSpan, eqResponseDB (f, lo, mid, hi));
        const float x = plot.getX() + xf * plot.getWidth();
        const float y = plot.getY() + (1.0f - (db / kDBSpan) * 0.5f - 0.5f) * plot.getHeight();
        // (db/kDBSpan in [-1,1]; *0.5 +0.5 maps to [0,1]; 1 - .. => +dB at top.)
        if (first) { curve.startNewSubPath (x, y); first = false; }
        else        curve.lineTo (x, y);
    }
    g.setColour (trace);
    g.strokePath (curve, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

    // ---- Band labels positioned under their bands ----
    g.setColour (dimText);
    g.setFont (lnf ? lnf->appFont (8.5f, juce::Font::plain) : juce::Font (juce::FontOptions (8.5f)));
    const auto labelArea = bounds.withTrimmedTop (bounds.getBottom() - labelH).reduced (4.0f, 0.0f);
    const auto drawBandLabel = [&] (const juce::String& text, float xf)
    {
        const float x = labelArea.getX() + xf * labelArea.getWidth();
        juce::Rectangle<float> r (x - 24.0f, labelArea.getY(), 48.0f, labelArea.getHeight());
        if (r.getX() < labelArea.getX()) r.setX (labelArea.getX());
        if (r.getRight() > labelArea.getRight()) r.setRight (labelArea.getRight());
        g.drawText (text, r, juce::Justification::centred, true);
    };
    // Low Cut at its cutoff freq (or the low edge when off).
    const double lowFreq = lo != 0 ? 20.0 * std::pow (1500.0 / 20.0, (double) (lo - 1) / 126.0)
                                   : 40.0;
    drawBandLabel ("Low Cut",  xFracForFreq (lowFreq));
    drawBandLabel ("Mid",      xFracForFreq (1000.0));
    drawBandLabel ("High Shelf", xFracForFreq (5000.0));
}

//==========================================================================
std::unique_ptr<juce::AccessibilityHandler> FxMasterEqCurve::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::group);
}

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

void FilterResponseDisplay::setLiveValuesProvider (std::function<parvati::LiveFilterValues()> p)
{
    liveValuesProvider_ = std::move (p);
    // An un-set provider must immediately hide any shown overlay (the state is
    // re-resolved on the next poll tick; a repaint now avoids one stale frame).
    if (! liveValuesProvider_ && dispLiveActive_)
    {
        dispLiveActive_ = false;
        repaint();
    }
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

    // ---- Live modulated overlay poll (docs/LIVE_MOD_FEEDBACK_DESIGN.md) ----
    // ONE provider call per tick (none at all when never wired). ACTIVITY is
    // TEMPORAL, not spatial: the overlay shows while the effective bytes are
    // MOVING (>= 1 byte vs the previous tick on either axis) and holds for a
    // short window after the last movement, hiding once the values settle.
    // Why not "departs from the knob base": the engine's effective cutoff
    // byte includes KEY TRACKING (~2 bytes per semitone), so a spatial
    // base-vs-live threshold trips for EVERY held note on any patch with
    // tracking — an always-on second curve for what is a static patch
    // setting, not live modulation. Temporal gating matches the goal
    // ("actively being modulated"): an env sweep, an LFO wobble or a wheel
    // ride moves the bytes every tick; a held note with static key tracking
    // settles within the hold window and the single opaque base preview
    // returns. The x/tick position is tracked from the live cutoff whenever
    // the provider is active, so the test seam reads position through
    // sub-threshold wobble too.
    constexpr int kLiveHoldTicks = 8;   // ~270 ms @ 30 Hz: bridges modulation
                                        // dips below the 1-byte/tick rate without
                                        // flicker, still hides a settled note fast
    bool  liveActive = false;
    int   liveCut = dispLiveCutByte_;
    int   liveRes = dispLiveResByte_;
    float liveCutX = dispLiveCutX_;
    if (liveValuesProvider_)
    {
        const parvati::LiveFilterValues lv = liveValuesProvider_();
        if (lv.active)
        {
            liveCut = juce::roundToInt (juce::jlimit (0.0f, 1.0f, lv.cutoff01) * 255.0f);
            liveRes = juce::roundToInt (juce::jlimit (0.0f, 1.0f, lv.reso01)   * 255.0f);
            const bool liveMoved = std::abs (liveCut - dispLiveCutByte_) >= 1
                                || std::abs (liveRes - dispLiveResByte_) >= 1;
            if (liveMoved)
                liveHoldTicks_ = kLiveHoldTicks;   // (re)arm the hold window
            else if (liveHoldTicks_ > 0)
                --liveHoldTicks_;
            liveActive = liveHoldTicks_ > 0;

            // Normalized log-frequency column of the live cutoff tick (same
            // mapping paint() uses for the ticks; kMinHz/kMaxHz live in this
            // file's anonymous namespace).
            const float fc = cutoffByteToHz (static_cast<uint8_t> (juce::jlimit (0, 255, liveCut)));
            liveCutX = juce::jlimit (0.0f, 1.0f,
                        (fc > kMinHz) ? std::log (fc / kMinHz) / std::log (kMaxHz / kMinHz) : 0.0f);
        }
        else
        {
            liveHoldTicks_ = 0;   // voice gone (released/killed): hide at once
        }
    }
    else
    {
        liveHoldTicks_ = 0;       // no provider wired: nothing to hold
    }
    // >= 1 byte of live motion (while visible) is enough to re-stroke the
    // overlay — sub-byte wobble cannot move the curve a whole pixel anyway.
    const bool liveChanged = (liveActive != dispLiveActive_)
        || (liveActive && (  std::abs (liveCut - dispLiveCutByte_) >= 1
                          || std::abs (liveRes - dispLiveResByte_) >= 1));
    dispLiveActive_  = liveActive;
    dispLiveCutByte_ = liveCut;
    dispLiveResByte_ = liveRes;
    dispLiveCutX_    = liveCutX;

    if (modeChanged || paramChanged || liveChanged)
    {
        ++generation_;   // TEST-ONLY: a real refresh is observable (see header)
        repaint();
    }
}

//==============================================================================
void FilterResponseDisplay::paint (juce::Graphics& g)
{
    auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel());
    const ParvatiTheme* t = lnf ? lnf->getTheme() : nullptr;

    const auto panelBg  = t ? t->backgroundPanel : juce::Colour (0xff24242e);
    const auto outline  = t ? t->outline         : juce::Colour (0xff3c3c4a);
    const auto accent   = t ? t->accentPrimary          : parvati::parvatiFallbackAccent;
    const auto textDim  = t ? t->textSecondary         : juce::Colour (0xff9a9aa8);
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
    // Resonance as ladder feedback K: 0 at none, -> ~3.85 (just shy of
    // self-oscillation) at full. A quadratic ease so the resonance peak reads
    // through the knob range (taller peak sooner) while the passband droops.
    // (The K derivation itself lives in drawCurve below — ONE definition
    // shared by the base curve and the live overlay.)
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

    // ONE curve renderer for BOTH the opaque base preview and the live
    // modulated overlay (docs/LIVE_MOD_FEEDBACK_DESIGN.md): the same ladder
    // magnitude model evaluated at a (cutoff byte, resonance 0..1) pair, so
    // the two curves can never disagree on the math. `withFill` picks the base
    // recipe (gradient area fill + 1.5px stroke); otherwise the LIVE overlay
    // recipe (stroke-only at a higher weight, full trace alpha, NO second fill
    // — two stacked gradients would read as a bright smear, and the overlay
    // must stay a clean "moving" line over the steady preview).
    auto drawCurve = [&] (uint8_t cutByte, float resoN, bool withFill) -> float
    {
        const float fc = cutoffByteToHz (cutByte);
        const float K  = 3.85f * (1.0f - (1.0f - resoN) * (1.0f - resoN));

        // Magnitude level (0..1 unipolar) at a column fraction.
        auto magLevel = [&] (float frac) -> float
        {
            const float f  = freqAt (frac);
            const float h2 = magnitudeSq (f, fc, K, mode);
            const float db = 10.0f * std::log10 (h2);   // == 20*log10(|H|)
            return dbToLevel (db);
        };

        const int sampleCount = juce::jmax (64, juce::roundToInt (plot.getWidth() * 2.0f));
        if (withFill)
        {
            // Peak alpha kept ~0.12 so the vertical gradient (accent at the
            // curve fading to 0% near the baseline) stays subtle and the curve
            // stays legible at the 42px decoration height.
            parvati::vectorTrace::render (g, plot, sampleCount, magLevel,
                                          trace, parvati::vectorTrace::Mode::unipolar,
                                          false, 1.5f, 0.12f);
        }
        else
        {
            juce::Path live = parvati::vectorTrace::buildTrace (plot, sampleCount, magLevel,
                                    parvati::vectorTrace::Mode::unipolar, false);
            g.setColour (trace);
            g.strokePath (live, juce::PathStrokeType (1.75f,
                                juce::PathStrokeType::curved,
                                juce::PathStrokeType::rounded));
        }
        return fc;
    };

    // Normalized log-frequency column (0..1) of an fc tick x position.
    auto fcColumn = [] (float fc) -> float
    {
        return juce::jlimit (0.0f, 1.0f,
                (fc > kMinHz) ? std::log (fc / kMinHz) / std::log (kMaxHz / kMinHz) : 0.0f);
    };

    // ---- BASE curve: exactly as before, always in place ----
    // Smooth vector trace + translucent gradient fill (unipolar; the area fills
    // below the curve — the pass-band skirt).
    const float fc = drawCurve (cutoffByte, rN, true);

    // ---- LIVE modulated overlay (docs/LIVE_MOD_FEEDBACK_DESIGN.md) ----
    // Drawn only while the effective bytes depart from the base knob bytes
    // (the change gate in timerCallback keeps dispLiveActive_ false otherwise).
    // The BASE curve + fill stay untouched in place; the base fc tick dims so
    // the bright live tick carries the attention.
    const bool liveOn = dispLiveActive_ && dispLiveCutByte_ >= 0;
    if (liveOn)
    {
        const uint8_t liveCutByte = static_cast<uint8_t> (juce::jlimit (0, 255, dispLiveCutByte_));
        const float   liveRN      = juce::jlimit (0.0f, 1.0f,
                                    static_cast<float> (juce::jlimit (0, 255, dispLiveResByte_)) / 255.0f);
        const float liveFc = drawCurve (liveCutByte, liveRN, false);

        // Bright live cutoff tick: the moving "what is happening" position.
        const float liveX = plot.getX() + fcColumn (liveFc) * plot.getWidth();
        g.setColour (trace.withAlpha (0.85f));
        g.drawVerticalLine (juce::roundToInt (liveX), plotTop, plot.getBottom());
    }

    // Cutoff vertical reference line (clean 1px) — dimmed while the live tick
    // is shown so the pair reads base-vs-modulated, not two equal markers.
    const float fcX = plot.getX() + fcColumn (fc) * plot.getWidth();
    g.setColour (accent.withAlpha (liveOn ? 0.30f : 0.55f));
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

void FilterResponseDisplay::visibilityChanged()
{
    updatePollTimer();
}

void FilterResponseDisplay::parentHierarchyChanged()
{
    updatePollTimer();
}

void FilterResponseDisplay::updatePollTimer()
{
    // F-ios-perf-3 gate (see OscPreviewDisplay.h for the full rationale):
    // BOTH hooks are required — visibilityChanged fires while still unparented
    // (addAndMakeVisible's setVisible precedes parenting) and never again once
    // ancestors change; parentHierarchyChanged fires on every hierarchy change
    // including the editor gaining its peer, which is the reliable
    // "became showing" signal. Fixed the frozen-preview regression.
    if (isShowing())
        startTimerHz (30);
    else
        stopTimer();
}

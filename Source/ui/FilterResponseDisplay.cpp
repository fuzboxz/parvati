// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.  See FilterResponseDisplay.h.

#include "FilterResponseDisplay.h"

#include <cmath>

#include "HellcatLookAndFeel.h"
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

    constexpr float kTopDb    =  21.0f;   // +peak headroom (Q laws reach +20 dB at r=0.95)
    constexpr float kBottomDb = -42.0f;   // far-attenuation floor

    // Drawn resonance ceiling: mirror of AnalogFilter::kMaxResonance (0.95).
    // Keeps every Q law finite at knob 63 (the runtime clamps there too).
    constexpr float kMaxDrawnReso = 0.95f;

    // 4P cascade per-stage Q exponent: mirror of kSsm4PeakExp in
    // Source/dsp/analog_filter.h (0.616). Keep the two in sync.
    constexpr float kSsm4PeakExp = 0.616f;

    // IR3109 resonance feedback cap: mirror of kIr3109KfbMax in
    // Source/dsp/analog_filter.h (3.4, below the 4.0 onset). The visibly
    // lower peak is the point of the preview.
    constexpr float kIr3109Kfb = 3.4f;

    // Ladder resonance feedback floor: JUCE maps its knob so the feedback
    // cannot drop below 0.4 (the documented dead zone). Mirror of
    // ladderResonanceKnob in Source/dsp/analog_filter.cpp.
    constexpr float kLadderKfbFloor = 0.4f;
}

//==============================================================================
FilterResponseDisplay::FilterResponseDisplay (juce::String title,
                                              std::function<float()> getCutoff,
                                              std::function<float()> getReso,
                                              std::function<float()> getMode,
                                              std::function<float()> getCard)
    : title_ (std::move (title)),
      getCutoff_ (std::move (getCutoff)),
      getReso_   (std::move (getReso)),
      getMode_   (std::move (getMode)),
      getCard_   (std::move (getCard))
{
    if (! getCutoff_) getCutoff_ = [] { return 0.5f; };
    if (! getReso_)   getReso_   = [] { return 0.0f; };
    if (! getMode_)   getMode_   = [] { return 0.0f; };
    if (! getCard_)   getCard_   = [] { return 0.0f; };   // SMR4 (default card)

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

float FilterResponseDisplay::ladderMagnitudeSq (float f, float fc, float K, int mode)
{
    // 4-pole RESONANT LADDER model (transistor-ladder class). Closed loop of four cascaded
    // one-pole low-pass sections with negative feedback K (0 = none, -> 4 =
    // self-oscillation):
    //     H(s) = 1 / ((1+s)^4 + K),   s = jw,  w = f/fc.
    // Expanding (1+jw)^4 = (1 - 6w^2 + w^4) + j*4w(1 - w^2)  =>  Re = A, Im = B,
    //     |H(jw)|^2 = 1 / ((A+K)^2 + B^2).
    // This single expression yields:
    //   * a 24 dB/oct skirt (4-pole);
    //   * a resonance PEAK near fc that grows as K -> 4;
    //   * a passband DROOP — DC gain = 1/(1+K)^2, so high resonance sinks the
    //     unattenuated part lower while the peak grows (classic analog ladder:
    //     resonance steals gain from the passband).
    // LP/BP/HP/Notch share this denominator; only the numerator changes.
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

float FilterResponseDisplay::magnitudeSq (float f, float fc, int card, float reso, int mode)
{
    // CARD-AWARE dispatch. The laws MIRROR the runtime resonance maps in
    // Source/dsp/analog_filter.h (kMaxResonance, kSsm4PeakExp, kIr3109KfbMax,
    // ladderResonanceKnob; the 2-pole Q law). Keep the two in sync. This
    // component stays independent of the DSP headers, so the constants are
    // local copies.
    const float r  = juce::jlimit (0.0f, kMaxDrawnReso, reso);
    const float w  = (fc > 0.0f) ? (f / fc) : 0.0f;
    const float w2 = w * w;

    const bool fourPoleFeedback = (card == 0 || card == 3 || card == 5);
    if (fourPoleFeedback)
    {
        // SMR4 (0): K = 4*r — the exact onset law.
        // Ladder (3): K = max(0.4, 4*r) — JUCE floors the feedback at 0.4.
        // IR3109 (5): K = 3.4*r — the factory cap, below the 4.0 onset.
        // These cards draw lowpass only (the caller clamps the mode).
        float K = 4.0f * r;
        if (card == 3) K = juce::jmax (kLadderKfbFloor, K);
        if (card == 5) K = kIr3109Kfb * r;
        return ladderMagnitudeSq (f, fc, K, mode);
    }

    if (card == 1)
    {
        // "4P" SSM2164: TWO identical cascaded 2-pole stages. Per-stage
        // q = 0.5*(1-r)^-kSsm4PeakExp (the runtime law): q = 0.5 at r = 0
        // (the exact cascade baseline), q(0.95)^2 = 10 (+20 dB, the family
        // cluster). |H| = |H1|^2, so |H|^2 = |H1|^4. Lowpass numerator only.
        const float q    = 0.5f * std::pow (juce::jmax (1.0e-4f, 1.0f - r), -kSsm4PeakExp);
        const float den1 = (1.0f - w2) * (1.0f - w2) + (w / q) * (w / q);
        return juce::jmax (1e-12f, 1.0f / (den1 * den1));
    }

    // 2-pole family: SVF (2) and the Polivoks skeleton (4).
    // Q = 1/(2*(1-r)): Q 0.5 at r = 0, Q 10 at r = 0.95 (the runtime cap).
    // The Polivoks character layer is not drawn; the skeleton is the honest
    // static estimate. Numerators by mode: LP 1, BP w, HP w^2, Notch (1-w^2).
    const float Q   = 0.5f / juce::jmax (1.0e-4f, 1.0f - r);
    const float den = (1.0f - w2) * (1.0f - w2) + (w / Q) * (w / Q);

    float num;
    switch (mode)
    {
        case 1:  num = w;                             break;  // BP
        case 2:  num = w2;                            break;  // HP
        case 3:  num = juce::jmax (1.0e-6f, std::fabs (1.0f - w2));  break;  // Notch (zero at fc)
        default: num = 1.0f;                          break;  // LP
    }
    return juce::jmax (1e-12f, (num * num) / den);
}

void FilterResponseDisplay::setLiveValuesProvider (std::function<hellcat::LiveFilterValues()> p)
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
    const int   card = juce::jlimit (0, 5, juce::roundToInt (fetch (getCard_) * 5.0f));

    constexpr float eps    = 1.0f / 512.0f;

    // Track the live APVTS target EXACTLY so the preview is accurate under
    // automation (no smoothing lag). Detect a change vs the previously-shown
    // value to drive the repaint gate (eps gate: no constant repaint when idle).
    // Mode and card are discrete and snap.
    const bool paramChanged = std::fabs (c - dispC_) > eps || std::fabs (r - dispR_) > eps;
    dispC_ = c;
    dispR_ = r;

    const bool modeChanged = std::fabs (m - lastM_) > eps;
    if (modeChanged) lastM_ = m;

    // Card change: snap the curve family (a card switch redraws at once —
    // the slope and the resonance law both depend on the card).
    const bool cardChanged = (card != lastCard_);
    if (cardChanged) lastCard_ = card;

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
        const hellcat::LiveFilterValues lv = liveValuesProvider_();
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

    // ---- Smoothed display convergence (see the header) ----
    // The paint target follows the OVERLAY state: live bytes while the
    // temporal gate is armed, base bytes at rest — one continuous ease both
    // ways (modulation onset glides in from the base curve, release glides
    // back). tau ~ 55 ms reads as responsive-but-liquid at 30 Hz (the per-tick
    // step alpha = 1 - e^(-dt/tau), rate-independent across the 5..60 Hz
    // refresh pref). Convergence beyond the eps gate keeps the repaint going
    // even when the raw bytes sit still for a tick.
    {
        const float tgtC = liveActive ? juce::jlimit (0.0f, 255.0f, (float) liveCut) / 255.0f : dispC_;
        const float tgtR = liveActive ? juce::jlimit (0.0f, 255.0f, (float) liveRes) / 255.0f : dispR_;
        if (smoothCut01_ < 0.0f || smoothRes01_ < 0.0f)
        {
            smoothCut01_ = tgtC;   // first tick (or after a reset): snap
            smoothRes01_ = tgtR;
        }
        else
        {
            const int intervalMs = getTimerInterval();
            const float dt = intervalMs > 0 ? (float) intervalMs * 0.001f : 1.0f / 30.0f;
            // tau 130 ms (2026-08-22, was 55): the live curve's byte-quantized
            // target steps (~1-4 bytes/tick during a sweep) were still visible
            // through a 55 ms ease at 30 Hz — choppiness under modulation. The
            // longer constant blends successive steps into liquid motion while
            // onset/release still read as prompt (~2 tau to settle).
            const float alpha = 1.0f - std::exp (-dt / 0.130f);
            smoothCut01_ += (tgtC - smoothCut01_) * alpha;
            smoothRes01_ += (tgtR - smoothRes01_) * alpha;
        }
        const float epsS = 1.0f / 1024.0f;
        const bool converging = std::fabs (smoothCut01_ - tgtC) > epsS
                             || std::fabs (smoothRes01_ - tgtR) > epsS;
        if (modeChanged || paramChanged || cardChanged || liveChanged || converging)
        {
            ++generation_;   // TEST-ONLY: a real refresh is observable (see header)
            repaint();
        }
        return;
    }
}

//==============================================================================
void FilterResponseDisplay::paint (juce::Graphics& g)
{
    const HellcatTheme* t = hellcat::themeFor (*this);

    const auto panelBg  = t ? t->backgroundPanel : hellcat::kFallbackPanel;
    const auto outline  = t ? t->outline         : hellcat::kFallbackOutline;
    const auto accent   = t ? t->accentPrimary          : hellcat::hellcatFallbackAccent;
    const auto trace    = hasCategoryColour_ ? categoryColour_ : accent;
    const auto gridCol  = t ? t->divider.withAlpha (0.10f) : accent.withAlpha (0.06f);

    const auto bounds = getLocalBounds().toFloat();
    g.setColour (panelBg);
    g.fillRect (bounds);

    // Faint clean grid backdrop (thin 1px lines).
    hellcat::vectorTrace::drawGrid (g, bounds.reduced (0.5f), gridCol, 18.0f);

    g.setColour (outline);
    g.drawRect (bounds.reduced (0.5f), 1.0f);

    // Compact plot (decoration under "Filter 1": plot-focused, tiny corner label).
    auto plot = bounds.reduced (4.0f, 3.0f);
    const float plotTop = plot.getY();
    const float plotH   = plot.getHeight();

    // ---- Resolve the current filter params (cutoff/resonance are SMOOTHED
    //      displayed values; mode is discrete/snapped) ----
    // smoothCut01_/smoothRes01_ carry the eased display state (base at rest,
    // live under modulation — see timerCallback); until the first tick they
    // are -1, so fall back to the raw fetches for a correct static paint.
    const float cN = smoothCut01_ >= 0.0f ? smoothCut01_ : juce::jlimit (0.0f, 1.0f, dispC_ >= 0.0f ? dispC_ : fetch (getCutoff_));
    const float rN = smoothRes01_ >= 0.0f ? smoothRes01_ : juce::jlimit (0.0f, 1.0f, dispR_ >= 0.0f ? dispR_ : fetch (getReso_));
    const float mN = lastM_ >= 0.0f ? lastM_ : fetch (getMode_);
    const int   card = lastCard_ >= 0 ? lastCard_
                     : juce::jlimit (0, 5, juce::roundToInt (fetch (getCard_) * 5.0f));

    const uint8_t cutoffByte = static_cast<uint8_t> (juce::roundToInt (cN * 255.0f));
    const int mode = juce::jlimit (0, 3, juce::roundToInt (mN * 3.0f));

    // Drawn mode per card: the runtime clamps the patch mode to LOWPASS on
    // every 4-pole card (applyFilterModeFromVoice in Source/AmbikaVoice.cpp —
    // the hardware cards are lowpass). The SVF card honours all four modes;
    // the Polivoks card provides LP and BP outputs only (HP/Notch clamp to
    // LP — see the FilterTopology docs in Source/dsp/analog_filter.h).
    const int drawnMode = (card == 2) ? mode
                        : (card == 4) ? (mode == 1 ? 1 : 0)
                        : 0;

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
    // modulated overlay (docs/LIVE_MOD_FEEDBACK_DESIGN.md): the same
    // card-aware magnitude model evaluated at a (cutoff byte, resonance 0..1)
    // pair, so the two curves can never disagree on the math. `withFill`
    // picks the base recipe (gradient area fill + 1.5px stroke); otherwise
    // the LIVE overlay recipe (stroke-only at a higher weight, full trace
    // alpha, NO second fill — two stacked gradients would read as a bright
    // smear, and the overlay must stay a clean "moving" line over the steady
    // preview). The resonance law resolution lives in magnitudeSq — ONE
    // definition shared by the base curve and the live overlay.
    auto drawCurve = [&] (uint8_t cutByte, float resoN, bool withFill) -> float
    {
        const float fc = cutoffByteToHz (cutByte);

        // Magnitude level (0..1 unipolar) at a column fraction.
        auto magLevel = [&] (float frac) -> float
        {
            const float f  = freqAt (frac);
            const float h2 = magnitudeSq (f, fc, card, resoN, drawnMode);
            const float db = 10.0f * std::log10 (h2);   // == 20*log10(|H|)
            return dbToLevel (db);
        };

        const int sampleCount = juce::jmax (64, juce::roundToInt (plot.getWidth() * 2.0f));
        if (withFill)
        {
            // Peak alpha kept ~0.12 so the vertical gradient (accent at the
            // curve fading to 0% near the baseline) stays subtle and the curve
            // stays legible at the 42px decoration height.
            hellcat::vectorTrace::render (g, plot, sampleCount, magLevel,
                                          trace, hellcat::vectorTrace::Mode::unipolar,
                                          false, 1.5f, 0.12f);
        }
        else
        {
            juce::Path live = hellcat::vectorTrace::buildTrace (plot, sampleCount, magLevel,
                                    hellcat::vectorTrace::Mode::unipolar, false);
            g.setColour (trace);
            g.strokePath (live, juce::PathStrokeType (1.75f,
                                juce::PathStrokeType::curved,
                                juce::PathStrokeType::rounded));
        }
        return fc;
    };

    // ---- ONE curve at a time (2026-08-21 user request) ----
    // While the filter is being modulated the STATIC base curve is hidden —
    // the live overlay renders with the base recipe (gradient fill + stroke)
    // so the preview stays visually continuous: one curve that MOVES while
    // the modulation runs and settles back to the knob state at rest (the
    // activity gate is temporal, so the handoff is seamless).
    const bool liveOn = dispLiveActive_ && dispLiveCutByte_ >= 0;
    if (liveOn)
    {
        const uint8_t liveCutByte = static_cast<uint8_t> (juce::jlimit (0, 255, dispLiveCutByte_));
        const float   liveRN      = juce::jlimit (0.0f, 1.0f,
                                    static_cast<float> (juce::jlimit (0, 255, dispLiveResByte_)) / 255.0f);
        drawCurve (liveCutByte, liveRN, true);
    }
    else
    {
        // ---- BASE curve (at rest): exactly as before ----
        // Smooth vector trace + translucent gradient fill (unipolar; the area
        // fills below the curve — the pass-band skirt).
        drawCurve (cutoffByte, rN, true);
    }

    // (The static cutoff vertical reference line was REMOVED 2026-08-22 per
    // user request — the curve's knee already carries the cutoff position and
    // the line read as noise; same cleanup class as the LP/BP/HP/NOTCH corner
    // label removal below.)

    // 0 dB reference line (clean 1px).
    const float zeroLevel = dbToLevel (0.0f);
    const float zeroY = plotTop + (1.0f - zeroLevel) * plotH;
    g.setColour (accent.withAlpha (0.18f));
    g.drawHorizontalLine (juce::roundToInt (zeroY), plot.getX(), plot.getRight());

    // (2026-08-21) The LP/BP/HP/NOTCH corner label was REMOVED per user
    // request — the curve shape itself communicates the topology, and the
    // caption competed with the moving live curve.
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

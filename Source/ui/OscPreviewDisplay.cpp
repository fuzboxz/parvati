// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See OscPreviewDisplay.h.

#include "OscPreviewDisplay.h"

#include <cmath>

#include "ParvatiLookAndFeel.h"
#include "VectorTrace.h"

// Real oscillator (DSP-sample path for the non-basic, deterministic shapes).
#include "dsp/constants.h"   // kAudioBlockSize
#include "dsp/oscillator.h"
#include "dsp/patch.h"       // WAVEFORM_* algorithm indices

//==============================================================================
namespace
{
    // Resolution of the analytic one-cycle buffer (smooth curves).
    constexpr int kAnalyticCycle = 256;

    // Phase increment (24-bit {integral:16, fractional:8}) that advances the
    // oscillator phase by ~one full cycle across kAudioBlockSize samples, so the
    // rendered 40-sample buffer holds one cycle. 65536 / 40 = 1638.4 =>
    // integral=1638, fractional=round(0.4*256)=102.
    inline ambika::dsp::uint24_t oneCycleIncrement()
    {
        ambika::dsp::uint24_t inc;
        inc.integral   = 1638;
        inc.fractional = 102;
        return inc;
    }

    // The BASE knob fetch is tracked EXACTLY (the eps-change term of the
    // repaint gate); the PAINTED value is the smoothed pair (see
    // timerCallback). kParamEps is the sub-knob jitter epsilon used by that
    // change gate. kMorphStep advances a discrete SHAPE-switch morph over
    // ~66 ms (2 ticks at 30 Hz) — the analogue of the filter display's
    // snapping mode switch, kept short so a shape change is not a hard snap.
    constexpr float kParamEps  = 1.0f / 512.0f;
    constexpr float kMorphStep = 1.0f / 2.0f;

    // Live-overlay temporal gate: a >= 1-byte effective move (re)arms this
    // many ticks of hold (~270 ms @ 30 Hz) — bridges modulation dips below
    // the 1-byte/tick rate without flicker, still hides a settled note fast.
    // Same constant + reasoning as FilterResponseDisplay's kLiveHoldTicks.
    constexpr int kLiveHoldTicks = 8;

    // Critically-damped convergence time constant (seconds) — the LIVE
    // overlay path keeps the SAME tau the FilterResponseDisplay uses, so
    // both previews glide identically under modulation.
    constexpr float kSmoothTau = 0.130f;

    // KNOB-PATH GLIDE (2026-08-23 second revision — "when the animation
    // finishes there is another delayed change"): the first revision's
    // adaptive-tau exponential still had an exponential TAIL — byte
    // crossings got sparser as it decelerated, so the preview looked
    // settled and then one final byte-step (or the half-byte snap crossing
    // a rounding boundary) popped in ~200-400 ms late. The knob path now
    // uses a FIXED-DURATION glide instead: every target change eases from
    // the current value to the target over kGlideT with a quadratic-out
    // curve (fast start, zero slope at the end), so ALL byte changes —
    // including the last — land inside the window and NOTHING can change
    // after it completes. Retargeting on every change makes a fast spin
    // track tightly (no windup: the residual never exceeds one glide of
    // knob motion), and release finishes exactly kGlideT later. The LIVE
    // path keeps the fixed-tau exponential (filter parity: its 1-4-byte
    // telemetry steps at 30 Hz are what the glide exists to blend).
    constexpr float kGlideT  = 0.140f;          // seconds, per target change
    constexpr float kSnapEps = 1.0f / 256.0f;   // half a param byte (LIVE path snap)

    inline uint8_t paramByteFromFloat (float p)
    {
        return static_cast<uint8_t> (juce::jlimit (0, 127,
            juce::roundToInt (juce::jlimit (0.0f, 1.0f, p) * 127.0f)));
    }
}  // namespace

//==============================================================================
OscPreviewDisplay::OscPreviewDisplay (juce::String title,
                                      std::function<float()> getShape,
                                      std::function<float()> getParam)
    : title_ (std::move (title)),
      getShape_ (std::move (getShape)),
      getParam_ (std::move (getParam))
{
    if (! getShape_) getShape_ = [] { return 0.0f; };
    if (! getParam_) getParam_ = [] { return 0.0f; };

    // OPAQUE painting (2026-08-23 CPU fix): paint() fills the ENTIRE bounds
    // with the panel background first, so the component can promise JUCE it
    // covers every pixel — a repaint then no longer recomposites the parent
    // (and the sibling controls in the region) behind it. The 30 Hz animated
    // previews repaint often; this was the bulk of the reported CPU cost.
    setOpaque (true);

    juce::Component::setTitle (title_);
    setDescription ("Oscillator waveform preview");

    // Render the initial cycle so the first paint (before the 30 Hz tick) shows
    // the current shape rather than an empty panel. The smoothed display value
    // is seeded from the SAME fetched param the cycle is built for, so the
    // first tick starts converged (no startup animation, and the byte-diff
    // rebuild can never fire on a stale ctor value).
    {
        const float sh0 = fetch (getShape_);
        const float pa0 = fetch (getParam_);
        const int shapeIdx0 = juce::jlimit (0, static_cast<int> (ambika::dsp::WAVEFORM_LAST) - 1,
                                            juce::roundToInt (sh0 * static_cast<float> (ambika::dsp::WAVEFORM_LAST - 1)));
        const uint8_t paramByte0 = paramByteFromFloat (pa0);
        cachedShape_        = shapeIdx0;
        lastBuiltParamByte_ = paramByte0;
        rebuildCycle (shapeIdx0, paramByte0);
        displayedParam_ = pa0;   // seed the exact base fetch (no startup anim)
        smoothParam01_  = pa0;   // seed the smoothed pair from the same value
    }

    startTimerHz (30);
}

OscPreviewDisplay::~OscPreviewDisplay()
{
    stopTimer();
}

//==============================================================================
float OscPreviewDisplay::fetch (const std::function<float()>& f) const
{
    const float v = (f ? f() : 0.0f);
    return juce::jlimit (0.0f, 1.0f, v);
}

void OscPreviewDisplay::timerCallback()
{
    const float sh = fetch (getShape_);
    const float pa = fetch (getParam_);

    // Base knob fetch, tracked EXACTLY (the smoothed target at rest). The
    // painted waveform depends only on the QUANTIZED byte of the smoothed
    // value, so the raw fetch's eps-change no longer belongs in the repaint
    // gate (2026-08-23 CPU fix: convergence-only ticks with an unchanged
    // byte move nothing on screen — the gate fires on BYTE movement below).
    displayedParam_ = pa;

    const int shapeIdx = juce::jlimit (0, static_cast<int> (ambika::dsp::WAVEFORM_LAST) - 1,
                                       juce::roundToInt (sh * static_cast<float> (ambika::dsp::WAVEFORM_LAST - 1)));
    const bool shapeChanged = shapeIdx != cachedShape_;

    // ---- Live modulated overlay poll (filter-preview parity) ----
    // ONE provider call per tick (none at all when never wired). ACTIVITY is
    // TEMPORAL, not spatial — the same gate + rationale as the filter
    // overlay: the engine's effective byte moves every tick under an
    // env/LFO/matrix sweep, while a held note with a settled (or static)
    // routing stops moving within the hold window and the preview eases back
    // to the knob state. A vanished voice hides the overlay at once.
    bool liveActive = false;
    int  liveParam  = dispLiveParamByte_;
    if (liveValuesProvider_)
    {
        const parvati::LiveOscValues lv = liveValuesProvider_();
        if (lv.active)
        {
            liveParam = juce::roundToInt (juce::jlimit (0.0f, 1.0f, lv.param01) * 127.0f);
            const bool liveMoved = std::abs (liveParam - dispLiveParamByte_) >= 1;
            if (liveMoved)
                liveHoldTicks_ = kLiveHoldTicks;   // (re)arm the hold window
            else if (liveHoldTicks_ > 0)
                --liveHoldTicks_;
            liveActive = liveHoldTicks_ > 0;
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
    const bool liveChanged = (liveActive != dispLiveActive_)
        || (liveActive && std::abs (liveParam - dispLiveParamByte_) >= 1);
    dispLiveActive_    = liveActive;
    dispLiveParamByte_ = liveParam;

    // ---- Smoothed display convergence (two-path, 2026-08-23 second rev) ----
    // The target follows the OVERLAY state: live effective byte while the
    // temporal gate is armed, base knob value at rest. KNOB path: a
    // FIXED-DURATION glide (kGlideT, quadratic-out ease) — bounded by
    // construction, no exponential tail, so no byte-step can land after the
    // animation completes (the "delayed change" bug). LIVE path: the fixed
    // tau exponential (filter parity). Both stay rate-independent across
    // the 5..60 Hz refresh pref.
    const float tgt = liveActive
        ? juce::jlimit (0.0f, 1.0f, static_cast<float> (liveParam) / 127.0f)
        : displayedParam_;
    if (smoothParam01_ < 0.0f)
    {
        smoothParam01_ = tgt;   // first tick (or after a reset): snap
        glideTo01_    = tgt;    // (and the glide starts converged)
    }
    else if (liveActive)
    {
        const int intervalMs = getTimerInterval();
        const float dt = intervalMs > 0 ? static_cast<float> (intervalMs) * 0.001f : 1.0f / 30.0f;
        const float alpha = 1.0f - std::exp (-dt / kSmoothTau);
        smoothParam01_ += (tgt - smoothParam01_) * alpha;
        // Half-byte snap (invisible — the waveform is byte-quantized).
        if (std::fabs (tgt - smoothParam01_) < kSnapEps)
            smoothParam01_ = tgt;
        glideTo01_ = -1.0f;   // stale on this path; the next knob tick retargets
    }
    else
    {
        const int intervalMs = getTimerInterval();
        const float dt = intervalMs > 0 ? static_cast<float> (intervalMs) * 0.001f : 1.0f / 30.0f;
        // (Re)target whenever the knob value moved: the glide RESTARTS from
        // the current displayed value, so a continuous spin keeps retargeting
        // (tracks with <= one glide of lag — no windup) and a release finishes
        // exactly kGlideT after the last change.
        if (glideTo01_ < 0.0f || std::fabs (tgt - glideTo01_) > kParamEps)
        {
            glideFrom01_ = smoothParam01_;
            glideTo01_   = tgt;
            glideT_      = 0.0f;
        }
        glideT_ += dt;
        const float u = juce::jlimit (0.0f, 1.0f, glideT_ / kGlideT);
        const float e = u * (2.0f - u);   // quadratic-out: fast start, 0 slope at the end
        smoothParam01_ = glideFrom01_ + (glideTo01_ - glideFrom01_) * e;
        if (u >= 1.0f)
            smoothParam01_ = glideTo01_;   // EXACT completion — nothing can change after
    }
    // (convergence state is implicit: the byte-diff below IS the visible
    // convergence signal — a tick that moves no byte paints nothing)

    // Repaint gate: visual-state FLIPS only — everything else (the byte-diff
    // below, the morph advance) ORs itself in as it changes actual pixels.
    bool needRepaint = shapeChanged || liveChanged;

    // ---- Cycle maintenance ----
    // SHAPE switch: stash + short morph (the filter's mode snaps; a waveform
    // swap reads better eased). PARAMETER motion: rebuild whenever the
    // byte-quantized SMOOTHED value moves — for the analytic shapes that is
    // the exact glyph at the new duty/formant mix, and for the DSP-sampled
    // algorithms a fresh deterministic Oscillator render (see buildSampled:
    // same (shape, byte) -> bit-identical buffer, so a moving param reshapes
    // the waveform with NO flicker; the old 8-step quantization + morph
    // restarts are gone).
    if (shapeChanged)
    {
        cachedShape_ = shapeIdx;
        stashAndRebuild (shapeIdx);
        needRepaint = true;
    }
    else
    {
        const uint8_t smoothByte = paramByteFromFloat (smoothParam01_);
        if (smoothByte != lastBuiltParamByte_)
        {
            rebuildCycle (shapeIdx, smoothByte);
            lastBuiltParamByte_ = smoothByte;
            needRepaint = true;   // the painted CYCLE changed -> pixels changed
        }
        // else: sub-byte convergence with an unchanged byte moves NOTHING on
        // screen (the waveform is byte-quantized) — NO repaint. This bounds
        // the paint count by information (byte crossings, <= 127 per full
        // sweep) instead of by ticks, which together with the adaptive tau
        // removed the 2026-08-23 CPU hot spot (spin + ~0.9 s tail of 30 Hz
        // repaints, each recompositing the non-opaque parent region).
    }

    // Advance any in-flight SHAPE morph (eps gate: repaint only while morphing).
    if (morphProgress_ < 1.0f)
    {
        morphProgress_ = juce::jmin (1.0f, morphProgress_ + kMorphStep);
        if (morphProgress_ >= 1.0f)
            prevCycle_.clear();   // drop the source once the morph completes
        needRepaint = true;
    }

    if (needRepaint)
        repaint();
}

void OscPreviewDisplay::setLiveValuesProvider (std::function<parvati::LiveOscValues()> p)
{
    liveValuesProvider_ = std::move (p);
    // An un-set provider must immediately hide any armed overlay (the state is
    // re-resolved on the next poll tick; a repaint now avoids one stale frame).
    if (! liveValuesProvider_ && dispLiveActive_)
    {
        dispLiveActive_ = false;
        liveHoldTicks_ = 0;
        repaint();
    }
}

void OscPreviewDisplay::stashAndRebuild (int shapeIdx)
{
    if (! cycle_.empty())
        prevCycle_ = cycle_;                 // morph source = last displayed cycle
    // Build the new shape's cycle at the CURRENT (smoothed) parameter byte so
    // the morph is param-continuous — a shape switch mid-glide does not snap
    // back to a stale param first.
    const uint8_t b = paramByteFromFloat (smoothParam01_ >= 0.0f ? smoothParam01_ : displayedParam_);
    rebuildCycle (shapeIdx, b);
    lastBuiltParamByte_ = b;
    morphProgress_ = 0.0f;
}

//==============================================================================
void OscPreviewDisplay::buildAnalytic (int shapeIdx, uint8_t paramByte)
{
    cycle_.resize (kAnalyticCycle);
    const float paramF = static_cast<float> (paramByte) / 127.0f;

    for (int i = 0; i < kAnalyticCycle; ++i)
    {
        const float phase = static_cast<float> (i) / static_cast<float> (kAnalyticCycle);   // 0..1
        float v = 0.0f;
        switch (shapeIdx)
        {
            case ambika::dsp::WAVEFORM_NONE:
                v = 0.0f;                                   // silence -> flat midline
                break;
            case ambika::dsp::WAVEFORM_SAW:
                v = 2.0f * phase - 1.0f;                    // rising ramp
                break;
            case ambika::dsp::WAVEFORM_SQUARE:
            {
                // 50% duty at param 0; the duty narrows with the parameter (PWM).
                const float duty = juce::jlimit (0.05f, 0.95f, 0.5f + 0.4f * paramF);
                v = (phase < duty) ? 1.0f : -1.0f;
                break;
            }
            case ambika::dsp::WAVEFORM_TRIANGLE:
                v = (phase < 0.5f) ? (4.0f * phase - 1.0f) : (3.0f - 4.0f * phase);
                break;
            case ambika::dsp::WAVEFORM_SINE:
                v = std::sin (2.0f * juce::MathConstants<float>::pi * phase);
                break;
            default:
                v = 0.0f;
                break;
        }
        cycle_[(size_t) i] = juce::jlimit (-1.0f, 1.0f, v);
    }
}

void OscPreviewDisplay::buildSampled (int shapeIdx, uint8_t paramByte)
{
    // Instantiate the REAL oscillator OFF the audio thread. The instance is
    // local and FRESH on every rebuild: value-initialization zeroes phase_ /
    // data_ (the filtered-noise LFSR starts from its fixed zero state), so the
    // SAME (shape, param byte) always renders the bit-identical 40-sample
    // buffer — the load-bearing determinism behind the smoothed per-byte
    // rebuild in timerCallback (a moving param reshapes the waveform; it can
    // never flicker between renders of the same byte). Reset() is NOT called
    // (it would touch the shared global Random LFSR used by the audio-thread
    // vowel renderer). Filtered Noise uses its OWN per-instance LFSR, so it
    // still renders a valid (deterministic-enough) texture without Reset().
    ambika::dsp::Oscillator osc;
    osc.set_parameter (paramByte);

    constexpr int N = ambika::dsp::kAudioBlockSize;   // 40
    uint8_t syncIn[N] = {};
    uint8_t syncOut[N] = {};
    uint8_t buf[N] = {};

    osc.Render (static_cast<uint8_t> (shapeIdx),
                /*note=*/60,                 // mid note -> a representative band-limit zone
                oneCycleIncrement(),
                syncIn, syncOut,
                buf);

    cycle_.resize (N);
    for (int i = 0; i < N; ++i)
        cycle_[(size_t) i] = juce::jlimit (-1.0f, 1.0f, (static_cast<float> (buf[i]) - 128.0f) * (1.0f / 128.0f));
}

void OscPreviewDisplay::rebuildCycle (int shapeIdx, uint8_t paramByte)
{
    ++generation_;   // TEST-ONLY: every real rebuild is observable (see header)

    // The 5 basic shapes are drawn ANALYTICALLY (exact).
    if (shapeIdx == ambika::dsp::WAVEFORM_NONE
        || shapeIdx == ambika::dsp::WAVEFORM_SAW
        || shapeIdx == ambika::dsp::WAVEFORM_SQUARE
        || shapeIdx == ambika::dsp::WAVEFORM_TRIANGLE
        || shapeIdx == ambika::dsp::WAVEFORM_SINE)
    {
        buildAnalytic (shapeIdx, paramByte);
        return;
    }

    // Vowel touches the shared global Random (data race with the audio thread)
    // -> analytic multi-formant GLYPH fallback.
    if (shapeIdx == ambika::dsp::WAVEFORM_VOWEL)
    {
        cycle_.resize (kAnalyticCycle);
        const float p = static_cast<float> (paramByte) / 127.0f;
        for (int i = 0; i < kAnalyticCycle; ++i)
        {
            const float phase = static_cast<float> (i) / static_cast<float> (kAnalyticCycle);
            // Glottal pulse (one per cycle) modulated by 3 formant sines.
            const float glottal = (phase < 0.5f) ? std::sin (juce::MathConstants<float>::pi * phase / 0.5f) : 0.0f;
            const float tp = juce::MathConstants<float>::twoPi * phase;
            const float f = 0.45f * std::sin (2.0f * tp)
                          + 0.30f * std::sin (7.0f * tp)
                          + 0.20f * std::sin (13.0f * tp);
            cycle_[(size_t) i] = juce::jlimit (-1.0f, 1.0f, glottal * (0.5f + 0.5f * p) + f * 0.5f);
        }
        return;
    }

    // All other (deterministic) algorithms: SAMPLE one cycle from the real DSP.
    buildSampled (shapeIdx, paramByte);
}

//==============================================================================
void OscPreviewDisplay::paint (juce::Graphics& g)
{
    auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel());
    const ParvatiTheme* t = lnf ? lnf->getTheme() : nullptr;

    const auto panelBg = t ? t->backgroundPanel : juce::Colour (0xff24242e);
    const auto outline = t ? t->outline         : juce::Colour (0xff3c3c4a);
    const auto accent  = t ? t->accentPrimary          : parvati::parvatiFallbackAccent;
    const auto trace   = hasCategoryColour_ ? categoryColour_ : accent;
    const auto gridCol = t ? t->divider.withAlpha (0.10f) : accent.withAlpha (0.06f);

    const auto bounds = getLocalBounds().toFloat();
    g.setColour (panelBg);
    g.fillRect (bounds);

    // Faint clean grid backdrop (thin 1px lines).
    parvati::vectorTrace::drawGrid (g, bounds.reduced (0.5f), gridCol, 18.0f);

    g.setColour (outline);
    g.drawRect (bounds.reduced (0.5f), 1.0f);

    // Compact plot area (inline preview: no title row, maximize waveform).
    auto plot = bounds.reduced (3.0f, 3.0f);

    // Sample a one-cycle buffer at a fractional phase position [0,1] (handles
    // any length: analytic = 256, sampled = kAudioBlockSize).
    auto sampleBuf = [] (const std::vector<float>& buf, float phase) -> float
    {
        const int n = static_cast<int> (buf.size());
        if (n <= 0) return 0.0f;
        const float pos = phase * static_cast<float> (n - 1);
        const int i0 = static_cast<int> (pos);
        if (i0 >= n - 1) return buf[(size_t) (n - 1)];
        const float fr = pos - static_cast<float> (i0);
        return buf[(size_t) i0] * (1.0f - fr) + buf[(size_t) (i0 + 1)] * fr;
    };

    // While morphing, lerp the previous cycle into the new target sample-by-
    // sample (both indexed by the same phase fraction); otherwise show the new
    // cycle. Square/PWM edges stay crisp (per-sample lerp of the two waveforms).
    auto sampleCycle = [&] (float phase) -> float
    {
        if (morphProgress_ >= 1.0f || prevCycle_.empty())
            return sampleBuf (cycle_, phase);
        return sampleBuf (prevCycle_, phase) * (1.0f - morphProgress_)
             + sampleBuf (cycle_,    phase) * morphProgress_;
    };

    // Smooth vector trace + translucent gradient fill (bipolar, around the
    // midline). Square/PWM stays smooth; only an S&H shape (none here) would use
    // the staircase variant.
    const int sampleCount = juce::jmax (64, juce::roundToInt (plot.getWidth() * 2.0f));
    parvati::vectorTrace::render (g, plot, sampleCount, sampleCycle,
                                  trace, parvati::vectorTrace::Mode::bipolar,
                                  false, 1.5f, 0.12f);

    // Midline reference (1px) so the bipolar centre reads on every shape.
    g.setColour (accent.withAlpha (0.25f));
    g.drawHorizontalLine (juce::roundToInt (plot.getCentre().y),
                          plot.getX(), plot.getRight());
}

//==========================================================================
std::unique_ptr<juce::AccessibilityHandler> OscPreviewDisplay::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler> (*this,
            juce::AccessibilityRole::group);
}

void OscPreviewDisplay::visibilityChanged()
{
    updatePollTimer();
}

void OscPreviewDisplay::parentHierarchyChanged()
{
    updatePollTimer();
}

void OscPreviewDisplay::updatePollTimer()
{
    // See the header: run the 30 Hz poll only while actually showing. Both
    // hooks funnel here — visibilityChanged alone cannot see "became showing
    // via parenting / peer creation" (the frozen-preview bug).
    if (isShowing())
        startTimerHz (30);
    else
        stopTimer();
}

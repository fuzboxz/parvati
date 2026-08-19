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

    // The continuous oscillator parameter is tracked EXACTLY (displayed = live
    // target) so the preview is accurate under automation (no smoothing lag).
    // The morph is the only smoothing left and it is short (~66 ms, 2 ticks at
    // 30 Hz) so a discrete OSC shape switch is not a hard snap. kParamEps is the
    // sub-knob jitter epsilon used by the change gates.
    constexpr float kParamEps  = 1.0f / 512.0f;
    constexpr float kMorphStep = 1.0f / 2.0f;

    inline uint8_t paramByteFromFloat (float p)
    {
        return static_cast<uint8_t> (juce::jlimit (0, 127,
            juce::roundToInt (juce::jlimit (0.0f, 1.0f, p) * 127.0f)));
    }
    inline uint8_t quantParamQ (float p)
    {
        return static_cast<uint8_t> (paramByteFromFloat (p) >> 4);
    }

    // Shapes rebuilt analytically (no Oscillator::Render -> flicker-free, so the
    // cycle may be rebuilt from the exact (live) parameter on change).
    inline bool isAnalyticShape (int idx)
    {
        return idx == ambika::dsp::WAVEFORM_NONE
            || idx == ambika::dsp::WAVEFORM_SAW
            || idx == ambika::dsp::WAVEFORM_SQUARE
            || idx == ambika::dsp::WAVEFORM_TRIANGLE
            || idx == ambika::dsp::WAVEFORM_SINE
            || idx == ambika::dsp::WAVEFORM_VOWEL;
    }
    // Analytic shapes whose one-cycle glyph actually depends on the parameter
    // (Square PWM duty / Vowel formant mix) -> worth a continuous rebuild.
    inline bool analyticShapeUsesParam (int idx)
    {
        return idx == ambika::dsp::WAVEFORM_SQUARE
            || idx == ambika::dsp::WAVEFORM_VOWEL;
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

    juce::Component::setTitle (title_);
    setDescription ("Oscillator waveform preview");

    // Render the initial cycle so the first paint (before the 30 Hz tick) shows
    // the current shape rather than an empty panel.
    {
        const float sh0 = fetch (getShape_);
        const float pa0 = fetch (getParam_);
        const int shapeIdx0 = juce::jlimit (0, static_cast<int> (ambika::dsp::WAVEFORM_LAST) - 1,
                                            juce::roundToInt (sh0 * static_cast<float> (ambika::dsp::WAVEFORM_LAST - 1)));
        const uint8_t paramByte0 = static_cast<uint8_t> (
            juce::jlimit (0, 127, juce::roundToInt (pa0 * 127.0f)));
        cachedShape_  = shapeIdx0;
        cachedParamQ_ = static_cast<uint8_t> (paramByte0 >> 4);
        rebuildCycle (shapeIdx0, paramByte0);
        displayedParam_  = pa0;       // seed the displayed param (no startup anim)
        lastBuiltParamF_ = pa0;
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

    // Track the live APVTS target EXACTLY so the preview is accurate under
    // automation (no smoothing lag). The only smoothing left is the short
    // discrete-shape morph below; analytic shapes rebuild from this exact value
    // and sampled shapes rebuild on a quantized change of it.
    displayedParam_ = pa;

    const int shapeIdx = juce::jlimit (0, static_cast<int> (ambika::dsp::WAVEFORM_LAST) - 1,
                                       juce::roundToInt (sh * static_cast<float> (ambika::dsp::WAVEFORM_LAST - 1)));

    bool needRepaint = false;

    if (shapeIdx != cachedShape_)
    {
        // Discrete shape switch -> morph from the previously-displayed cycle.
        cachedShape_ = shapeIdx;
        stashAndRebuild (shapeIdx);
        needRepaint = true;
    }
    else if (isAnalyticShape (shapeIdx) && morphProgress_ >= 1.0f)
    {
        // Analytic shapes are flicker-free (no Oscillator::Render), so rebuild
        // from the exact (live) parameter on change -> PWM duty / formants
        // track the real value with no lag.
        if (analyticShapeUsesParam (shapeIdx)
            && std::fabs (displayedParam_ - lastBuiltParamF_) > kParamEps)
        {
            rebuildCycle (shapeIdx, paramByteFromFloat (displayedParam_));
            lastBuiltParamF_ = displayedParam_;
            needRepaint = true;
        }
    }
    else if (! isAnalyticShape (shapeIdx))
    {
        // DSP-sampled shapes: rebuild ONLY on a quantized-parameter change (a
        // per-tick re-render would flicker, e.g. Filtered Noise re-seeds), and
        // morph between the cycles for a smooth transition.
        const uint8_t paramQ = quantParamQ (displayedParam_);
        if (paramQ != cachedParamQ_)
        {
            cachedParamQ_ = paramQ;
            stashAndRebuild (shapeIdx);
            needRepaint = true;
        }
    }

    // Advance any in-flight morph (eps gate: repaint only while morphing).
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

void OscPreviewDisplay::stashAndRebuild (int shapeIdx)
{
    if (! cycle_.empty())
        prevCycle_ = cycle_;                 // morph source = last displayed cycle
    rebuildCycle (shapeIdx, paramByteFromFloat (displayedParam_));
    cachedParamQ_    = quantParamQ (displayedParam_);
    lastBuiltParamF_ = displayedParam_;
    morphProgress_   = 0.0f;
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
    // local; only its own state is mutated. Reset() is NOT called (it would
    // touch the shared global Random LFSR used by the audio-thread vowel
    // renderer). Filtered Noise uses its OWN per-instance LFSR, so it still
    // renders a valid (deterministic-enough) texture without Reset().
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

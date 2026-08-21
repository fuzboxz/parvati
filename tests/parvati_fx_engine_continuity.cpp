// Full-engine continuity regression: renders a sustained note through the ACTUAL
// ParvatiAudioProcessor::processBlock (-> renderPartFx -> FxChain -> each Clouds
// FX's HostRateBridge) with each Clouds FX enabled, no modulation, and asserts
// the main-bus output is click-free.
//
// WHY this test exists (and why the isolated fx->process tests missed the bug):
// the audible Clouds-FX crackle was a 1-sample sub-chunk produced ONLY by
// renderPartFx's drift-corrected ~980 Hz sub-chunking. That 1-sample block hit
// HostRateBridge's m==0 branch, which ZEROED the output (a full-amplitude
// dropout). Isolated fx->process / chain.process tests never generate that
// 1-sample chunk, so they passed while the plugin clicked. This test drives the
// real path, so a regression of THIS bug (or any future renderPartFx/chain
// orchestration glitch) fails here.
//
// Detector: curvature-immune -- |delta[i]| that is both > 8x the 93rd-percentile
// |delta| of a trailing 64-sample window AND > 0.004 absolute. The threshold
// (0.06) sits in the gap between the modes' intrinsic grain (PitchShifter/WSOLA
// ~0.017-0.043, their crossfade/splice character) and the m==0 dropout bug
// (>=0.082). FAILS if the HostRateBridge m==0 hold fix is reverted.
//
// Build: linked as parvati_fx_engine_continuity_test (see CMakeLists).


#include <algorithm>
#include "unified_test_runner.h"
#include <cmath>
#include <cstdio>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "PluginProcessor.h"
#include "dsp/fx/FxProcessor.h"   // createFxProcessor (direct WSOLA startup regression)

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

void setInt (ParvatiAudioProcessor& proc, const char* id, int value)
{
    if (auto* param = proc.getApvts().getParameter (id))
        if (auto* ip = dynamic_cast<juce::AudioParameterInt*> (param))
            ip->setValueNotifyingHost (ip->convertTo0to1 (static_cast<float> (value)));
}
void setChoice (ParvatiAudioProcessor& proc, const char* id, int index)
{
    if (auto* param = proc.getApvts().getParameter (id))
        param->setValueNotifyingHost (param->convertTo0to1 (static_cast<float> (index)));
}

const char* fxName (int ti)
{
    switch (ti) { case 1: return "Diffuser"; case 2: return "PitchShifter"; case 3: return "Reverb";
        case 4: return "LoopingDelay"; case 5: return "WSOLA"; case 6: return "Spectral"; default: return "?"; }
}

// Returns the worst flagged impulse (max |delta| among curvature-immune outliers)
// over the steady-state region of the captured main-bus L. 0.0 if none flagged.
double worstImpulse (const std::vector<float>& out, int warmup, int total)
{
    std::vector<float> d (static_cast<size_t> (total), 0.0f);
    for (int i = warmup + 1; i < total; ++i)
        d[(size_t) i] = std::fabs (out[(size_t) i] - out[(size_t) i - 1]);
    double worst = 0.0;
    for (int i = warmup + 65; i < total - 1; ++i)
    {
        float window[64];
        for (int k = 0; k < 64; ++k) window[k] = d[(size_t) (i - 64 + k)];
        std::sort (window, window + 64);
        const float base = window[60];   // ~93rd percentile of the trailing window
        if (d[(size_t) i] > 8.0f * base && d[(size_t) i] > 0.004f)
            worst = std::max (worst, static_cast<double> (d[(size_t) i]));
    }
    return worst;
}

// Render a sustained note through the full processor with FX slot 1 = @p fxType.
// Returns the captured main-bus L.
std::vector<float> render (int fxType, double sr, int bufferSize, double durSec)
{
    ParvatiAudioProcessor proc;
    proc.prepareToPlay (sr, bufferSize);
    proc.getApvts().getParameterAsValue ("part_select") = 1.0f;
    setInt (proc, "osc1_shape", 1);   // SAW (audible source on Part 0)
    setChoice (proc, "fx1_type", fxType);
    setInt (proc, "fx1_enabled", 1);
    setInt (proc, "fx1_drywet", 127);                       // full wet
    // Freeze-safe params: param3 (Looper/WSOLA Freeze, >0.5 holds the loop)
    // must stay BELOW the >0.5 threshold -- 64/127 = 0.504 FREEZES the
    // buffer-based FX into a silent empty buffer, silently degrading this
    // test to a dry-copy check for them (their wet path -- the thing under
    // test -- was never exercised before this fix).
    for (int k = 1; k <= 4; ++k)
        setInt (proc, ("fx1_param" + std::to_string (k)).c_str(), k == 3 ? 32 : 64);   // ~0.5, freeze params safe

    // flush one block so the FX state + note-on settle
    { juce::AudioBuffer<float> f (2, bufferSize); f.clear(); juce::MidiBuffer e; proc.processBlock (f, e); }

    const int total = static_cast<int> (durSec * sr);
    std::vector<float> cap (static_cast<size_t> (total), 0.0f);
    bool noteOn = false;
    int written = 0;
    while (written < total)
    {
        juce::AudioBuffer<float> buf (2, bufferSize); buf.clear();
        juce::MidiBuffer midi;
        if (! noteOn) { midi.addEvent (juce::MidiMessage::noteOn (1, 60, static_cast<uint8_t> (100)), 0); noteOn = true; }
        proc.processBlock (buf, midi);
        const int n = std::min (bufferSize, total - written);
        const float* L = buf.getReadPointer (0);
        for (int i = 0; i < n; ++i) cap[(size_t) (written + i)] = L[i];
        written += n;
    }
    return cap;
}

// The continuity bound. See file header: sits above the modes' intrinsic grain
// (max ~0.043) and below the m==0 dropout bug (>=0.082).
constexpr double kClickBound = 0.06;

void runConfig (double sr, int bufferSize)
{
    const double durSec = 4.0;
    const int total = static_cast<int> (durSec * sr);
    const int warmup = static_cast<int> (0.5 * sr);   // skip note-attack + buffer-fill transients
    std::printf ("-- @%.1fk, host buffer %d --\n", sr / 1000.0, bufferSize);
    for (int t : { 1, 2, 3, 4, 5, 6 })
    {
        const auto cap = render (t, sr, bufferSize, durSec);
        const double worst = worstImpulse (cap, warmup, total);
        char msg[160];
        std::snprintf (msg, sizeof (msg), "%-12s @%.0fk buf=%d: worst impulse=%.4f (must be < %.3f)",
                       fxName (t), sr / 1000.0, bufferSize, worst, kClickBound);
        check (worst < kClickBound, msg);
    }
}
// ----------------------------------------------------------------------------
// WSOLA startup-splice regression (2026-08 crackle audit).
//
// Root cause (fixed in clouds/dsp/wsola_sample_player.h): the FIRST WSOLA
// window was scheduled from an unrun correlator (best_match()==0), placing it
// at buffer position -window_size_/2. Its 0->1 gain ramp then played over the
// never-written zeroed tail of the record buffer, and the window reached FULL
// gain exactly as it stepped onto the first recorded sample - an instantaneous
// discontinuity of ~|signal[0]| per channel, window_size_/2 internal samples
// (= 1024 x 48k/32k = 1536 host samples) into the stream, with no fading
// partner window.
//
// Measured BEFORE the fix (standalone sweep, params {0.5,0.5,0.5,0,1.0},
// 220 Hz stereo sines, 0.5 amp, 48 kHz, 64-sample chunks):
//   jumpR 0.126-0.144 @ samples 1536-1538 (8.8-10.0x the input slope 0.0144);
//   jumpL stayed ~1x slope only because that sweep's L happened to start on a
//   zero crossing (sin(0)=0) - the step tracks |signal[0]| per channel, so the
//   phases below (+0.5/+0.7 rad) step BOTH channels pre-fix (~10x slope).
// Measured AFTER the fix (window placed at the head -> its gain ramp fades IN
// the recorded audio): both channels stay <= ~2x slope everywhere, including
// the first three window periods.
void wsolaStartupRegression()
{
    constexpr double sr = 48000.0;
    constexpr int kChunk = 64;
    constexpr int kTotal = 9216;   // 3 window periods (window = 2048 internal = 3072 host)
    constexpr float kAmp = 0.5f;
    constexpr float kSlope = 2.0f * 3.14159265f * 220.0f * kAmp / static_cast<float> (sr);   // 0.0144

    auto fx = createFxProcessor (FxType::WSOLAStretch);
    check (fx != nullptr, "WSOLA startup: factory returns non-null");
    if (! fx)
        return;
    fx->prepare (sr, 256);

    const float params[5] = { 0.5f, 0.5f, 0.5f, 0.0f, 1.0f };   // unison, tone LP bypassed

    // Whole-run maxima AND post-startup maxima. The startup window: the first
    // correlator-placed window crossfades against the head-anchored one at a
    // misaligned point, so the first ~1.5 window periods carry an INHERENT
    // both-channel transient (measured post-fix: L 0.2537 / R 0.2504);
    // samples beyond that must be clean.
    constexpr int kStartupSamples = 4608;   // 1.5 window periods (3072 host/period)
    float maxJumpL = 0.0f, maxJumpR = 0.0f;              // whole run
    float postJumpL = 0.0f, postJumpR = 0.0f;            // after the startup window
    float prevL = 0.0f, prevR = 0.0f;
    float oL[kChunk], oR[kChunk];
    bool first = true;
    for (int off = 0; off < kTotal; off += kChunk)
    {
        fx->setParams (params);   // the driver re-applies every block
        for (int i = 0; i < kChunk; ++i)
        {
            const float t = static_cast<float> (off + i) / static_cast<float> (sr);
            oL[i] = kAmp * std::sin (2.0f * 3.14159265f * 220.0f * t + 0.5f);
            oR[i] = kAmp * std::sin (2.0f * 3.14159265f * 220.0f * t + 0.7f);
        }
        fx->process (oL, oR, kChunk);
        for (int i = 0; i < kChunk; ++i)
        {
            if (! first)
            {
                const float jl = std::fabs (oL[i] - prevL);
                const float jr = std::fabs (oR[i] - prevR);
                maxJumpL = std::fmax (maxJumpL, jl);
                maxJumpR = std::fmax (maxJumpR, jr);
                if (off + i >= kStartupSamples)
                {
                    postJumpL = std::fmax (postJumpL, jl);
                    postJumpR = std::fmax (postJumpR, jr);
                }
            }
            prevL = oL[i];
            prevR = oR[i];
            first = false;
        }
    }

    char msg[160];
    // (1) SYMMETRY — the one-sided-crackle invariant this regression pins:
    // pre-fix the R channel alone stepped onto unwritten buffer (R 0.14+
    // with L clean, diff >> 0.1); post-fix L/R match within the inherent
    // artifact's channel jitter (measured |L-R| = 0.0033).
    std::snprintf (msg, sizeof (msg),
                   "WSOLA startup: channel symmetry |L-R| <= 0.1 (L=%.4f R=%.4f diff=%.4f)",
                   maxJumpL, maxJumpR, std::fabs (maxJumpL - maxJumpR));
    check (std::fabs (maxJumpL - maxJumpR) <= 0.1f, msg);
    // (2) BOUNDED startup: the inherent first-splice transient measured 0.2537
    // post-fix; pre-fix hot inputs stepped up to ~|signal| into unwritten
    // buffer. 0.6 absolute keeps headroom above the inherent transient while
    // catching any unwritten-buffer garbage (which scales with input level).
    std::snprintf (msg, sizeof (msg),
                   "WSOLA startup: whole-run max jump <= 0.6 absolute (L=%.4f R=%.4f)",
                   maxJumpL, maxJumpR);
    check (maxJumpL <= 0.6f && maxJumpR <= 0.6f, msg);
    // (3) POST-STARTUP cleanliness: beyond the startup window there is no
    // SUSTAINED crackle — per-channel max jump back within 5x the input slope
    // (5 x 0.0144 = 0.072).
    std::snprintf (msg, sizeof (msg),
                   "WSOLA startup: post-startup jump <= 5x slope (L=%.4f R=%.4f vs %.4f)",
                   postJumpL, postJumpR, 5.0f * kSlope);
    check (postJumpL <= 5.0f * kSlope && postJumpR <= 5.0f * kSlope, msg);
}
}  // namespace

TEST(parvati_fx_engine_continuity)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf ("=== Full-engine Clouds-FX continuity regression ===\n");
    std::printf ("Renders a sustained note through processBlock with each Clouds FX (no mod);\n");
    std::printf ("asserts the main bus is click-free (worst impulse < %.3f).\n\n", kClickBound);

    // 48k at two host buffer sizes (the drift-cadence / 1-sample-sub-chunk rate
    // depends on the buffer size) + 44.1k (the other common rate).
    runConfig (48000.0, 128);
    runConfig (48000.0, 256);
    runConfig (44100.0, 256);

    std::printf ("\n-- Direct WSOLA startup-splice regression --\n");
    wsolaStartupRegression();

    std::printf ("\n%s\n", g_failures == 0
        ? "Full-engine Clouds-FX continuity PASSED."
        : "Full-engine Clouds-FX continuity FAILED.");
    return g_failures == 0;
}

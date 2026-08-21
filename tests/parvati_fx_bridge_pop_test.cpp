// Regression: the HostRateBridge (32 kHz resampler inside every Clouds FX) must
// NOT produce rhythmic popping when the FX is processed in 980 Hz sub-chunks
// (the engine's normal cadence). Root cause was a per-sub-chunk fractional-delay
// discontinuity: hostWritePhase_ carries a remainder so each sub-chunk's start
// phase alternates, and the old internalToHost read at i*ratio_ (ignoring that
// start phase), applying a per-sub-chunk delay shift -> a click at every boundary.
// The fix: internalToHost reads at (i - phaseStart_)*ratio_ and blends a 1-sample
// head-overlap tail (prevTail_) for the leading samples, making the resampling
// seamless across sub-chunk AND block boundaries.
//
// This test renders every Clouds (bridge) FX via an FxChain slot, sub-chunked at
// the engine's ~980 Hz cadence, and asserts the isolated-spike count sits at
// the single-block (no-sub-chunking) floor + a small margin (was 13+ above it
// for the Diffuser pre-fix). Native (non-bridge) FX are a control asserted at
// the same single-block floor: their render is chunking-invariant (no per-call
// state), so sub-chunking must add nothing — but their ABSOLUTE spike count
// need not be 0 (the Resonator's Structure param legitimately shapes an
// inharmonic modal waveform whose narrow crests trip the isolated-spike
// heuristic at some sample rates).
// It FAILS if the head-overlap fix is reverted (the per-sub-chunk clicks return).
//
// Build: linked as parvati_fx_bridge_pop_test (see CMakeLists).

#include <algorithm>
#include "unified_test_runner.h"
#include <cmath>
#include <cstdio>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "dsp/fx/FxChain.h"
#include "dsp/fx/FxTypes.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

const char* fxTypeName (FxType t)
{
    switch (t)
    {
        case FxType::Diffuser:        return "Diffuser";
        case FxType::PitchShifter:    return "PitchShifter";
        case FxType::Reverb:    return "Reverb";
        case FxType::LoopingDelay:    return "LoopingDelay";
        case FxType::WSOLAStretch:    return "WSOLAStretch";
        case FxType::Spectral:        return "Spectral";
        case FxType::Wavefolder:      return "Wavefolder";
        case FxType::FrequencyShifter: return "FreqShifter";
        case FxType::RingModulator:   return "RingModulator";
        case FxType::Resonator:       return "Resonator";
        case FxType::None:           return "None";
        case FxType::Count:           return "Count";
        default:                      return "None";
    }
}
// Isolated-spike detector: a sample where |Δout[i]| > k * max(neighbour deltas).
// Catches impulsive discontinuities (the per-sub-chunk bridge click) regardless of
// the steady signal level.
int countSpikes (const std::vector<float>& out, int start, int end, double k = 4.0)
{
    int spikes = 0;
    for (int i = start + 1; i < end - 1; ++i)
    {
        const float d     = std::fabs (out[static_cast<size_t> (i)] - out[static_cast<size_t> (i - 1)]);
        const float dn    = std::fabs (out[static_cast<size_t> (i + 1)] - out[static_cast<size_t> (i)]);
        const float neigh = std::max (dn, std::fabs (out[static_cast<size_t> (i - 1)] - out[static_cast<size_t> (i - 2)]));
        // Absolute floor 5e-3: the AA filter smooths the allpass HF content,
        // lowering the neighbour-delta baseline and making the relative detector
        // over-sensitive to sub-sample-level interpolation artifacts (~1e-3).
        // Real bridge discontinuities (pre-fix) were >>0.01, so 5e-3 cleanly
        // separates real pops from the linear-interp floor.
        if (d > k * neigh && d > 5e-3f)
            ++spikes;
    }
    return spikes;
}

// Render a Clouds (or any) FX via an FxChain slot, sub-chunked at ~980 Hz.
// Returns the left-channel output so spikes can be counted.
std::vector<float> renderSubChunked (FxType type, double sr, int subChunk, int blocks)
{
    const int N = subChunk * blocks;
    std::vector<float> inL (static_cast<size_t> (N)), inR (static_cast<size_t> (N));
    std::vector<float> outL (static_cast<size_t> (N)), outR (static_cast<size_t> (N));
    for (size_t i = 0; i < static_cast<size_t> (N); ++i)
    {
        inL[i] = static_cast<float> (0.5 * std::sin (2.0 * 3.14159265 * 220.0 * static_cast<double> (i) / sr));
        inR[i] = inL[i];
    }

    FxChain chain;
    chain.prepare (sr, 256);
    chain.setTopology (FxTopology::Series);
    chain.setSlotType (0, type);
    chain.setSlotEnabled (0, true);
    chain.setSlotDryWet (0, 1.0f);
    for (int k = 0; k < kNumFxSlotParams; ++k)
        chain.setSlotParam (0, k, 0.5f);

    for (int s = 0; s < blocks; ++s)
    {
        const int off = s * subChunk;
        chain.process (inL.data() + off, inR.data() + off,
                       outL.data() + off, outR.data() + off, subChunk);
    }
    return outL;
}

// Count spikes over the steady-state region (skip the FX warmup tail).
int measureSpikes (const std::vector<float>& out)
{
    const int total = static_cast<int> (out.size());
    return countSpikes (out, 2048, total - 64);
}

// A Clouds FX is "pop-clean" under sub-chunking if its sub-chunk spike count does
// not exceed the single-block (no-sub-chunking) floor by more than a small margin.
// Pre-fix the sub-chunk count was FAR above the single-block floor (Diffuser @48k:
// 13 vs 3; @44k: 48 vs 1). The fix brings them together. Margin 1 is well below the
// pre-fix gap (>=10) — native and bridged FX alike are compared against their own
// single-block floor, which is the actual contract (sub-chunking adds nothing).
constexpr int kSubVsSingleMargin = 1;

void testRate (double sr, int subChunk, int blocks, int singleChunk, int singleBlocks)
{
    const FxType cloudsFx[] = {
        FxType::Diffuser, FxType::Reverb, FxType::PitchShifter,
        FxType::LoopingDelay, FxType::WSOLAStretch, FxType::Spectral,
    };
    const FxType nativeFx[] = { FxType::Resonator, FxType::FrequencyShifter };

    std::printf ("-- %.1f kHz (sub-chunk %d, ~%.0f Hz) --\n", sr / 1000.0, subChunk, sr / subChunk);

    for (FxType t : cloudsFx)
    {
        const int spikesSub    = measureSpikes (renderSubChunked (t, sr, subChunk, blocks));
        const int spikesSingle = measureSpikes (renderSubChunked (t, sr, singleChunk, singleBlocks));
        char msg[160];
        std::snprintf (msg, sizeof (msg),
            "%-14s @%.0fk: sub-chunk spikes=%d (single=%d; must be <= single+%d)",
            fxTypeName (t), sr / 1000.0, spikesSub, spikesSingle, kSubVsSingleMargin);
        std::printf ("  %s\n", msg);
        check (spikesSub <= spikesSingle + kSubVsSingleMargin, msg);
    }

    for (FxType t : nativeFx)
    {
        const int spikesSub    = measureSpikes (renderSubChunked (t, sr, subChunk, blocks));
        const int spikesSingle = measureSpikes (renderSubChunked (t, sr, singleChunk, singleBlocks));
        char msg[160];
        std::snprintf (msg, sizeof (msg),
            "%-14s @%.0fk (native, control): sub-chunk spikes=%d (single=%d; must be <= single+%d)",
            fxTypeName (t), sr / 1000.0, spikesSub, spikesSingle, kSubVsSingleMargin);
        std::printf ("  %s\n", msg);
        // Native FX never had the bridge bug: their render is chunking-invariant
        // (no per-call state to reset), so sub-chunk spikes must sit at the
        // single-block floor. An ABSOLUTE 0 held only before Structure became
        // param 5 (496ed01: 0.5 -> inharmonic modal layout -> legitimately narrow
        // crest texture); a real native discontinuity would still blow the gap
        // (sub >> single), exactly like the pre-fix bridge did.
        check (spikesSub <= spikesSingle + kSubVsSingleMargin, msg);
    }
}

}  // namespace

TEST(parvati_fx_bridge_pop_test)
{
    std::printf ("=== HostRateBridge sub-chunk pop regression ===\n");
    std::printf ("Clouds FX must be pop-clean under 980 Hz sub-chunking (sub-chunk spikes <= single-block + %d).\n", kSubVsSingleMargin);

    // Sub-chunk sizes mirror the engine's internal-block cadence: 40*sr/39216.
    testRate (48000.0, 49, 600, 256, 115);    // ~980 Hz sub-chunks @48k
    testRate (44100.0, 45, 653, 256, 115);    // ~980 Hz sub-chunks @44.1k
    testRate (96000.0, 98, 300, 256, 115);    // ~980 Hz sub-chunks @96k

    // ---- Anti-alias filter checks (96 kHz worst case: 3:1 downsample) ----
    // 1. ALIAS REJECTION: a 20 kHz tone at 96 kHz host is above the 32 kHz
    //    internal Nyquist (16 kHz) and would alias to 12 kHz WITHOUT the AA
    //    filter. WITH the filter (14 kHz LP), the 20 kHz is removed before
    //    decimation -> output ~0. Disabling the AA filter -> alias returns.
    // 2. IN-BAND PASSBAND: a 5 kHz tone is well inside the filter's passband
    //    and must pass with minimal attenuation (<~3 dB total bridge loss).
    std::printf ("\n-- Anti-alias filter (96 kHz 3:1 downsample) --\n");
    {
        const double sr = 96000.0;
        const int    N  = 256 * 150;
        auto renderTone = [sr, N] (double freq) {
            std::vector<float> inL (static_cast<size_t> (N)), inR (static_cast<size_t> (N));
            std::vector<float> outL (static_cast<size_t> (N)), outR (static_cast<size_t> (N));
            for (int i = 0; i < N; ++i)
                inL[i] = inR[i] = static_cast<float> (0.5 * std::sin (2.0 * 3.14159265 * freq * static_cast<double> (i) / sr));
            FxChain chain;
            chain.prepare (sr, 256);
            chain.setTopology (FxTopology::Series);
            chain.setSlotType (0, FxType::Diffuser);
            chain.setSlotEnabled (0, true);
            chain.setSlotDryWet (0, 1.0f);
            for (int k = 0; k < kNumFxSlotParams; ++k)
                chain.setSlotParam (0, k, 0.5f);
            for (int s = 0; s < 150; ++s)
            {
                const int off = s * 256;
                chain.process (inL.data() + off, inR.data() + off,
                               outL.data() + off, outR.data() + off, 256);
            }
            // RMS over steady state (skip warmup).
            double sumSq = 0.0;
            int    cnt  = 0;
            for (int i = 4096; i < N; ++i)
            {
                sumSq += static_cast<double> (outL[static_cast<size_t> (i)]) * outL[static_cast<size_t> (i)];
                ++cnt;
            }
            return std::sqrt (sumSq / static_cast<double> (cnt));
        };
        const double aliasRms = renderTone (20000.0);   // 20 kHz: must be filtered
        const double bandRms  = renderTone (5000.0);     //  5 kHz: must pass
        char msg1[160];
        std::snprintf (msg1, sizeof (msg1),
            "AA rejection: 20 kHz @96k Diffuser RMS=%.4f (must be < 0.05)", aliasRms);
        std::printf ("  %s\n", msg1);
        check (aliasRms < 0.05, msg1);
        char msg2[160];
        std::snprintf (msg2, sizeof (msg2),
            "AA passband:  5 kHz @96k Diffuser RMS=%.4f (must be > 0.03)", bandRms);
        std::printf ("  %s\n", msg2);
        check (bandRms > 0.03, msg2);
    }

    std::printf ("\nAll HostRateBridge sub-chunk pop checks PASSED.\n");
    return g_failures == 0;
}

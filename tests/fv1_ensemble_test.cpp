// Standalone verification of Fv1Ensemble (BBD-style ensemble chorus).
// JUCE-FREE: compiles in seconds with
//   clang++ -std=c++17 -O2 -Wall -Wextra -fsanitize=address,undefined \
//     -I Source tests/fv1_ensemble_test.cpp Source/dsp/fx/fv1/Fv1Ensemble.cpp \
//     -o /tmp/fv1_ensemble_test && /tmp/fv1_ensemble_test

#include <array>
#include <cmath>
#include "unified_test_runner.h"
#include <cstdio>

#include "dsp/fx/fv1/Fv1Ensemble.h"

namespace fv1 = parvati::fv1;

namespace
{
int g_fail = 0;
void check (bool ok, const char* m)
{
    std::printf ("  %s: %s\n", ok ? "ok  " : "FAIL", m);
    if (! ok) ++g_fail;
}

// The RateBridge sizes its internal buffers from the maxBlock passed to
// prepare(), so process() must be driven in blocks no larger than that.
constexpr int kMaxBlock = 256;

// Render `n` mono samples (duplicated to L==R) through `fx` into L/R, in
// kMaxBlock-sized chunks (mirrors how the FX chain drives the processor).
void run (fv1::Fv1Ensemble& fx, const float* in, int n, float* L, float* R)
{
    for (int i = 0; i < n; ++i) { L[i] = in[i]; R[i] = in[i]; }
    for (int off = 0; off < n; off += kMaxBlock)
    {
        const int m = (n - off) < kMaxBlock ? (n - off) : kMaxBlock;
        fx.process (L + off, R + off, m);
    }
}
} // namespace

TEST(fv1_ensemble_test)
{
    constexpr int n = 1024;
    static float inBuf[n];
    static float L[n];
    static float R[n];

    fv1::Fv1Ensemble fx;
    fx.prepare (48000.0, 256);
    fx.reset();

    // ---- 1) Sine input: finite + effect applied ----
    {
        for (int i = 0; i < n; ++i)
            inBuf[i] = 0.5f * std::sin (6.28318530718f * 440.0f
                                        * static_cast<float> (i) / 48000.0f);

        // Mid chorus: rate ~2.15 Hz, depth ~5 ms, center ~5 ms, feedback +0.18.
        std::array<float, kNumFxSlotParams> p = { 0.5f, 0.33f, 0.13f, 0.6f, 0.0f };
        fx.setParams (p);
        run (fx, inBuf, n, L, R);

        bool finite = true;
        for (int i = 0; i < n; ++i)
            if (! std::isfinite (L[i]) || ! std::isfinite (R[i])) { finite = false; break; }
        check (finite, "sine: all outputs finite");

        // Effect applied: late-block wet must differ from the dry input.
        bool differs = false;
        for (int i = 512; i < n; ++i)
            if (std::fabs (L[i] - inBuf[i]) > 1e-3f || std::fabs (R[i] - inBuf[i]) > 1e-3f)
            { differs = true; break; }
        check (differs, "sine: wet output differs from input (effect applied)");
    }

    // ---- 2) Impulse train: finite at the extremes (all-0 and all-1) ----
    {
        // Impulse train: a click every 64 samples.
        for (int i = 0; i < n; ++i)
            inBuf[i] = (i % 64 == 0) ? 0.9f : 0.0f;

        std::array<std::array<float, kNumFxSlotParams>, 2> extremes = {{
            { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },  // rate 0.1 Hz, depth 0, center 2 ms, fb -0.9
            { 1.0f, 1.0f, 1.0f, 1.0f, 0.0f }   // rate 8 Hz, depth 15 ms, center 25 ms, fb +0.9
        }};
        for (int e = 0; e < 2; ++e)
        {
            fx.reset();
            fx.setParams (extremes[static_cast<size_t> (e)]);
            run (fx, inBuf, n, L, R);
            bool finite = true;
            for (int i = 0; i < n; ++i)
                if (! std::isfinite (L[i]) || ! std::isfinite (R[i])) { finite = false; break; }
            check (finite, e == 0 ? "impulse train @ all-0: finite"
                                  : "impulse train @ all-1: finite");
        }
    }

    // ---- 3) Effect-specific: the 90-deg LFO offset gives stereo divergence
    //         ONLY when depth > 0 (depth==0 reads the same delay in both lines) ----
    {
        for (int i = 0; i < n; ++i)
            inBuf[i] = 0.4f * std::sin (6.28318530718f * 220.0f
                                        * static_cast<float> (i) / 48000.0f);

        // Depth = 0: both lines read the identical center delay, so L tracks R.
        std::array<float, kNumFxSlotParams> pMono = { 0.7f, 0.0f, 0.3f, 0.5f, 0.0f };
        fx.reset();
        fx.setParams (pMono);
        run (fx, inBuf, n, L, R);
        float maxDiff = 0.0f;
        for (int i = 256; i < n; ++i)
            maxDiff = std::fmax (maxDiff, std::fabs (L[i] - R[i]));
        check (maxDiff < 1e-2f, "depth=0: L and R track (mono)");

        // Depth > 0: the 90-deg offset makes the two reads diverge (stereo).
        std::array<float, kNumFxSlotParams> pStereo = { 0.7f, 0.7f, 0.3f, 0.5f, 0.0f };
        fx.reset();
        fx.setParams (pStereo);
        run (fx, inBuf, n, L, R);
        maxDiff = 0.0f;
        for (int i = 256; i < n; ++i)
            maxDiff = std::fmax (maxDiff, std::fabs (L[i] - R[i]));
        check (maxDiff > 1e-3f, "depth>0: L and R diverge (90-deg stereo)");
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_fail ? "FV1 ENSEMBLE TEST: FAILURES"
                        : "FV1 ENSEMBLE TEST: ALL CHECKS PASSED",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0;
}

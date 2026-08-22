// Standalone verification of the FV-1 6-stage Phaser (Fv1Phaser).
// JUCE-FREE: compiles in seconds with
//   clang++ -std=c++17 -O2 -Wall -Wextra -fsanitize=address,undefined \
//     -I Source tests/fv1_phaser_test.cpp Source/dsp/fx/fv1/Fv1Phaser.cpp \
//     -o /tmp/fv1_phaser_test && /tmp/fv1_phaser_test
// No JUCE link needed (the framework + this effect are std-only).
//
// NOTE: prepare(48000.0, 256) sizes the RateBridge internal buffers to 256, so
// every process() call uses a block of exactly 256 host samples (== maxBlock).

#include <array>
#include <cmath>
#include "unified_test_runner.h"
#include <cstdio>

#include "dsp/fx/fv1/Fv1Phaser.h"

namespace fv1 = parvati::fv1;

namespace
{
int g_fail = 0;
void check (bool ok, const char* m)
{
    std::printf ("  %s: %s\n", ok ? "ok  " : "FAIL", m);
    if (! ok) ++g_fail;
}
bool approx (float a, float b, float tol) { return std::fabs (a - b) <= tol; }

constexpr int kBlock = 256;

// Render `nBlocks` of a sine into out[], running the effect in maxBlock chunks.
// Returns true if every sample is finite. dryAt(i) writes the unprocessed sine.
template <typename DryFn>
bool runSine (fv1::Fv1Phaser& fx, float* out, int nBlocks, DryFn dryAt)
{
    bool finite = true;
    float L[kBlock], R[kBlock];
    for (int b = 0; b < nBlocks; ++b)
    {
        for (int i = 0; i < kBlock; ++i)
        {
            const int idx = b * kBlock + i;
            const float v = dryAt (idx);
            L[i] = v;
            R[i] = v;
        }
        fx.process (L, R, kBlock);
        for (int i = 0; i < kBlock; ++i)
        {
            out[b * kBlock + i] = L[i];
            if (! std::isfinite (L[i]) || ! std::isfinite (R[i]))
                finite = false;
        }
    }
    return finite;
}
} // namespace

TEST(fv1_phaser_test)
{
    fv1::Fv1Phaser fx;

    // ---- type() ----
    check (fx.type() == FxType::Phaser, "type() == FxType::Phaser");

    // ---- prepare / reset ----
    fx.prepare (48000.0, 256);
    fx.reset();

    constexpr int kN = 8 * kBlock;   // 2048 samples total

    // Several settings including the param extremes (0 and 1 where valid).
    const std::array<float, kNumFxSlotParams> settings[5] = {
        { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },   // all min: rate 0.1 Hz, depth 0, fb -0.9, center 200
        { 0.5f, 0.5f, 0.5f, 0.5f, 0.0f },   // mid
        { 1.0f, 1.0f, 1.0f, 1.0f, 0.0f },   // all max: rate 8 Hz, depth 1500, fb 0.9, center 2000
        { 0.7f, 1.0f, 0.5f, 0.3f, 0.0f },   // fast rate, full depth
        { 0.0f, 0.0f, 0.5f, 1.0f, 0.0f }    // min rate/depth, max center
    };

    bool allFinite = true;
    bool effectApplies = false;   // at some wet setting output differs from input

    auto sineDry = [] (int idx)
    {
        return 0.5f * std::sin (6.28318530718f * 220.0f * static_cast<float> (idx) / 48000.0f);
    };

    for (const auto& s : settings)
    {
        fx.reset();
        fx.setParams (s);

        static float out[kN];
        if (! runSine (fx, out, kN / kBlock, sineDry))
            allFinite = false;

        // Did this setting change the signal (after the input BW settles)?
        float maxDiff = 0.0f;
        for (int i = 2 * kBlock; i < kN; ++i)
            maxDiff = std::fmax (maxDiff, std::fabs (out[i] - sineDry (i)));
        if (maxDiff > 1e-3f) effectApplies = true;

        // Impulse train input (one impulse every 64 samples), in 256-blocks.
        fx.reset();
        bool impFinite = true;
        {
            float L[kBlock], R[kBlock];
            for (int b = 0; b < kN / kBlock; ++b)
            {
                for (int i = 0; i < kBlock; ++i)
                {
                    const int idx = b * kBlock + i;
                    const float v = (idx % 64 == 0) ? 0.8f : 0.0f;
                    L[i] = v;
                    R[i] = v;
                }
                fx.process (L, R, kBlock);
                for (int i = 0; i < kBlock; ++i)
                    if (! std::isfinite (L[i]) || ! std::isfinite (R[i]))
                        impFinite = false;
            }
        }
        if (! impFinite) allFinite = false;
    }
    check (allFinite, "all outputs finite (sine + impulse train, 5 settings)");
    check (effectApplies, "output differs from input at some wet setting (effect applied)");

    // ---- Effect-specific sanity 1: feedback near +0.9 must NOT blow up.
    //      The hard-clip feedback path mathematically bounds the internal signal,
    //      so we require: every sample finite, peak bounded, and the late-block
    //      peak no larger than the early-block peak (no runaway growth).
    {
        fx.reset();
        const std::array<float, kNumFxSlotParams> p = { 0.5f, 1.0f, 1.0f, 0.0f, 0.0f }; // fb = +0.9 (max)
        fx.setParams (p);
        bool finite = true;
        constexpr int kBlks = 8;
        float blockMax[kBlks] = {};
        for (int b = 0; b < kBlks; ++b)
        {
            float L[kBlock], R[kBlock];
            for (int i = 0; i < kBlock; ++i)
            {
                const int idx = b * kBlock + i;
                const float v = 0.5f * std::sin (6.28318530718f * 440.0f
                                             * static_cast<float> (idx) / 48000.0f);
                L[i] = v;
                R[i] = v;
            }
            fx.process (L, R, kBlock);
            for (int i = 0; i < kBlock; ++i)
            {
                if (! std::isfinite (L[i])) finite = false;
                blockMax[b] = std::fmax (blockMax[b], std::fabs (L[i]));
            }
        }
        // The saturated internal signal + output BW-limit overshoots only
        // marginally past 1.0; a true runaway would go to inf / keep growing.
        float globalMax = 0.0f;
        for (int b = 0; b < kBlks; ++b) globalMax = std::fmax (globalMax, blockMax[b]);
        check (finite, "feedback=+0.9: every sample finite");
        check (globalMax < 1.1f, "feedback=+0.9: peak bounded (no runaway to inf)");
        check (blockMax[kBlks - 1] <= blockMax[0] * 1.05f + 1e-3f,
               "feedback=+0.9: late-block peak not growing (stable)");
    }

    // ---- Effect-specific sanity 2: depth=0, fb=0 -> static notch sweep; the
    //      output must be deterministic across identical runs AND still differ
    //      from the dry signal (the allpass cascade still shapes the spectrum).
    {
        const std::array<float, kNumFxSlotParams> p = { 0.0f, 0.0f, 0.0f, 0.5f, 0.0f }; // depth=0, fb=0, center ~632 Hz

        auto dry330 = [] (int idx)
        {
            return 0.4f * std::sin (6.28318530718f * 330.0f * static_cast<float> (idx) / 48000.0f);
        };

        static float run1[kN];
        fx.reset();
        fx.setParams (p);
        runSine (fx, run1, kN / kBlock, dry330);

        static float run2[kN];
        fx.reset();
        fx.setParams (p);
        runSine (fx, run2, kN / kBlock, dry330);

        bool deterministic = true;
        for (int i = 0; i < kN; ++i)
            if (! approx (run2[i], run1[i], 1e-7f)) deterministic = false;
        check (deterministic, "depth=0 output is deterministic across identical runs");

        float diff = 0.0f;
        for (int i = 2 * kBlock; i < kN; ++i)
            diff = std::fmax (diff, std::fabs (run1[i] - dry330 (i)));
        check (diff > 1e-3f, "depth=0 still shapes the signal (notch filtering)");
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_fail ? "FV1 PHASER TEST: FAILURES" : "FV1 PHASER TEST: ALL CHECKS PASSED",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0;
}

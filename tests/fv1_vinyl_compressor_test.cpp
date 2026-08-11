// Standalone verification of Fv1VinylCompressor. JUCE-FREE: compiles in seconds
// with
//   clang++ -std=c++17 -O2 -Wall -Wextra -fsanitize=address,undefined \
//     -I Source tests/fv1_vinyl_compressor_test.cpp \
//     Source/dsp/fx/fv1/Fv1VinylCompressor.cpp -o /tmp/fv1_vinyl_compressor_test \
//     && /tmp/fv1_vinyl_compressor_test
// No JUCE link needed (the framework is <array>/<cmath>/<cstdint>/<vector> only).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "dsp/fx/fv1/Fv1VinylCompressor.h"

namespace fv1 = parvati::fv1;

namespace
{
int g_fail = 0;
void check (bool ok, const char* m)
{
    std::printf ("  %s: %s\n", ok ? "ok  " : "FAIL", m);
    if (! ok) ++g_fail;
}

// Set params, copy in -> out, then process in place in maxBlock-sized chunks
// (the RateBridge sizes its internal buffers from maxBlock, so no single
// process() call may exceed the block size passed to prepare(); the base reads
// all of a chunk into its internal buffers first, so in-place is safe).
constexpr int kBlock = 256;
void runSetting (fv1::Fv1VinylCompressor& fx, float c, float p, float k, float age,
                 const float* inL, const float* inR, int n, float* outL, float* outR)
{
    float prm[5] = { c, p, k, age, 0.0f };   // param[4] unused (chain dry/wet)
    fx.setParams (prm);
    std::memcpy (outL, inL, sizeof (float) * static_cast<size_t> (n));
    std::memcpy (outR, inR, sizeof (float) * static_cast<size_t> (n));
    for (int i = 0; i < n; i += kBlock)
        fx.process (outL + i, outR + i, std::min (kBlock, n - i));
}
} // namespace

int main()
{
    fv1::Fv1VinylCompressor fx;
    fx.prepare (48000.0, 256);
    fx.reset();
    check (fx.type() == FxType::VinylCompressor, "type() == VinylCompressor");

    constexpr int kN = 2048;

    // 1 kHz sine at 48k (well under the 15 kHz BW limit) duplicated L==R.
    float inL[kN], inR[kN];
    for (int i = 0; i < kN; ++i)
    {
        const float w = 6.28318530718f * 1000.0f;
        const float v = 0.5f * std::sin (w * static_cast<float> (i) / 48000.0f);
        inL[i] = v;
        inR[i] = v;
    }

    // ---- Mid settings: finite + differs from input (effect is applied) ----
    {
        float oL[kN], oR[kN];
        runSetting (fx, 0.5f, 0.5f, 0.5f, 0.5f, inL, inR, kN, oL, oR);
        bool finite = true, differs = false;
        for (int i = 0; i < kN; ++i)
        {
            if (! std::isfinite (oL[i]) || ! std::isfinite (oR[i])) finite = false;
            if (std::fabs (oL[i] - inL[i]) > 1e-4f) differs = true;
        }
        check (finite, "mid settings: all outputs finite");
        check (differs, "mid settings: output differs from input (effect applied)");
    }

    // ---- Extremes 0 and 1: every output must remain finite ----
    {
        float oL[kN], oR[kN];
        for (float c : { 0.0f, 1.0f })
            for (float p : { 0.0f, 1.0f })
                for (float k : { 0.0f, 1.0f })
                    for (float a : { 0.0f, 1.0f })
                    {
                        runSetting (fx, c, p, k, a, inL, inR, kN, oL, oR);
                        bool finite = true;
                        for (int i = 0; i < kN; ++i)
                            if (! std::isfinite (oL[i]) || ! std::isfinite (oR[i]))
                                finite = false;
                        check (finite, "extreme setting: outputs finite");
                    }
    }

    // ---- Impulse train: finite + nonzero ----
    {
        float impL[kN], impR[kN], oL[kN], oR[kN];
        for (int i = 0; i < kN; ++i)
        {
            const float v = (i % 64 == 0) ? 0.8f : 0.0f;
            impL[i] = v;
            impR[i] = v;
        }
        runSetting (fx, 0.7f, 0.3f, 0.2f, 0.4f, impL, impR, kN, oL, oR);
        bool finite = true, nonzero = false;
        for (int i = 0; i < kN; ++i)
        {
            if (! std::isfinite (oL[i]) || ! std::isfinite (oR[i])) finite = false;
            if (std::fabs (oL[i]) > 1e-5f) nonzero = true;
        }
        check (finite, "impulse train: outputs finite");
        check (nonzero, "impulse train: produces nonzero output");
    }

    // ---- Effect-specific: compressor tames a loud steady signal ----
    // Strong Compress (th=0.1, makeup=2.0) on a 0.9 DC. After the 1638-sample
    // delay fills, the steady-state output is well below the 0.9 input.
    {
        fx.reset();
        constexpr int M = 4096;
        float loudL[M], loudR[M], oL[M], oR[M];
        for (int i = 0; i < M; ++i) { loudL[i] = 0.9f; loudR[i] = 0.9f; }
        runSetting (fx, 1.0f, 0.0f, 0.0f, 1.0f, loudL, loudR, M, oL, oR);
        float sumIn = 0.0f, sumOut = 0.0f;
        for (int i = M - 200; i < M; ++i)
        {
            sumIn += std::fabs (loudL[i]);
            sumOut += std::fabs (oL[i]);
        }
        const float meanIn = sumIn / 200.0f;
        const float meanOut = sumOut / 200.0f;
        check (meanOut < meanIn, "compressor reduces a loud steady signal");
        check (meanOut > 0.05f, "compressed signal still passes (not zeroed)");
        check (meanOut <= 1.0f + 1e-3f, "compressed output stays in range");
    }

    // ---- Effect-specific: crackle injects clicks on a silent input ----
    // With silence in, only the crackle path can produce output. Crackle=1 yields
    // nonzero clicks; Crackle=0 yields (near) silence. (reset() empties the delay
    // so no residual tail leaks into the silent-input check.)
    {
        fx.reset();
        float silL[kN], silR[kN], o0[kN], o1[kN];
        for (int i = 0; i < kN; ++i) { silL[i] = 0.0f; silR[i] = 0.0f; }

        runSetting (fx, 0.0f, 0.0f, 0.0f, 1.0f, silL, silR, kN, o0, o0); // no crackle
        float maxNoCrackle = 0.0f;
        for (int i = 0; i < kN; ++i)
            maxNoCrackle = std::max (maxNoCrackle, std::fabs (o0[i]));

        runSetting (fx, 0.0f, 0.0f, 1.0f, 1.0f, silL, silR, kN, o1, o1); // full crackle
        float maxCrackle = 0.0f;
        for (int i = 0; i < kN; ++i)
            maxCrackle = std::max (maxCrackle, std::fabs (o1[i]));

        check (maxNoCrackle < 1e-4f, "silent input, no crackle -> ~silent output");
        check (maxCrackle > 1e-3f, "silent input, full crackle -> clicks present");
    }

    // ---- Makeup gain restores level under heavy compression ----
    // At full Compression the threshold drops to 0.1 and makeup rises to 4.0
    // (docs/FX_FV1_DESIGN.md), so a loud input is compressed then boosted BACK
    // ABOVE its no-compression level — proving the fixed-point gain path supports
    // g>1 (the integer-shift decomposition), not just attenuation.
    {
        float oL0[kN], oR0[kN], oL1[kN], oR1[kN];
        runSetting (fx, 0.0f, 0.0f, 0.0f, 1.0f, inL, inR, kN, oL0, oR0); // no compression
        runSetting (fx, 1.0f, 0.0f, 0.0f, 1.0f, inL, inR, kN, oL1, oR1); // full comp + makeup 4.0
        float peak0 = 0.0f, peak1 = 0.0f;
        for (int i = 0; i < kN; ++i)
        {
            peak0 = std::max (peak0, std::fabs (oL0[i]));
            peak1 = std::max (peak1, std::fabs (oL1[i]));
        }
        check (peak1 > peak0 + 0.05f,
               "full Compression boosts level (makeup > compression reduction)");
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_fail ? "FV1 VINYL COMPRESSOR TEST: FAILURES"
                        : "FV1 VINYL COMPRESSOR TEST: ALL CHECKS PASSED",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}

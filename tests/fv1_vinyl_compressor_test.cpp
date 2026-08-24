// Standalone verification of Fv1VinylCompressor. JUCE-FREE: compiles in seconds
// with
//   clang++ -std=c++17 -O2 -Wall -Wextra -fsanitize=address,undefined \
//     -I Source tests/fv1_vinyl_compressor_test.cpp \
//     Source/dsp/fx/fv1/Fv1VinylCompressor.cpp -o /tmp/fv1_vinyl_compressor_test \
//     && /tmp/fv1_vinyl_compressor_test
// No JUCE link needed (the framework is <array>/<cmath>/<cstdint>/<vector> only).

#include <array>
#include <algorithm>
#include "unified_test_runner.h"
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
    std::array<float, kNumFxSlotParams> prm = { c, p, k, age, 0.0f };   // param[4] unused (chain dry/wet)
    fx.setParams (prm);
    std::memcpy (outL, inL, sizeof (float) * static_cast<size_t> (n));
    std::memcpy (outR, inR, sizeof (float) * static_cast<size_t> (n));
    for (int i = 0; i < n; i += kBlock)
        fx.process (outL + i, outR + i, std::min (kBlock, n - i));
}
} // namespace

TEST(fv1_vinyl_compressor_test)
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
    // Strong Compress on a 0.9 DC. After the 1638-sample delay fills, the
    // steady-state output is well below the 0.9 input.
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

    // ---- Effect-specific (SP-404 semantics): loud DOWN, quiet UP, hard ----
    // The SP-303/404 "Vinyl Sim COMP" is a mastering-to-vinyl feel: high
    // levels pulled down, quiet material pushed up (heavy makeup), the
    // dynamic ratio crushed. Two steady inputs (0.9 and 0.06) at FULL
    // Compress: the 15:1 input-level ratio must collapse to <= ~1:3.5 at
    // the output, and the quiet signal must come out several times louder
    // than it went in.
    {
        constexpr int M = 8192;   // > delay fill + release settle
        auto steady = [] (float* l, float* r, float amp)
        {
            for (int i = 0; i < M; ++i) { l[i] = amp; r[i] = amp; }
        };
        float bufL[M], bufR[M], obufL[M], obufR[M];
        steady (bufL, bufR, 0.9f);
        fx.reset();
        runSetting (fx, 1.0f, 0.0f, 0.0f, 0.0f, bufL, bufR, M, obufL, obufR);   // Age bright
        float outLoud = 0.0f;
        for (int i = M - 200; i < M; ++i) outLoud += std::fabs (obufL[i]);
        outLoud /= 200.0f;

        steady (bufL, bufR, 0.06f);
        fx.reset();
        runSetting (fx, 1.0f, 0.0f, 0.0f, 0.0f, bufL, bufR, M, obufL, obufR);
        float outQuiet = 0.0f;
        for (int i = M - 200; i < M; ++i) outQuiet += std::fabs (obufL[i]);
        outQuiet /= 200.0f;

        check (outQuiet > 0.06f * 1.5f,
               "SP squash: quiet material is pushed UP by the makeup gain");
        {
            char m[128];
            std::snprintf (m, sizeof (m),
                           "SP squash: 15:1 input ratio crushed (out ratio %.2f <= 3.5)",
                           outQuiet > 1e-6f ? outLoud / outQuiet : 999.0f);
            check (outQuiet > 1e-6f && outLoud / outQuiet <= 3.5f, m);
        }
    }

    // ---- Effect-specific: wow/flutter is AUDIBLE (the SP warble) ----
    // A 1 kHz sine through FULL Wow/Flut must show real frequency modulation:
    // measure the output period between successive rising zero crossings over
    // a window spanning the 2.5 s wow period; the peak-to-peak period swing
    // must be >= 1.5 % (the depth-scaled FM at 300 samples = ~4.6 % p-p).
    // The pre-tune 24-sample wow measured ~0.36 % p-p — SUB-AUDIBLE (slow-FM
    // hearing threshold ~0.3-0.5 %); this check fails on those depths.
    {
        fx.reset();
        constexpr double dur = 6.0;                       // > 2 wow periods
        const int durSamples = static_cast<int> (dur * 48000.0);
        std::vector<float> bufL (static_cast<size_t> (durSamples)), bufR (static_cast<size_t> (durSamples));
        std::vector<float> obufL (static_cast<size_t> (durSamples)), obufR (static_cast<size_t> (durSamples));
        for (int i = 0; i < durSamples; ++i)
        {
            const float v = 0.5f * std::sin (6.28318530718f * 1000.0f
                                             * static_cast<float> (i) / 48000.0f);
            bufL[static_cast<size_t> (i)] = v;
            bufR[static_cast<size_t> (i)] = v;
        }
        runSetting (fx, 0.0f, 1.0f, 0.0f, 1.0f, bufL.data(), bufR.data(), durSamples,
                    obufL.data(), obufR.data());   // compress off / wow full / crackle off / age bright

        // Rising zero crossings (linear-interpolated) after the delay fills
        // (~2400 host samples); periods = successive crossing spacings.
        std::vector<float> periods;
        float prevX = -1.0f;
        for (int i = 1; i < durSamples; ++i)
        {
            const float a = obufL[static_cast<size_t> (i - 1)];
            const float b = obufL[static_cast<size_t> (i)];
            if (a <= 0.0f && b > 0.0f)
            {
                const float x = static_cast<float> (i - 1) - a / (b - a);   // crossing pos
                if (prevX > 0.0f && (x - prevX) > 24.0f && (x - prevX) < 72.0f)
                    periods.push_back (x - prevX);
                prevX = x;
            }
        }
        float mn = 1e9f, mx = 0.0f, sum = 0.0f;
        for (float p : periods) { mn = std::min (mn, p); mx = std::max (mx, p); sum += p; }
        const float swing = periods.size () > 64 ? (mx - mn) / (sum / static_cast<float> (periods.size ())) : 0.0f;
        {
            char m[128];
            std::snprintf (m, sizeof (m),
                           "wow/flutter is AUDIBLE: period p-p swing %.2f%% >= 1.5%% (n=%zu)",
                           swing * 100.0, periods.size ());
            check (periods.size () > 64 && swing >= 0.015f, m);
        }
    }
    // ---- Effect-specific: saturation "lathe" adds character ----
    // A hot input at full Compress must come out SOFT-clipped (bounded well
    // below the linear-scaled level would clip, i.e. flat-topped, not
    // hard-square) and DIFFER from a pure linear gain: harmonic content.
    // Measured: 0.5-amp sine at full compress -> sat region engaged.
    {
        fx.reset();
        constexpr int M = 4096;
        float hotL[M], hotR[M], oL[M], oR[M];
        for (int i = 0; i < M; ++i)
        {
            const float v = 0.8f * std::sin (6.28318530718f * 220.0f
                                             * static_cast<float> (i) / 48000.0f);
            hotL[i] = v; hotR[i] = v;
        }
        runSetting (fx, 1.0f, 0.0f, 0.0f, 0.0f, hotL, hotR, M, oL, oR);
        float peak = 0.0f;
        int flatTops = 0;
        for (int i = 0; i < M; ++i)
        {
            const float a = std::fabs (oL[i]);
            if (a > peak) peak = a;
            if (a > 0.95f * peak && a > 0.2f) ++flatTops;
        }
        check (peak <= 1.0f + 1e-3f, "saturation: output bounded");
        check (flatTops > 16,
               "saturation: soft flat-topping present (character), not linear");
    }

    // ---- Effect-specific: crackle is a SUBTLE vinyl noise floor ----
    // With silence in, only the noise path can produce output. Crackle=0 ->
    // ~silence; Crackle=1 -> ticks + hiss present BUT BOUNDED: the loudest
    // tick must stay well under the old full-scale behavior (ceiling ~0.18)
    // and hiss stays a low background (<< tick level).
    {
        fx.reset();
        constexpr int M = 8192;   // enough samples for ~50 ticks at 0.6 % density
        float silL[M], silR[M], o0[M], o1[M];
        for (int i = 0; i < M; ++i) { silL[i] = 0.0f; silR[i] = 0.0f; }

        runSetting (fx, 0.0f, 0.0f, 0.0f, 1.0f, silL, silR, M, o0, o0); // no crackle
        float maxNoCrackle = 0.0f;
        for (int i = 0; i < M; ++i)
            maxNoCrackle = std::max (maxNoCrackle, std::fabs (o0[i]));

        runSetting (fx, 0.0f, 0.0f, 1.0f, 1.0f, silL, silR, M, o1, o1); // full crackle
        float maxCrackle = 0.0f;
        double sumSq = 0.0;   // double accumulator: the products are double
        int ticks = 0;
        for (int i = 1; i < M; ++i)
        {
            const float a0 = std::fabs (o1[i - 1]);
            const float a = std::fabs (o1[i]);
            if (a > maxCrackle) maxCrackle = a;
            sumSq += static_cast<double> (o1[static_cast<size_t> (i)]) * o1[static_cast<size_t> (i)];
            if (a > 0.01f && a0 <= 0.01f) ++ticks;   // rising edge above the hiss floor
        }

        check (maxNoCrackle < 1e-4f, "silent input, no crackle -> ~silent output");
        check (maxCrackle > 1e-3f, "silent input, full crackle -> ticks present");
        {
            char m[96];
            std::snprintf (m, sizeof (m),
                           "crackle stays SUBTLE (max tick %.3f < 0.25; was full-scale)", maxCrackle);
            check (maxCrackle < 0.25f, m);
        }
        check (ticks >= 8, "crackle density: multiple ticks over the window");
        {
            char m[96];
            std::snprintf (m, sizeof (m),
                           "hiss stays a low floor (rms %.5f < 0.01)",
                           std::sqrt (sumSq / static_cast<double> (M)));
            check (std::sqrt (sumSq / static_cast<double> (M)) < 0.01, m);
        }
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_fail ? "FV1 VINYL COMPRESSOR TEST: FAILURES"
                        : "FV1 VINYL COMPRESSOR TEST: ALL CHECKS PASSED",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0;
}

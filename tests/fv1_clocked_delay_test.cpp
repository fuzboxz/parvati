// Standalone verification of parvati::fv1::Fv1ClockedDelay (the FV-1 DAW-synced
// clocked delay). JUCE-FREE: compiles in seconds with
//   clang++ -std=c++17 -O2 -Wall -Wextra -fsanitize=address,undefined \
//     -I Source tests/fv1_clocked_delay_test.cpp \
//     Source/dsp/fx/fv1/Fv1ClockedDelay.cpp -o /tmp/fv1_clocked_delay_test \
//   && /tmp/fv1_clocked_delay_test

#include <array>
#include <algorithm>
#include "unified_test_runner.h"
#include <cmath>
#include <cstdio>

#include "dsp/fx/fv1/Fv1ClockedDelay.h"

namespace fv1 = parvati::fv1;

namespace
{
int g_fail = 0;

void check (bool ok, const char* m)
{
    std::printf ("  %s: %s\n", ok ? "ok  " : "FAIL", m);
    if (! ok) ++g_fail;
}

void setAll (fv1::Fv1ClockedDelay& fx, float sync, float fb, float age, float grit)
{
    std::array<float, kNumFxSlotParams> p = { sync, fb, age, grit, 0.0f };   // param[4] unused
    fx.setParams (p);
}

// Process a buffer in prepared-sized (256) chunks. The RateBridge is stateful
// across calls, so this is equivalent to one long block but never exceeds the
// maxBlock the effect was prepared for.
void processChunked (fv1::Fv1ClockedDelay& fx, float* L, float* R, int n)
{
    for (int i = 0; i < n; i += 256)
    {
        const int len = std::min (256, n - i);
        fx.process (L + i, R + i, len);
    }
}
} // namespace

TEST(fv1_clocked_delay_test)
{
    constexpr int kPreparedBlock = 256;

    // ---- type() ----
    {
        fv1::Fv1ClockedDelay fx;
        check (fx.type() == FxType::ClockedDelay, "type() == FxType::ClockedDelay");
    }

    // ---- finite outputs over a sine + impulse train at several settings ----
    {
        fv1::Fv1ClockedDelay fx;
        fx.prepare (48000.0, kPreparedBlock);
        fx.reset();
        fx.setTransport (120.0, true);

        struct Setting { float s, f, a, g; const char* name; };
        const Setting settings[] = {
            { 0.0f,   0.0f, 0.0f, 0.0f, "all zero" },
            { 0.5f,   0.5f, 0.5f, 0.5f, "mid" },
            { 1.0f,   1.0f, 1.0f, 1.0f, "all max" },
            { 0.857f, 0.0f, 0.0f, 1.0f, "grit max, fb0" }
        };

        constexpr int n = 1024;
        for (const auto& st : settings)
        {
            setAll (fx, st.s, st.f, st.a, st.g);

            // Sine.
            float L[n], R[n];
            for (int i = 0; i < n; ++i)
            {
                const float v = 0.4f
                              * std::sin (6.28318530718f * 220.0f
                                            * static_cast<float> (i) / 48000.0f);
                L[i] = v;
                R[i] = v;
            }
            processChunked (fx, L, R, n);
            bool finite = true;
            for (int i = 0; i < n; ++i)
                if (! std::isfinite (L[i]) || ! std::isfinite (R[i]))
                    finite = false;
            check (finite, "finite output (sine)");

            // Impulse train.
            for (int i = 0; i < n; ++i)
            {
                const float v = (i % 64 == 0) ? 0.8f : 0.0f;
                L[i] = v;
                R[i] = v;
            }
            processChunked (fx, L, R, n);
            finite = true;
            for (int i = 0; i < n; ++i)
                if (! std::isfinite (L[i]) || ! std::isfinite (R[i]))
                    finite = false;
            check (finite, "finite output (impulse train)");
        }
    }

    // ---- output differs from input (effect is applied) ----
    {
        fv1::Fv1ClockedDelay fx;
        fx.prepare (48000.0, kPreparedBlock);
        fx.reset();
        fx.setTransport (120.0, true);
        setAll (fx, 0.5f, 0.4f, 0.3f, 0.3f);

        constexpr int n = 1024;
        float L[n], R[n], Lin[n], Rin[n];
        for (int i = 0; i < n; ++i)
        {
            const float v = 0.4f
                          * std::sin (6.28318530718f * 440.0f
                                        * static_cast<float> (i) / 48000.0f);
            L[i] = R[i] = v;
            Lin[i] = Rin[i] = v;
        }
        processChunked (fx, L, R, n);
        bool differs = false;
        for (int i = 0; i < n; ++i)
            if (std::fabs (L[i] - Lin[i]) > 1e-4f || std::fabs (R[i] - Rin[i]) > 1e-4f)
                differs = true;
        check (differs, "output differs from input (effect applied)");
    }

    // ---- effect-specific 1: feedback increases the echo energy ----
    {
        constexpr int n = 16384;
        auto impulseEnergy = [] (float fb) -> double {
            fv1::Fv1ClockedDelay fx;
            fx.prepare (48000.0, kPreparedBlock);
            fx.reset();
            fx.setTransport (240.0, true);      // shorter delays -> echo inside block
            setAll (fx, 1.0f, fb, 0.0f, 0.0f);  // 1/16, no age/grit
            float L[n], R[n];
            for (int i = 0; i < n; ++i) { L[i] = 0.0f; R[i] = 0.0f; }
            L[0] = R[0] = 0.9f;                 // single impulse
            processChunked (fx, L, R, n);
            double e = 0.0;
            for (int i = 0; i < n; ++i)
                e += static_cast<double> (L[i]) * L[i];
            return e;
        };
        const double eNoFb  = impulseEnergy (0.0f);
        const double eMaxFb = impulseEnergy (1.0f);
        std::printf ("  [info] impulse energy: fb0=%.4e  fbMax=%.4e\n", eNoFb, eMaxFb);
        check (eMaxFb > eNoFb * 1.5, "feedback increases echo energy");
    }

    // ---- effect-specific 2: clocked delay length tracks the host BPM ----
    {
        constexpr int n = 16384;
        // pSync selects index 6 -> divisor 12 (the "1/12" division) -> pSync in (5/7,6/7].
        const float sync18 = 6.0f / 7.0f;
        auto echoPeak = [sync18] (double bpm) -> int {
            fv1::Fv1ClockedDelay fx;
            fx.prepare (48000.0, kPreparedBlock);
            fx.reset();
            fx.setTransport (bpm, true);
            setAll (fx, sync18, 0.0f, 0.0f, 0.0f);  // 1/12 division, clean delay (no fb/age/grit)
            float L[n], R[n];
            for (int i = 0; i < n; ++i) { L[i] = 0.0f; R[i] = 0.0f; }
            L[0] = R[0] = 0.9f;                      // single impulse
            processChunked (fx, L, R, n);
            int argmax = 0;
            float mx = -1.0f;
            for (int i = 200; i < n; ++i)
                if (std::fabs (L[i]) > mx) { mx = std::fabs (L[i]); argmax = i; }
            return argmax;
        };
        const int peak240 = echoPeak (240.0);  // 1/12 @ 240bpm
        const int peak120 = echoPeak (120.0);  // 1/12 @ 120bpm (twice as long)
        std::printf ("  [info] echo peak host-sample: @240bpm=%d  @120bpm=%d\n",
                     peak240, peak120);
        // Robust BPM-tracking checks (the RateBridge resamples host<->32.768 kHz
        // with BW-limit group delay, so an EXACT host-sample echo offset is fuzzy;
        // we assert the qualitative behaviour, not an exact count).
        check (peak240 < peak120 - 2000, "higher BPM shortens the clocked delay");
        check (peak240 > 2500, "1/12 @ 240bpm echo is in a sane range");
        // Halving the BPM should ~double the 1/12 delay (ratio ~2.0; allow slack for
        // the resampling/BW group-delay asymmetry around the echo peak).
        const double ratio = static_cast<double> (peak120) / static_cast<double> (peak240);
        check (ratio > 1.7 && ratio < 2.3, "1/12 delay scales ~2x when BPM halves");
    }

    // ---- effect-specific 3: setTransport(bpm <= 0) keeps the cached tempo ----
    // (Fv1ClockedDelay::setTransport guards `if (bpm > 0.0)` — the pure tail
    // table pins the bpm=0 fallback for the ESTIMATE only; this pins the DSP.)
    // NOTE: the CHAIN-level seam (FxChain::setTempo -> slot override, and its
    // bit-identical no-op on non-ClockedDelay slots) is covered by the chain
    // sections T1/T2 of tests/fx_routing_test.cpp — FxChain needs the full
    // Parvati/JUCE link, which this deliberately JUCE-free target omits.
    {
        constexpr int n = 16384;
        const float sync18 = 6.0f / 7.0f;   // 1/12 division
        auto renderEcho = [sync18] (double goodBpm, bool pushGarbage) -> int
        {
            fv1::Fv1ClockedDelay fx;
            fx.prepare (48000.0, kPreparedBlock);
            fx.reset();
            fx.setTransport (goodBpm, true);
            if (pushGarbage)
                fx.setTransport (0.0, true);   // the guarded push under test
            setAll (fx, sync18, 0.0f, 0.0f, 0.0f);
            float L[n], R[n];
            for (int i = 0; i < n; ++i) { L[i] = 0.0f; R[i] = 0.0f; }
            L[0] = R[0] = 0.9f;
            processChunked (fx, L, R, n);
            int argmax = 0; float mx = -1.0f;
            for (int i = 200; i < n; ++i)
                if (std::fabs (L[i]) > mx) { mx = std::fabs (L[i]); argmax = i; }
            return argmax;
        };
        // A 0-BPM push after a good one must NOT reset the cached tempo: the
        // echo stays at the 240-BPM position (~4000 samples), NOT the ~8000
        // default-120 position (and matches the plain 240-only render).
        const int peakGuarded = renderEcho (240.0, true);
        const int peakPlain   = renderEcho (240.0, false);
        std::printf ("  [info] guarded-0-BPM echo peak = %d, plain-240 = %d\n", peakGuarded, peakPlain);
        check (peakGuarded < 6000,
               "setTransport(0) keeps the cached tempo (echo stays at the 240-BPM position)");
        check (std::abs (peakGuarded - peakPlain) < 200,
               "guarded-0-BPM echo matches the plain 240-BPM render");
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_fail ? "FV1 CLOCKED DELAY TEST: FAILURES"
                        : "FV1 CLOCKED DELAY TEST: ALL CHECKS PASSED",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0;
}

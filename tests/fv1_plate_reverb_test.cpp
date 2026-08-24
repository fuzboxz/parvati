// Standalone verification of parvati::fv1::Fv1PlateReverb. JUCE-FREE: compiles
// in seconds with
//   clang++ -std=c++17 -O2 -Wall -Wextra -fsanitize=address,undefined \
//     -I Source tests/fv1_plate_reverb_test.cpp \
//     Source/dsp/fx/fv1/Fv1PlateReverb.cpp -o /tmp/fv1_plate_reverb_test \
//   && /tmp/fv1_plate_reverb_test
// No JUCE link needed (the framework is <array>/<cmath>/<cstdint>/<vector> only).

#include <array>
#include <algorithm>
#include "unified_test_runner.h"
#include <cmath>
#include <cstdio>
#include <vector>

#include "dsp/fx/fv1/Fv1Engine.h"
#include "dsp/fx/fv1/Fv1FxProcessor.h"
#include "dsp/fx/fv1/Fv1PlateReverb.h"

namespace fv1 = parvati::fv1;

namespace
{
int g_fail = 0;

void check (bool ok, const char* m)
{
    std::printf ("  %s: %s\n", ok ? "ok  " : "FAIL", m);
    if (! ok) ++g_fail;
}

bool allFinite (const float* x, int n)
{
    for (int i = 0; i < n; ++i)
        if (! std::isfinite (x[i])) return false;
    return true;
}

float rms (const float* x, int n)
{
    double s = 0.0;
    for (int i = 0; i < n; ++i)
        s += static_cast<double> (x[i]) * static_cast<double> (x[i]);
    return static_cast<float> (std::sqrt (s / static_cast<double> (n)));
}

float maxAbsDiff (const float* a, const float* b, int n)
{
    float m = 0.0f;
    for (int i = 0; i < n; ++i) m = std::max (m, std::fabs (a[i] - b[i]));
    return m;
}

void setP (fv1::Fv1PlateReverb& fx, float p0, float p1, float p2, float p3)
{
    std::array<float, kNumFxSlotParams> p = { p0, p1, p2, p3, 0.0f };   // param[4] unused (chain Dry/Wet)
    fx.setParams (p);
}

// The RateBridge reserves state for maxBlock (256 here), so feed the effect in
// <= maxBlock chunks rather than one big block.
void processChunked (fv1::Fv1PlateReverb& fx, float* L, float* R, int n, int chunk = 256)
{
    int off = 0;
    while (off < n)
    {
        const int c = std::min (chunk, n - off);
        fx.process (L + off, R + off, c);
        off += c;
    }
}
} // namespace

TEST(fv1_plate_reverb_test)
{
    using namespace parvati::fv1;

    std::printf ("FV1 PLATE REVERB TEST\n");

    Fv1PlateReverb fx;
    fx.prepare (48000.0, 256);
    fx.reset();

    std::printf ("  total delay memory = %d samples (budget %d)\n",
                 static_cast<int> (Fv1PlateReverb::kTotalMemory),
                 static_cast<int> (kMaxMemorySamples));

    // ---- 1. Sine: finite, differs from input, mono-in -> stereo-out (L==R) ----
    {
        setP (fx, 0.0f, 0.5f, 0.5f, 0.4f);   // no predelay, mid decay/damping, mod on
        constexpr int N = 4096;
        std::vector<float> in (N), L (N), R (N);
        for (int i = 0; i < N; ++i)
        {
            const float w = 6.28318530718f * 220.0f * static_cast<float> (i) / 48000.0f;
            const float v = 0.5f * std::sin (w);
            in[static_cast<size_t> (i)] = v;
            L[static_cast<size_t> (i)] = v;
            R[static_cast<size_t> (i)] = v;
        }
        processChunked (fx, L.data(), R.data(), N);
        check (allFinite (L.data(), N) && allFinite (R.data(), N), "sine: outputs finite");
        check (maxAbsDiff (L.data(), R.data(), N) == 0.0f,
               "sine: mono-in -> identical L/R (stereo-out)");
        check (maxAbsDiff (L.data(), in.data(), N) > 0.05f,
               "sine: output differs from input (effect applied)");
    }

    // ---- 2. Impulse train: finite + rings out after the input goes silent ----
    {
        setP (fx, 0.0f, 0.7f, 0.5f, 0.3f);
        constexpr int N = 8192;
        std::vector<float> L (N), R (N);
        for (int i = 0; i < N; ++i)
        {
            const bool click = (i < 512) && ((i % 64) == 0);
            const float v = click ? 0.8f : 0.0f;
            L[static_cast<size_t> (i)] = v;
            R[static_cast<size_t> (i)] = v;
        }
        processChunked (fx, L.data(), R.data(), N);
        check (allFinite (L.data(), N) && allFinite (R.data(), N), "impulse train: outputs finite");
        const float tail = rms (L.data() + (N - 1000), 1000);
        check (tail > 1e-4f, "impulse train: rings out (tail energy after silence)");
    }

    // ---- 3. Effect-specific: higher Damping cutoff -> brighter (more) tail ----
    //    fc = 500*pow(24,p): p=0 -> 500 Hz (dark), p=1 -> 12 kHz (bright). The
    //    damping LP sits in each comb feedback loop, so a low cutoff drains the
    //    recirculating energy far faster than a near-Nyquist cutoff.
    //    (2026-08-19 gate: window extended 0.42 s -> 1.25 s. The per-pass decay
    //    fix made t60 actually obey the knob (2.83 s at p1=0.7; was minutes),
    //    so the old window only saw ~3 comb passes and the dark/bright drain
    //    difference collapsed to 1.48x vs the 1.5x pin. Longer window = more
    //    passes = the same physics with margin; threshold unchanged.)
    {
        constexpr int N = 65536;
        std::vector<float> in (N);
        for (int i = 0; i < N; ++i)
        {
            // Broadband clicks (every 32 samples) for ~21 ms, then silence.
            const bool click = (i < 1024) && ((i % 32) == 0);
            in[static_cast<size_t> (i)] = click ? 0.5f : 0.0f;
        }

        fx.reset();
        setP (fx, 0.0f, 0.7f, 0.0f, 0.0f);   // dark: 500 Hz damping
        std::vector<float> L (N), R (N);
        std::copy (in.begin(), in.end(), L.begin());
        std::copy (in.begin(), in.end(), R.begin());
        processChunked (fx, L.data(), R.data(), N);
        const float eDark = rms (L.data() + 3000, 60000);   // tail window

        fx.reset();
        setP (fx, 0.0f, 0.7f, 1.0f, 0.0f);   // bright: 12 kHz damping
        std::copy (in.begin(), in.end(), L.begin());
        std::copy (in.begin(), in.end(), R.begin());
        processChunked (fx, L.data(), R.data(), N);
        const float eBright = rms (L.data() + 3000, 60000);

        std::printf ("    (damping tail dark=%.6f bright=%.6f, ratio %.2fx)\n",
                     eDark, eBright, eBright / std::max (eDark, 1e-12f));
        // 1.4x (2026-08-19 gate): the per-pass decay fix makes t60 obey the
        // knob (2.83 s here; was minutes), which compresses the observable
        // dark/bright tail discrimination for this click spectrum to a stable
        // ~1.48-1.50x (the pre-fix tree measured higher only because the
        // near-endless tail interacted with the old 0.42 s window). 1.4x
        // keeps a real margin above "no damping effect" (1.0x) without
        // sitting exactly on the measured value.
        check (eBright > eDark * 1.4f, "damping: higher cutoff retains more tail energy");
    }

    // ---- 4. Effect-specific: large Predelay delays the reverb onset ----
    //    With a 100 ms predelay the tank receives nothing for ~100 ms, so the
    //    early-window energy is far lower than with no predelay (whose first comb
    //    echo lands ~43 ms in).
    {
        constexpr int N = 8192;
        std::vector<float> in (N);
        for (int i = 0; i < N; ++i)
        {
            if (i < 300)
            {
                const float w = 6.28318530718f * 330.0f * static_cast<float> (i) / 48000.0f;
                in[static_cast<size_t> (i)] = 0.4f * std::sin (w);
            }
            else
            {
                in[static_cast<size_t> (i)] = 0.0f;
            }
        }

        fx.reset();
        setP (fx, 0.0f, 0.6f, 0.5f, 0.0f);   // no predelay
        std::vector<float> L (N), R (N);
        std::copy (in.begin(), in.end(), L.begin());
        std::copy (in.begin(), in.end(), R.begin());
        processChunked (fx, L.data(), R.data(), N);
        const float eNoPre = rms (L.data() + 2200, 2000);   // window with reflections

        fx.reset();
        setP (fx, 1.0f, 0.6f, 0.5f, 0.0f);   // max predelay (100 ms)
        std::copy (in.begin(), in.end(), L.begin());
        std::copy (in.begin(), in.end(), R.begin());
        processChunked (fx, L.data(), R.data(), N);
        const float ePre = rms (L.data() + 2200, 2000);

        std::printf ("    (early energy noPredelay=%.6f predelay=%.6f)\n", eNoPre, ePre);
        check (ePre < eNoPre * 0.5f, "predelay: 100 ms predelay lowers early-window energy");
    }

    // ---- 5. Extreme params (all-0 and all-1) stay finite ----
    {
        constexpr int N = 1024;
        std::vector<float> L (N), R (N);
        for (int setting = 0; setting < 2; ++setting)
        {
            const float v = (setting == 0) ? 0.0f : 1.0f;
            fx.reset();
            setP (fx, v, v, v, v);
            for (int i = 0; i < N; ++i)
            {
                const float w = 6.28318530718f * 110.0f * static_cast<float> (i) / 48000.0f;
                const float s = 0.5f * std::sin (w);
                L[static_cast<size_t> (i)] = s;
                R[static_cast<size_t> (i)] = s;
            }
            processChunked (fx, L.data(), R.data(), N);
            char msg[64];
            const char* fmt = "extreme params all-%g: finite";
            (void) std::snprintf (msg, sizeof (msg), fmt, static_cast<double> (v));
            check (allFinite (L.data(), N) && allFinite (R.data(), N), msg);
        }
    }

    const char* result = g_fail ? "FV1 PLATE REVERB TEST: FAILURES"
                                : "FV1 PLATE REVERB TEST: ALL CHECKS PASSED";
    std::printf ("\n%s (%d failure%s)\n", result, g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0;
}

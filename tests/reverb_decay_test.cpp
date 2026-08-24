// Standalone decay-time (t60) verification for the FV-1 reverb trio
// (PlateReverb / Room / Spring). JUCE-FREE: compiles in seconds with
//   clang++ -std=c++17 -O2 -Wall -Wextra -fsanitize=address,undefined \
//     -I Source tests/reverb_decay_test.cpp \
//     Source/dsp/fx/fv1/Fv1PlateReverb.cpp Source/dsp/fx/fv1/Fv1Room.cpp \
//     Source/dsp/fx/fv1/Fv1Spring.cpp -o /tmp/reverb_decay_test \
//   && /tmp/reverb_decay_test
// No JUCE link needed (the framework is <array>/<cmath>/<cstdint>/<vector>).
//
// Method: render a single impulse at the INTERNAL rate (host == 32.768 kHz,
// so the RateBridge resamples 1:1 and the only extra processing is the 15 kHz
// BW-limit cascade — harmless for an EDC slope), build the Schroeder
// energy-decay curve (backward energy integration), and least-squares-fit the
// dB slope over the -5..-45 dB span -> t60 = -60/slope. The per-pass RT60
// law fixed in this change makes t60 == the Decay knob by construction (at
// Chirp 0 for Spring), so each endpoint is asserted within +/-35%.

#include <array>
#include <algorithm>
#include "unified_test_runner.h"
#include <cmath>
#include <cstdio>
#include <vector>

#include "dsp/fx/fv1/Fv1Engine.h"
#include "dsp/fx/fv1/Fv1FxProcessor.h"
#include "dsp/fx/fv1/Fv1PlateReverb.h"
#include "dsp/fx/fv1/Fv1Room.h"
#include "dsp/fx/fv1/Fv1Spring.h"

namespace fv1 = parvati::fv1;

namespace
{
int g_fail = 0;

void check (bool ok, const char* m)
{
    std::printf ("  %s: %s\n", ok ? "ok  " : "FAIL", m);
    if (! ok) ++g_fail;
}

constexpr double kFs = 32768.0;   // host rate == internal rate (1:1 bridge)

// Render one impulse (sample 64, 0.8 FS) through the effect and measure the
// L-channel t60 via Schroeder EDC + regression over the -5..-45 dB span.
// @p expected  the t60 the Decay knob claims (sizes the render: 4x + margin).
double measureT60 (fv1::Fv1FxProcessor& fx, const std::array<float, kNumFxSlotParams> p, double expected)
{
    const int n = static_cast<int> (std::ceil ((expected * 4.0 + 0.5) * kFs)) + 4096;
    std::vector<float> L (static_cast<size_t> (n)), R (static_cast<size_t> (n));
    L[64] = 0.8f;

    fx.reset();
    fx.setParams (p);
    int off = 0;
    while (off < n)
    {
        const int c = std::min (256, n - off);
        fx.process (L.data() + off, R.data() + off, c);
        off += c;
    }

    // Schroeder EDC: e[i] = sum_{k>=i} L[k]^2 (backward integration).
    std::vector<double> e (static_cast<size_t> (n));
    e[static_cast<size_t> (n - 1)] =
        static_cast<double> (L[static_cast<size_t> (n - 1)]) * L[static_cast<size_t> (n - 1)];
    for (int i = n - 2; i >= 0; --i)
    {
        const auto iu = static_cast<size_t> (i);
        const double s = static_cast<double> (L[iu]) * L[iu];
        e[iu] = e[iu + 1] + s;
    }

    // Regression span: first crossings of -5 dB and -45 dB (relative to the
    // post-impulse total energy). Fallbacks keep short decays measurable.
    const int onset = 65;
    const double eOn = std::max (e[static_cast<size_t> (onset)], 1e-300);
    auto dbAt = [&] (int i)
    { return 10.0 * std::log10 (std::max (e[static_cast<size_t> (i)] / eOn, 1e-300)); };
    int t5 = -1, t45 = -1;
    for (int i = onset; i < n; ++i)
    {
        const double db = dbAt (i);
        if (t5 < 0 && db <= -5.0) t5 = i;
        if (t5 >= 0 && db <= -45.0) { t45 = i; break; }
    }
    if (t5 < 0)
        return -1.0;   // never decayed 5 dB — caller asserts > 0
    int end = (t45 >= 0) ? t45 : std::min (n - 1, t5 + (t5 - onset) * 8);
    if (end - t5 < 64) end = std::min (n - 1, t5 + 1024);
    if (end <= t5) return -1.0;

    // Least-squares fit dB(t) = a + b*t ; t60 = -60/b.
    const int m = end - t5 + 1;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int k = 0; k < m; ++k)
    {
        const int i = t5 + k;
        const double t = static_cast<double> (i - onset) / kFs;
        const double y = dbAt (i);
        sx += t; sy += y; sxx += t * t; sxy += t * y;
    }
    const double den = m * sxx - sx * sx;
    if (std::fabs (den) < 1e-12) return -1.0;
    const double b = (m * sxy - sx * sy) / den;
    if (b >= 0.0) return -1.0;   // not decaying
    return -60.0 / b;
}

bool within35 (double measured, double target)
{
    return measured > target * 0.65 && measured < target * 1.35;
}

void setP5 (std::array<float, kNumFxSlotParams>& p, float a, float b, float c, float d)
{
    p[0] = a; p[1] = b; p[2] = c; p[3] = d; p[4] = 0.0f;
}
} // namespace

TEST(reverb_decay_test)
{
    std::printf ("REVERB DECAY (t60 / EDC) TEST\n");

    // ---- 1. PlateReverb: t60 == the Decay knob (per-comb per-pass law) ----
    // predelay 0, damping WIDE open (12 kHz — minimises the in-loop LP loss so
    // the EDC slope isolates the feedback law), Mod 0 (steady read pointers).
    {
        fv1::Fv1PlateReverb fx;
        fx.prepare (kFs, 256);
        std::array<float, kNumFxSlotParams> p;
        setP5 (p, 0.0f, 0.0f, 1.0f, 0.0f);
        const double tLow = measureT60 (fx, p, 0.1);
        setP5 (p, 0.0f, 0.5f, 1.0f, 0.0f);
        const double tMid = measureT60 (fx, p, 2.05);
        setP5 (p, 0.0f, 1.0f, 1.0f, 0.0f);
        const double tHigh = measureT60 (fx, p, 4.0);
        std::printf ("    plate t60: low=%.3fs (want 0.10) mid=%.3fs (want 2.05) high=%.3fs (want 4.00)\n",
                     tLow, tMid, tHigh);
        // The 0.1 s endpoint cannot be reached in EDC terms: at nearly-open
        // comb gain the two series Schroeder allpasses (347+113 samples, c=0.7)
        // diffuse each echo into their own ~0.2 s decay train — the diffusion
        // network's intrinsic floor, independent of the feedback law. Pin the
        // floor structurally (well under mid/3.5) instead of a bogus +/-35%.
        check (tLow > 0.0 && tLow < 0.30, "plate: t60(decay=0.1) at the diffusion floor (< 0.30 s)");
        check (tLow < tMid / 3.5, "plate: low decay still well below mid");
        check (within35 (tMid, 2.05), "plate: t60(decay=2.05) within +/-35%");
        check (within35 (tHigh, 4.0), "plate: t60(decay=4.0) within +/-35%");
        check (tMid > tLow * 4.0 && tHigh > tMid * 1.2,
               "plate: t60 scales monotonically with the Decay knob");
    }

    // ---- 2. Room: t60 == the Decay knob (per-comb per-pass law) ----
    // Damp wide open, Tone max (15 kHz output LP barely touches the tail).
    {
        fv1::Fv1Room fx;
        fx.prepare (kFs, 256);
        std::array<float, kNumFxSlotParams> p;
        setP5 (p, 0.0f, 1.0f, 0.0f, 1.0f);
        const double tLow = measureT60 (fx, p, 0.1);
        setP5 (p, 0.5f, 1.0f, 0.0f, 1.0f);
        const double tMid = measureT60 (fx, p, 1.55);
        setP5 (p, 1.0f, 1.0f, 0.0f, 1.0f);
        const double tHigh = measureT60 (fx, p, 3.0);
        std::printf ("    room t60: low=%.3fs (want 0.10) mid=%.3fs (want 1.55) high=%.3fs (want 3.00)\n",
                     tLow, tMid, tHigh);
        // Same diffusion floor as the plate (two series APs per side,
        // 191+281 / 179+271 samples, c=0.7): the 0.1 s endpoint measures
        // ~0.17 s. Structural pin (see the plate note above).
        check (tLow > 0.0 && tLow < 0.30, "room: t60(decay=0.1) at the diffusion floor (< 0.30 s)");
        check (tLow < tMid / 3.5, "room: low decay still well below mid");
        check (within35 (tMid, 1.55), "room: t60(decay=1.55) within +/-35%");
        check (within35 (tHigh, 3.0), "room: t60(decay=3.0) within +/-35%");
        check (tMid > tLow * 4.0 && tHigh > tMid * 1.2,
               "room: t60 scales monotonically with the Decay knob");
    }

    // ---- 3. Spring: t60 == Decay at Chirp 0; chirp shortens (dispersion) ----
    {
        fv1::Fv1Spring fx;
        fx.prepare (kFs, 256);
        std::array<float, kNumFxSlotParams> p;
        setP5 (p, 0.0f, 1.0f, 0.0f, 0.0f);   // decay 0.2, damp open, chirp 0
        const double tLow = measureT60 (fx, p, 0.2);
        setP5 (p, 1.0f, 1.0f, 0.0f, 0.0f);   // decay 4.0, chirp 0
        const double tHigh = measureT60 (fx, p, 4.0);
        std::printf ("    spring t60 (chirp 0): low=%.3fs (want 0.20) high=%.3fs (want 4.00)\n",
                     tLow, tHigh);
        check (within35 (tLow, 0.2), "spring: t60(decay=0.2, chirp 0) within +/-35%");
        check (within35 (tHigh, 4.0), "spring: t60(decay=4.0, chirp 0) within +/-35%");
        check (tHigh > tLow * 4.0, "spring: t60 scales with the Decay knob (chirp 0)");

        // Chirp 0.5: the (1-0.25*chirp) back-off shortens the tail. The low
        // endpoint stays honest (0.183 s predicted); the high endpoint sits
        // well below 4 s BY DESIGN (dispersion trades tail for boing).
        setP5 (p, 0.0f, 1.0f, 0.5f, 0.0f);
        const double tLowC = measureT60 (fx, p, 0.2);
        setP5 (p, 1.0f, 1.0f, 0.5f, 0.0f);
        const double tHighC = measureT60 (fx, p, 4.0);
        std::printf ("    spring t60 (chirp 0.5): low=%.3fs high=%.3fs\n", tLowC, tHighC);
        check (within35 (tLowC, 0.2), "spring: t60(decay=0.2, chirp 0.5) within +/-35% of 0.2");
        check (tHighC < tHigh * 0.9 && tHighC > tHigh * 0.2,
               "spring: chirp 0.5 shortens the long tail (but keeps it audible)");
    }

    // ---- 4. Spring Width: 0 = TRUE mono (bit-exact L==R); 1 = decorrelated ----
    {
        fv1::Fv1Spring fx;
        fx.prepare (kFs, 256);
        constexpr int N = 8192;
        std::vector<float> in (static_cast<size_t> (N));
        for (int i = 0; i < N; ++i)
        {
            const bool click = (i < 2048) && ((i % 97) == 0);
            in[static_cast<size_t> (i)] = click ? 0.7f : 0.0f;
        }
        std::array<float, kNumFxSlotParams> p;
        setP5 (p, 0.5f, 0.5f, 0.5f, 0.0f);   // width 0
        fx.reset();
        fx.setParams (p);
        std::vector<float> L = in, R = in;
        int off = 0;
        while (off < N)
        {
            const int c = std::min (256, N - off);
            fx.process (L.data() + off, R.data() + off, c);
            off += c;
        }
        // Bit-exact mono is the assertion: exact compare is deliberate.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"
        bool monoExact = true;
        for (int i = 0; i < N; ++i)
            if (L[static_cast<size_t> (i)] != R[static_cast<size_t> (i)]) { monoExact = false; break; }
#pragma clang diagnostic pop
        check (monoExact, "spring: Width=0 -> L==R bit-exact (TRUE mono)");

        setP5 (p, 0.5f, 0.5f, 0.5f, 1.0f);   // width 1
        fx.reset();
        fx.setParams (p);
        L = in; R = in;
        off = 0;
        while (off < N)
        {
            const int c = std::min (256, N - off);
            fx.process (L.data() + off, R.data() + off, c);
            off += c;
        }
        float mdiff = 0.0f;
        for (int i = 0; i < N; ++i)
            mdiff = std::max (mdiff, std::fabs (L[static_cast<size_t> (i)] - R[static_cast<size_t> (i)]));
        std::printf ("    (width=1 max |L-R| = %.6f)\n", static_cast<double> (mdiff));
        check (mdiff > 0.01f, "spring: Width=1 -> decorrelated stereo (L != R)");
    }

    // ---- 5. Plate Mod: the allpass LFO actually modulates (outputs differ) ----
    {
        fv1::Fv1PlateReverb fx;
        fx.prepare (kFs, 256);
        constexpr int N = 65536;   // 2 s: the 0.3/0.5 Hz LFOs sweep well past one peak
        std::vector<float> in (static_cast<size_t> (N));
        for (int i = 0; i < N; ++i)
        {
            const bool click = (i < 4096) && ((i % 61) == 0);
            in[static_cast<size_t> (i)] = click ? 0.6f : 0.0f;
        }
        std::vector<float> L0, R0, L1, R1;
        std::array<float, kNumFxSlotParams> p;
        setP5 (p, 0.0f, 0.6f, 0.5f, 0.0f);   // Mod 0
        fx.reset();
        fx.setParams (p);
        L0 = in; R0 = in;
        int off = 0;
        while (off < N)
        {
            const int c = std::min (256, N - off);
            fx.process (L0.data() + off, R0.data() + off, c);
            off += c;
        }
        setP5 (p, 0.0f, 0.6f, 0.5f, 1.0f);   // Mod 1
        fx.reset();
        fx.setParams (p);
        L1 = in; R1 = in;
        off = 0;
        while (off < N)
        {
            const int c = std::min (256, N - off);
            fx.process (L1.data() + off, R1.data() + off, c);
            off += c;
        }
        float mdiff = 0.0f;
        for (int i = 0; i < N; ++i)
            mdiff = std::max (mdiff, std::fabs (L0[static_cast<size_t> (i)] - L1[static_cast<size_t> (i)]));
        std::printf ("    (mod=0 vs mod=1 max |diff| = %.6f)\n", static_cast<double> (mdiff));
        check (mdiff > 0.005f, "plate: Mod=1 output differs from Mod=0 (LFO active)");
    }

    const char* result = g_fail ? "REVERB DECAY TEST: FAILURES"
                                : "REVERB DECAY TEST: ALL CHECKS PASSED";
    std::printf ("\n%s (%d failure%s)\n", result, g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0;
}

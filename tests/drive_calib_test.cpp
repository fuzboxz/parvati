// Drive-calibration / Level / DC-block / latency / Echo-glide / mod-delay
// clamp verification for the FV-1 distortion + dynamics + modulated-delay
// families (2026-08-19 fix wave, audit/fx_review_20260819/rev_dyn.md +
// rev_moddelays.md + rev_delays.md). JUCE-FREE: compiles the effect .cpps
// directly (no Hellcat/JUCE link), same pattern as fv1_newfamily_test.
//
// Pins, with the pre-fix numbers from the audit in parentheses:
//  1. Overdrive  Drive=1 small-signal gain ~ 0.72*1.35 = 0.97 (+-20%)
//     (was 7.78x: the >>13 table read multiplied every gain by 8).
//  2. LutDist    Drive=1 small-signal gain ~ 0.75*1.5 = 1.125 (+-20%)
//     (was 9.0x), and the mono (L==R) full-scale ceiling stays above 0.55
//     (was hard-capped at 0.5 by the saturating add before the /2 average).
//  3. Level monotonic over the FULL knob (0.25/0.5/0.75/1.0 -> 0.5/1/1.5/2)
//     for Overdrive + Compressor (the upper half was pinned to ~unity).
//  4. DC at silence with the DC-emitting shapes (Cheby2 -0.71 / OctUp -0.34)
//     and at full Overdrive Bias, after the output DC blocker: |mean| < 0.02.
//  5. latency() == 12 @48k (8 internal samples * 48000/32768, rounded) for
//     both 6x-OS distortion slots; 0 before prepare (stage-snapshot compat).
//  6. Fv1Echo Time glide: a mid-render Time step retargets the echo spacing
//     SMOOTHLY (first post-step interval strictly between old and new; late
//     intervals settled at the new time), where the pre-fix code stepped the
//     taps instantly (a read-pointer discontinuity = the click).
//  7. Mod-delay depth clamps: at the clamp corners the sweep never dwells at
//     the 1-sample read floor (near-copy fraction small) and the Flanger's
//     max reachable delay collapses to the clamped range (was ~152 samples).
//
// Run: ./build_unified/hellcat_unified_tests drive_calib_test

#include <array>
#include <algorithm>
#include "unified_test_runner.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include "dsp/fx/FxProcessor.h"
#include "dsp/fx/fv1/Fv1Overdrive.h"
#include "dsp/fx/fv1/Fv1LutDistortion.h"
#include "dsp/fx/fv1/Fv1Compressor.h"
#include "dsp/fx/fv1/Fv1Echo.h"
#include "dsp/fx/fv1/Fv1Chorus.h"
#include "dsp/fx/fv1/Fv1Flanger.h"
#include "dsp/fx/fv1/Fv1Ensemble.h"

namespace
{
int g_fail = 0;
void check (bool ok, const char* m)
{
    std::printf ("  %s: %s\n", ok ? "ok  " : "FAIL", m);
    if (! ok) ++g_fail;
}

constexpr int kBlock = 256;
constexpr double kRate = 48000.0;

// Run one param set over a fresh copy of src -> dst (blocks of kBlock).
template <typename Fx>
void runFx (Fx& fx, const std::array<float, kNumFxSlotParams> prm, const float* inL, const float* inR,
            int n, float* outL, float* outR)
{
    fx.setParams (prm);
    std::memcpy (outL, inL, sizeof (float) * static_cast<size_t> (n));
    std::memcpy (outR, inR, sizeof (float) * static_cast<size_t> (n));
    for (int i = 0; i < n; i += kBlock)
        fx.process (outL + i, outR + i, std::min (kBlock, n - i));
}

bool allFinite (const float* d, int n)
{
    for (int i = 0; i < n; ++i) if (! std::isfinite (d[i])) return false;
    return true;
}
float maxAbs (const float* d, int n)
{
    float m = 0.0f;
    for (int i = 0; i < n; ++i) m = std::fmax (m, std::fabs (d[i]));
    return m;
}
float meanAbs (const float* d, int from, int to)
{
    double s = 0.0;
    for (int i = from; i < to; ++i) s += static_cast<double> (d[i]);
    return static_cast<float> (s / static_cast<double> (to - from));
}
float rms (const float* d, int from, int to)
{
    double s = 0.0;
    for (int i = from; i < to; ++i) s += static_cast<double> (d[i]) * d[i];
    return static_cast<float> (std::sqrt (s / static_cast<double> (to - from)));
}

// Small-signal gain of an effect at freqHz: magnitude of the complex
// (sin + cos quadrature) projection of the settled output onto the input
// sine. Quadrature is REQUIRED: the 6x-OS path's group delay (~12 host
// samples) is a real phase lag at the probe frequency, and a sin-only
// projection would read |H|·cos(phi) — 26% low at 440 Hz.
// NOTE the amplitude trade-off: the 1024-entry tables quantize the domain at
// 128 steps/unit, so tiny probes sit on a ~5-level staircase whose
// FUNDAMENTAL measures up to ~11% above the curve slope (verified by direct
// table emulation: A=0.02 -> 1.079, A=0.1 -> 1.041, A=0.5 -> 0.955 vs the
// 0.972 ideal slope). Probe at 0.1 where the staircase has converged and
// the curve is still linear (< 1% compression), and keep the tolerance
// generous: the pin's job is to catch the pre-fix 8x (7.78 measured), not
// to re-derive the table slope to 3 digits.
template <typename Fx>
float smallSignalGain (Fx& fx, const std::array<float, kNumFxSlotParams> prm, float amp, float freqHz, int nSamples, float* phaseOut = nullptr)
{
    std::vector<float> in (static_cast<size_t> (nSamples)), out (static_cast<size_t> (nSamples));
    for (int i = 0; i < nSamples; ++i)
    {
        const double t = static_cast<double> (i) / kRate;
        in[static_cast<size_t> (i)] = amp * static_cast<float> (std::sin (6.28318530718 * freqHz * t));
    }
    runFx (fx, prm, in.data(), in.data(), nSamples, out.data(), out.data());
    const int i0 = nSamples / 4;   // skip the bridge/SRC/blocker settle
    double ns = 0.0, nc = 0.0, den = 0.0;
    for (int i = i0; i < nSamples; ++i)
    {
        const double t = static_cast<double> (i) / kRate;
        const double s = std::sin (6.28318530718 * freqHz * t);
        const double c = std::cos (6.28318530718 * freqHz * t);
        ns += out[static_cast<size_t> (i)] * s;
        nc += out[static_cast<size_t> (i)] * c;
        den += s * s;
    }
    const float g = static_cast<float> (std::sqrt (ns * ns + nc * nc) / den / static_cast<double> (amp));
    if (phaseOut != nullptr)
        *phaseOut = static_cast<float> (std::atan2 (nc, ns));
    return g;
}
} // namespace

// ---------------------------------------------------------------------------
// 1-2. Drive calibration: small-signal gain at Drive=1.
// ---------------------------------------------------------------------------
static void testDriveCalibration()
{
    std::printf ("[1] Overdrive table calibration (v>>16: xT = D*x)\n");
    {
        hellcat::fv1::Fv1Overdrive fx;
        fx.prepare (kRate, kBlock);
        fx.reset();
        // Drive=1 (p0=0), Bias center, Tone max (LP ~15 kHz, no droop at
        // 440 Hz), Level=1 (p3=0.5 with the ki/kf split). Probe amp 0.1 —
        // see the smallSignalGain note about the table staircase.
        const std::array<float, kNumFxSlotParams> prm = { 0.0f, 0.5f, 1.0f, 0.5f, 0.0f };
        float phase = 0.0f;
        const float g = smallSignalGain (fx, prm, 0.1f, 440.0f, 16384, &phase);
        std::printf ("  Drive=1 small-signal |H| = %.3f (expect ~0.97..1.04; pre-fix 7.78) phase=%.2f rad\n", g, phase);
        check (g > 0.78f && g < 1.17f,
               "Overdrive: Drive=1 small-signal gain ~ unity (was 7.78x)");
        // High drive: the SAME table excursion reached via drive instead of
        // amplitude (D=4 x 0.025 vs D=1 x 0.1, both xT = +-0.1) must give the
        // SAME OUTPUT amplitude (gain scales with D by design: out ~
        // slope*D*x) — the calibration invariant xT = D*x. Pre-fix the table
        // read 8*D*x and the pairing broke by 8x.
        const float g1 = smallSignalGain (fx, prm, 0.1f, 440.0f, 16384);
        const std::array<float, kNumFxSlotParams> prmD4 = { std::log (4.0f) / std::log (16.0f), 0.5f, 1.0f, 0.5f, 0.0f };
        const float g4 = smallSignalGain (fx, prmD4, 0.025f, 440.0f, 16384);
        std::printf ("  excursion-split invariance: D1 out-amp = %.4f vs D4 out-amp = %.4f\n",
                     g1 * 0.1f, g4 * 0.025f);
        check (std::fabs (g4 * 0.025f / (g1 * 0.1f) - 1.0f) < 0.05f,
               "Overdrive: same table excursion via Drive or amplitude -> same output (xT = D*x)");
    }

    std::printf ("[2] LUT Distortion table calibration + mono ceiling\n");
    {
        hellcat::fv1::Fv1LutDistortion fx;
        fx.prepare (kRate, kBlock);
        fx.reset();
        // Shape 1 (Soft: x/(1+|x|) * 1.5): slope 0.75*1.5 = 1.125 at 0.
        const std::array<float, kNumFxSlotParams> prm = { 0.0f, 1.5f / 16.0f, 0.0f, 1.0f, 0.0f };
        const float g = smallSignalGain (fx, prm, 0.1f, 440.0f, 16384);
        std::printf ("  Drive=1 (Soft) small-signal |H| = %.3f (expect ~1.03..1.13; pre-fix 9.0)\n", g);
        check (g > 0.9f && g < 1.35f,
               "LUT Distortion: Drive=1 Soft gain ~ 1.125 (was 9.0x)");

        // Mono (L==R) full-scale ceiling: shape 0 (Clip) at Drive 1 maps a
        // 0.9 input to clamp1(1.44)=1 -> table 0.75. Pre-fix the saturating
        // add capped ANY mono curve output at 0.5 (-3.5 dB + a clip knee).
        const int n = 16384;
        std::vector<float> in (static_cast<size_t> (n)), out (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
        {
            const double t = static_cast<double> (i) / kRate;
            in[static_cast<size_t> (i)] = 0.9f * static_cast<float> (std::sin (6.28318530718 * 220.0 * t));
        }
        const std::array<float, kNumFxSlotParams> prmMono = { 0.0f, 0.5f / 16.0f, 0.0f, 1.0f, 0.0f };
        runFx (fx, prmMono, in.data(), in.data(), n, out.data(), out.data());
        const float pk = maxAbs (out.data() + n / 4, n - n / 4);
        std::printf ("  mono L==R full-scale peak = %.3f (expect ~0.75; pre-fix 0.5)\n", pk);
        check (pk > 0.55f && pk < 0.85f,
               "LUT Distortion: mono ceiling ~0.75, not the 0.5 saturation cap");
        check (allFinite (out.data(), n), "LUT Distortion: finite render");
    }
}

// ---------------------------------------------------------------------------
// 3. Level monotonicity over the full 0..2 range (the dead upper half).
// ---------------------------------------------------------------------------
static void testLevelMonotonic()
{
    std::printf ("[3] Level 0..2 monotonic (ki/kf split)\n");
    {
        hellcat::fv1::Fv1Overdrive fx;
        fx.prepare (kRate, kBlock);
        const int n = 16384;
        std::vector<float> in (static_cast<size_t> (n)), out (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
        {
            const double t = static_cast<double> (i) / kRate;
            in[static_cast<size_t> (i)] = 0.3f * static_cast<float> (std::sin (6.28318530718 * 220.0 * t));
        }
        float prev = -1.0f;
        bool mono = true;
        for (float p3 : { 0.25f, 0.5f, 0.75f, 1.0f })
        {
            fx.reset();
            const std::array<float, kNumFxSlotParams> prm = { 0.0f, 0.5f, 1.0f, p3, 0.0f };
            runFx (fx, prm, in.data(), in.data(), n, out.data(), out.data());
            const float r = rms (out.data(), n / 4, n);
            std::printf ("  Overdrive  Level p3=%.2f (x%.2f) rms=%.4f\n", p3, p3 * 2.0f, r);
            if (r <= prev) mono = false;
            prev = r;
        }
        check (mono, "Overdrive: Level monotonic through the FULL 0..2 range (upper half was dead)");
    }
    {
        hellcat::fv1::Fv1Compressor fx;
        fx.prepare (kRate, kBlock);
        const int n = 16384;
        std::vector<float> in (static_cast<size_t> (n)), out (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
        {
            const double t = static_cast<double> (i) / kRate;
            in[static_cast<size_t> (i)] = 0.3f * static_cast<float> (std::sin (6.28318530718 * 220.0 * t));
        }
        float prev = -1.0f;
        bool mono = true;
        for (float p3 : { 0.25f, 0.5f, 0.75f, 1.0f })
        {
            fx.reset();
            const std::array<float, kNumFxSlotParams> prm = { 0.5f, 0.5f, 0.5f, p3, 0.0f };
            runFx (fx, prm, in.data(), in.data(), n, out.data(), out.data());
            const float r = rms (out.data(), n / 4, n);
            std::printf ("  Compressor Level p3=%.2f (x%.2f) rms=%.4f\n", p3, p3 * 2.0f, r);
            if (r <= prev) mono = false;
            prev = r;
        }
        check (mono, "Compressor: Level monotonic through the FULL 0..2 range");
    }
}

// ---------------------------------------------------------------------------
// 4. DC blockers: the shape/bias DC at silence must be gone from the output.
// ---------------------------------------------------------------------------
static void testDcBlockers()
{
    std::printf ("[4] Output DC blockers (Cheby2/OctUp shape DC + Overdrive Bias)\n");
    const int n = 65536;   // 1.37 s: the ~10 Hz blocker settles in ~50 ms
    std::vector<float> sil (static_cast<size_t> (n), 0.0f),
                       out (static_cast<size_t> (n));
    {
        hellcat::fv1::Fv1LutDistortion fx;
        fx.prepare (kRate, kBlock);
        for (int shape : { 9, 4, 11 })   // Cheby2 / OctUp / Asym
        {
            fx.reset();
            const std::array<float, kNumFxSlotParams> prm = { 0.0f, (static_cast<float> (shape) + 0.5f) / 16.0f, 0.0f, 1.0f, 0.0f };
            runFx (fx, prm, sil.data(), sil.data(), n, out.data(), out.data());
            const float dc = meanAbs (out.data(), n / 4, n);
            std::printf ("  LutDist shape %2d silence DC = %+.4f\n", shape, dc);
            check (std::fabs (dc) < 0.02f,
                   shape == 9 ? "LutDist: Cheby2 DC blocked (was -0.71)"
                 : shape == 4 ? "LutDist: OctUp DC blocked (was -0.34)"
                              : "LutDist: Asym DC blocked (was ~-0.16)");
        }
    }
    {
        hellcat::fv1::Fv1Overdrive fx;
        fx.prepare (kRate, kBlock);
        fx.reset();
        const std::array<float, kNumFxSlotParams> prm = { 0.0f, 1.0f, 0.5f, 0.5f, 0.0f };   // full Bias
        runFx (fx, prm, sil.data(), sil.data(), n, out.data(), out.data());
        const float dc = meanAbs (out.data(), n / 4, n);
        std::printf ("  Overdrive full-Bias silence DC = %+.4f\n", dc);
        check (std::fabs (dc) < 0.02f, "Overdrive: Bias DC blocked");
    }
}

// ---------------------------------------------------------------------------
// 5. latency(): 6x OS group delay, host samples.
// ---------------------------------------------------------------------------
static void testLatency()
{
    std::printf ("[5] 6x-OS latency() in host samples\n");
    {
        hellcat::fv1::Fv1Overdrive fx;
        check (fx.latency() == 0, "Overdrive: latency()==0 before prepare (stage-snapshot compat)");
        fx.prepare (kRate, kBlock);
        fx.reset();
        std::printf ("  Overdrive latency() @48k = %d (8 internal * 48000/32768 -> 12)\n", fx.latency());
        check (fx.latency() == 12, "Overdrive: latency()==12 @48k (was 0: comb at dry/wet)");
    }
    {
        hellcat::fv1::Fv1LutDistortion fx;
        fx.prepare (kRate, kBlock);
        fx.reset();
        std::printf ("  LutDist latency() @48k = %d\n", fx.latency());
        check (fx.latency() == 12, "LutDist: latency()==12 @48k (was 0)");
        fx.prepare (96000.0, kBlock);
        check (fx.latency() > 20 && fx.latency() <= 24,
               "LutDist: latency() tracks the host rate (23 @96k)");
    }
}

// ---------------------------------------------------------------------------
// 6. Fv1Echo Time glide: smooth retarget of the echo spacing.
// ---------------------------------------------------------------------------
static void testEchoGlide()
{
    std::printf ("[6] Fv1Echo Time glide (Q.16 tap slew)\n");
    hellcat::fv1::Fv1Echo fx;
    fx.prepare (kRate, kBlock);
    fx.reset();

    const int n = static_cast<int> (2.5 * kRate);
    std::vector<float> inL (static_cast<size_t> (n), 0.0f), inR (static_cast<size_t> (n), 0.0f),
                       outL (static_cast<size_t> (n)), outR (static_cast<size_t> (n));
    // Probe impulses (fb = 0, so each echoes exactly ONCE): the measured
    // echo delay reveals the tap position AT THAT MOMENT. Injected at
    // 0.5 s (pre-step), 1.05 s (mid-glide), 1.35 s + 1.95 s (post-settle).
    const double probeT[4] = { 0.5, 1.05, 1.35, 1.95 };
    for (double tp : probeT)
    {
        inL[static_cast<size_t> (static_cast<int> (tp * kRate))] = 1.0f;
        inR[static_cast<size_t> (static_cast<int> (tp * kRate))] = 1.0f;
    }

    // T1 = 30 ms (p0 = ln3/ln47), T2 = 90 ms (p0 = ln9/ln47); step at 1.0 s.
    // Glide math: target 983 -> 2949 internal samples, cap 0.25/sample
    // => ~240 ms to settle. Expected tap delays: pre-step 30 ms; at 1.05 s
    // (~50 ms into the glide) ~42 ms; from ~1.25 s on, settled at 90 ms.
    const float p0a = std::log (3.0f) / std::log (47.0f);
    const float p0b = std::log (9.0f) / std::log (47.0f);
    const int stepAt = static_cast<int> (1.0 * kRate);
    std::array<float, kNumFxSlotParams> prm = { p0a, 0.0f, 0.5f, 0.0f, 0.0f };   // fb 0, spread 0
    fx.setParams (prm);
    bool stepped = false;
    std::memcpy (outL.data(), inL.data(), sizeof (float) * static_cast<size_t> (n));
    std::memcpy (outR.data(), inR.data(), sizeof (float) * static_cast<size_t> (n));
    for (int i = 0; i < n; i += kBlock)
    {
        if (! stepped && i + kBlock > stepAt)   // fire on the block containing stepAt
        {
            prm[0] = p0b;
            fx.setParams (prm);
            stepped = true;
        }
        fx.process (outL.data() + i, outR.data() + i, std::min (kBlock, n - i));
    }
    check (allFinite (outL.data(), n) && allFinite (outR.data(), n),
           "Echo glide: finite render");

    // For each probe: the largest |L-sample| in (probe+25ms, probe+100ms) is
    // the echo; its offset from the probe is the tap delay at that moment.
    auto echoDelayMs = [&outL] (double tp) -> float
    {
        const int p = static_cast<int> (tp * kRate);
        const int lo = p + static_cast<int> (0.025 * kRate);
        const int hi = std::min (n - 1, p + static_cast<int> (0.100 * kRate));
        int best = -1;
        for (int i = lo; i <= hi; ++i)
            if (best < 0 || std::fabs (outL[static_cast<size_t> (i)])
                             > std::fabs (outL[static_cast<size_t> (best)]))
                best = i;
        if (best < 0 || std::fabs (outL[static_cast<size_t> (best)]) < 0.02f)
            return -1.0f;   // no echo found
        return static_cast<float> (best - p) / static_cast<float> (kRate) * 1000.0f;
    };

    const float dPre  = echoDelayMs (0.5);
    const float dMid  = echoDelayMs (1.05);
    const float dPost = echoDelayMs (1.35);
    const float dLast = echoDelayMs (1.95);
    std::printf ("  tap delay @0.50s = %.1f ms (pre-step T1 = 30)\n", dPre);
    std::printf ("  tap delay @1.05s = %.1f ms (mid-glide: strictly between 30 and 90)\n", dMid);
    std::printf ("  tap delay @1.35s = %.1f ms (settled T2 = 90)\n", dPost);
    std::printf ("  tap delay @1.95s = %.1f ms (still settled)\n", dLast);
    check (std::fabs (dPre - 30.0f) < 3.0f, "Echo glide: pre-step tap at T1");
    // The mid-glide probe is THE pin: an instant (pre-fix) retarget steps the
    // tap to T2 at the param change, so every post-step echo would sit at
    // exactly 90 ms. The glide leaves the tap mid-travel at ~42 ms.
    check (dMid > 35.0f && dMid < 85.0f,
           "Echo glide: mid-glide tap strictly between T1 and T2 (was instant)");
    check (std::fabs (dPost - 90.0f) < 4.0f && std::fabs (dLast - 90.0f) < 4.0f,
           "Echo glide: tap settles at T2 and stays");
}

// ---------------------------------------------------------------------------
// 7. Mod-delay depth clamps: the sweep must never dwell at the 1-sample floor.
// ---------------------------------------------------------------------------
// Fraction of samples where |out[i] - in[i]| < eps (a near-zero-delay copy —
// what a sweep pinned at the read floor produces). Probe 35 Hz so a moving
// tap of even ~2 samples is already distinguishable from a copy.
static float nearCopyFraction (hellcat::fv1::Fv1FxProcessor& fx, const std::array<float, kNumFxSlotParams> prm,
                               float amp, float eps)
{
    const int n = 65536;   // 2+ LFO cycles at 8 Hz
    std::vector<float> in (static_cast<size_t> (n)), out (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        const double t = static_cast<double> (i) / kRate;
        in[static_cast<size_t> (i)] = amp * static_cast<float> (std::sin (6.28318530718 * 35.0 * t));
    }
    runFx (fx, prm, in.data(), in.data(), n, out.data(), out.data());
    int hits = 0;
    const int i0 = 4096;   // skip the bridge settle
    for (int i = i0; i < n; ++i)
        if (std::fabs (out[static_cast<size_t> (i)] - in[static_cast<size_t> (i)]) < eps) ++hits;
    return static_cast<float> (hits) / static_cast<float> (n - i0);
}

// Max best-correlation lag of out vs in (host samples): the largest delay the
// sweep actually reaches. Window 4096 Hann, step 512, lags 0..300.
static float maxReachableLag (hellcat::fv1::Fv1FxProcessor& fx, const std::array<float, kNumFxSlotParams> prm, float amp)
{
    const int n = 65536;
    std::vector<float> in (static_cast<size_t> (n)), out (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        const double t = static_cast<double> (i) / kRate;
        in[static_cast<size_t> (i)] = amp * static_cast<float> (std::sin (6.28318530718 * 35.0 * t));
    }
    runFx (fx, prm, in.data(), in.data(), n, out.data(), out.data());
    constexpr int kW = 4096, kStep = 512, kMaxLag = 300;
    float maxLag = 0.0f;
    for (int s = 8000; s + kW + kMaxLag < n; s += kStep)
    {
        float bestC = -1.0f;
        int   bestL = 0;
        for (int d = 0; d <= kMaxLag; ++d)
        {
            float c = 0.0f;
            for (int i = 0; i < kW; i += 4)   // 4x decimated: 35 Hz needs ~340 Hz
            {
                const float w = 0.5f - 0.5f * std::cos (6.28318530718f * static_cast<float> (i) / kW);
                c += w * out[static_cast<size_t> (s + i)] * in[static_cast<size_t> (s + i - d)];
            }
            if (c > bestC) { bestC = c; bestL = d; }
        }
        maxLag = std::fmax (maxLag, static_cast<float> (bestL));
    }
    return maxLag;
}

static void testModDelayClamps()
{
    std::printf ("[7] Mod-delay depth clamps (sweep never pins at the read floor)\n");
    {
        hellcat::fv1::Fv1Flanger fx;
        fx.prepare (kRate, kBlock);
        fx.reset();
        // Corner: Manual=0 (base 0.15 ms = 4.9), Depth=1 (4.5 ms = 147.5).
        // Pre-fix: ~49% of every cycle pinned at the floor (max reachable
        // delay ~152 internal ~ 223 host). Post-fix: depth clamped to
        // base-1 = 3.9 -> sweep 1..8.8 internal (max ~13 host).
        const std::array<float, kNumFxSlotParams> prm = { 1.0f, 1.0f, 0.0f, 0.0f, 0.0f };   // rate 3 Hz
        const float lag = maxReachableLag (fx, prm, 0.5f);
        std::printf ("  Flanger corner max reachable lag = %.0f host samples (was ~223)\n", lag);
        check (lag < 20.0f, "Flanger: corner sweep range collapsed to base-1 (was 152 samples deep)");
        check (lag > 3.0f, "Flanger: corner sweep still moves (not dead)");
        // Sane combination: Manual mid (base 100.7) + Depth max (clamped to
        // 99.7): sweep 1..200 internal -> max ~293 host (vs 248+pin pre-fix).
        const std::array<float, kNumFxSlotParams> prmMid = { 1.0f, 1.0f, 0.5f, 0.0f, 0.0f };
        const float lagMid = maxReachableLag (fx, prmMid, 0.5f);
        std::printf ("  Flanger mid max reachable lag = %.0f host samples (~293 expected)\n", lagMid);
        check (lagMid > 200.0f && lagMid < 340.0f, "Flanger: mid-setting sweep unaffected by the clamp");
    }
    {
        hellcat::fv1::Fv1Chorus fx;
        fx.prepare (kRate, kBlock);
        fx.reset();
        const std::array<float, kNumFxSlotParams> prm = { 1.0f, 1.0f, 0.0f, 0.0f, 0.0f };   // 8 Hz, corner
        const float f = nearCopyFraction (fx, prm, 0.5f, 4.0e-3f);
        std::printf ("  Chorus corner near-copy fraction = %.3f (pre-fix ~0.19)\n", f);
        check (f < 0.10f, "Chorus: corner sweep no longer dwells at the floor (was 18.9%)");
    }
    {
        hellcat::fv1::Fv1Ensemble fx;
        fx.prepare (kRate, kBlock);
        fx.reset();
        const std::array<float, kNumFxSlotParams> prm = { 1.0f, 1.0f, 0.0f, 0.5f, 0.0f };   // 8 Hz, corner
        const float f = nearCopyFraction (fx, prm, 0.5f, 4.0e-3f);
        std::printf ("  Ensemble corner near-copy fraction = %.3f (pre-fix ~0.46)\n", f);
        check (f < 0.15f, "Ensemble: corner sweep no longer dwells at the floor (was 45.8%)");
        // Sanity: a sane (Center mid, Depth mid) render is finite + wet.
        std::vector<float> in (8192), out (8192);
        for (int i = 0; i < 8192; ++i)
        {
            const double t = static_cast<double> (i) / kRate;
            in[static_cast<size_t> (i)] = 0.5f * static_cast<float> (std::sin (6.28318530718 * 220.0 * t));
        }
        const std::array<float, kNumFxSlotParams> prmSane = { 0.5f, 0.5f, 0.5f, 0.5f, 0.0f };
        runFx (fx, prmSane, in.data(), in.data(), 8192, out.data(), out.data());
        check (allFinite (out.data(), 8192) && rms (out.data(), 1024, 8192) > 1e-3f,
               "Ensemble: sane setting finite + wet");
    }
}

TEST(drive_calib_test)
{
    testDriveCalibration();
    testLevelMonotonic();
    testDcBlockers();
    testLatency();
    testEchoGlide();
    testModDelayClamps();

    std::printf ("\nDRIVE-CALIB TEST: %s (%d failure%s)\n",
                 g_fail == 0 ? "ALL CHECKS PASSED" : "FAILURES", g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0;
}

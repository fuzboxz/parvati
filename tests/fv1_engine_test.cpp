// Standalone verification of the FV-1 emulation framework (Fv1Engine.h /
// Fv1FxProcessor.h). JUCE-FREE: compiles in seconds with
//   clang++ -std=c++17 -O2 -Wall -Wextra -fsanitize=address,undefined \
//     -I Source tests/fv1_engine_test.cpp -o /tmp/fv1_engine_test && /tmp/fv1_engine_test
// No JUCE link needed (the framework is <array>/<cmath>/<cstdint>/<vector> only).

#include <array>
#include <cmath>
#include "unified_test_runner.h"
#include <cstdio>

#include "dsp/fx/fv1/Fv1Engine.h"
#include "dsp/fx/fv1/Fv1FxProcessor.h"
#include "dsp/fx/fv1/Fv1JunoChorus.h"

namespace fv1 = hellcat::fv1;

namespace
{
int g_fail = 0;
void check (bool ok, const char* m)
{
    std::printf ("  %s: %s\n", ok ? "ok  " : "FAIL", m);
    if (! ok) ++g_fail;
}
bool approx (float a, float b, float tol) { return std::fabs (a - b) <= tol; }
} // namespace

// A trivial effect that delays by 1 sample and halves, to exercise the base.
class PassthroughFx : public fv1::Fv1FxProcessor
{
public:
    void processSampleFx (int32_t lin, int32_t rin, int32_t& lout, int32_t& rout) override
    {
        lout = fv1::f24_mulk (lin, 4096); // *0.5 (14-bit: q14(0.5)=4096)
        rout = fv1::f24_mulk (rin, 4096);
    }
    void setParams (const std::array<float, kNumFxSlotParams>&) override {}
    FxType type() const override { return FxType::None; }
};

TEST(fv1_engine_test)
{
    using namespace hellcat::fv1;

    // ---- Fixed-point round-trip ----
    {
        for (float v : { -0.99f, -0.5f, 0.0f, 0.25f, 0.5f, 0.75f, 0.999f })
        {
            const int32_t q = f24_fromFloat (v);
            const float r = f24_toFloat (q);
            check (approx (r, v, 1.5e-7f), "f24 round-trip");
        }
        check (f24_fromFloat (2.0f) == kMaxQ23, "f24 saturates high");
        check (f24_fromFloat (-2.0f) == kMinQ23, "f24 saturates low");
        check (f24_sat (kMaxQ23 + 100) == kMaxQ23, "f24_sat high");
        check (f24_sat (kMinQ23 - 100) == kMinQ23, "f24_sat low");
    }

    // ---- 14-bit coefficient quantization ----
    {
        check (q14 (1.0f) == 8191, "q14 clamps high");
        check (q14 (-1.0f) == -8192, "q14 clamps low");
        check (q14 (0.0f) == 0, "q14 zero");
        // f24_mulk by 0.5 (14-bit) ~= halve.
        const int32_t h = f24_mulk (kOneQ23 >> 1, q14 (0.5f));
        check (approx (f24_toFloat (h), 0.25f, 2e-4f), "f24_mulk 0.5");
    }

    // ---- Grit (bit-truncation) ----
    {
        const int32_t x = f24_fromFloat (0.123456f);
        const int32_t g = f24_quantBits (x, 8); // 8-bit
        // 8-bit keeps top 8 bits (incl sign); low 16 zeroed.
        check ((g & 0xFFFF) == 0, "grit zeros low 16 bits at 8-bit");
        check (f24_quantBits (x, 24) == x, "grit 24-bit is identity");
    }

    // ---- LUTs ----
    {
        check (approx (lutSine32 (0.0f), 0.0f, 1e-3f), "sine LUT at 0");
        check (approx (lutSine32 (0.25f), 1.0f, 1e-3f), "sine LUT peak");
        check (approx (lutSine32 (0.5f), 0.0f, 1e-3f), "sine LUT at 0.5");
        check (approx (lutSine32 (0.75f), -1.0f, 1e-3f), "sine LUT trough");
        check (approx (lutTri32 (0.0f), -1.0f, 1e-3f), "tri LUT at 0");
        check (approx (lutTri32 (0.5f), 1.0f, 1e-3f), "tri LUT peak");
        check (approx (lutTri32 (0.25f), 0.0f, 1e-3f), "tri LUT zero crossing");
    }

    // ---- Delay line ----
    {
        DelayLine<8> d;
        d.clear();
        d.write (f24_fromFloat (0.1f));
        d.write (f24_fromFloat (0.2f));
        d.write (f24_fromFloat (0.3f));
        check (approx (f24_toFloat (d.read (1)), 0.3f, 1e-6f), "delay read(1)");
        check (approx (f24_toFloat (d.read (2)), 0.2f, 1e-6f), "delay read(2)");
        check (approx (f24_toFloat (d.readFrac (1.5f)), 0.25f, 1e-3f), "delay readFrac(1.5)");
    }

    // ---- One-pole LP sanity (DC passes ~unity after settling) ----
    {
        OnePoleLpFx lp;
        lp.setCutoff (2000.0f);
        int32_t y = 0;
        for (int i = 0; i < 4000; ++i)
            y = lp.process (f24_fromFloat (0.5f));
        check (approx (f24_toFloat (y), 0.5f, 1e-2f), "1-pole LP passes DC at 0.5");
    }

    // ---- Allpass unity-gain (1st-order AP is unity magnitude) ----
    {
        Allpass1Fx ap;
        ap.setCoef (0.5f);
        ap.process (f24_fromFloat (0.0f));
        int32_t y = ap.process (f24_fromFloat (0.5f));
        // AP output is bounded; just check finite-ish (within [-1,1]).
        check (y >= kMinQ23 && y <= kMaxQ23, "allpass output in range");
    }

    // ---- RateBridge round-trip at 48 kHz ----
    {
        constexpr int n = 512;
        RateBridge rb;
        rb.prepare (48000.0, n);
        float L[n], R[n], Lo[n], Ro[n];
        // 1 kHz sine at 48k -> must survive the 32.768k round-trip (well under 15k BW).
        for (int i = 0; i < n; ++i)
        {
            const float v = 0.5f * std::sin (6.28318530718f * 1000.0f * static_cast<float> (i) / 48000.0f);
            L[i] = v;
            R[i] = v;
        }
        const int m = rb.hostToInternal (L, R, n);
        check (m > 0 && m < n * 2, "rate bridge produces internal samples");
        // Echo internal back out (effect = identity here).
        rb.internalToHost (Lo, Ro, n);
        bool finite = true, nonzero = false;
        for (int i = 0; i < n; ++i)
        {
            if (! std::isfinite (Lo[i]) || ! std::isfinite (Ro[i])) finite = false;
            if (std::fabs (Lo[i]) > 1e-3f) nonzero = true;
        }
        check (finite, "rate bridge round-trip output finite");
        check (nonzero, "rate bridge round-trip passes 1 kHz tone");
    }

    // ---- RateBridge BW-limit stability across host rates (regression) ----
    // The steep 15 kHz biquad must stay stable even when the host rate is below
    // ~30 kHz (where 15 kHz > fs/2). Before the fc<=0.49*fs clamp the biquad went
    // UNSTABLE at 22050 Hz and emitted inf/NaN. Tone well within the (clamped)
    // cutoff must survive finite at every rate.
    {
        for (double sr : { 22050.0, 32000.0, 44100.0, 48000.0, 96000.0 })
        {
            constexpr int n = 512;
            RateBridge rb;
            rb.prepare (sr, n);
            float L[n], R[n];
            const double f = std::min (1000.0, 0.3 * sr);   // tone below any clamped cutoff
            for (int i = 0; i < n; ++i)
            {
                const float v = 0.4f * std::sin (6.28318530718f * static_cast<float> (f) * static_cast<float> (i) / static_cast<float> (sr));
                L[i] = v; R[i] = v;
            }
            (void) rb.hostToInternal (L, R, n);
            rb.internalToHost (L, R, n);
            bool finite = true;
            for (int i = 0; i < n; ++i)
                if (! std::isfinite (L[i]) || ! std::isfinite (R[i])) finite = false;
            check (finite, "rate bridge stable + finite across host rates (22.05k..96k)");
        }
    }

    // ---- Fv1FxProcessor base: halving passthrough ----
    {
        PassthroughFx fx;
        fx.prepare (48000.0, 256);
        float L[256], R[256];
        for (int i = 0; i < 256; ++i) { L[i] = 0.4f; R[i] = 0.4f; }
        fx.process (L, R, 256);
        // After BW-limit + halve, mid-block steady state ~ 0.2 (allow tolerance).
        float sum = 0.0f;
        for (int i = 128; i < 256; ++i) sum += L[i];
        const float mean = sum / 128.0f;
        check (approx (mean, 0.2f, 0.03f), "Fv1FxProcessor halves DC (0.4 -> ~0.2)");
    }

    // ---- Dual-BBD Chorus (Juno port): documented-configuration pins ----
    // All renders drive processSampleFx directly at the internal rate: no
    // bridge, no resampling, deterministic. The mix law keeps a dry term at
    // every Mix value (the source sums dry internally), so the wet path is
    // extracted analytically: wet = out - q14(dryW)*in, with the exact same
    // fixed-point helpers the effect uses.
    {
        using P = struct Fv1JunoChorusTap : Fv1JunoChorus { using Fv1FxProcessor::processSampleFx; };
        constexpr double sr = kInternalRate;
        const int16_t dry14 = q14 (1.0f - 0.65f);   // the Mix=1.0 dry weight

        // One render at params; returns L/R wet-only pairs (dry subtracted).
        auto renderWet = [&] (float mode, float rate, float depth, float mix,
                              double freqHz, double amp, double durSec,
                              std::vector<float>& wL, std::vector<float>& wR,
                              std::vector<float>* rawL = nullptr)
        {
            P fx;
            fx.setParams ({ { mode, rate, depth, mix, 0.0f } });
            const int n = (int) (durSec * sr);
            wL.assign ((size_t) n, 0.0f);
            wR.assign ((size_t) n, 0.0f);
            if (rawL) rawL->assign ((size_t) n, 0.0f);
            for (int i = 0; i < n; ++i)
            {
                const float x = (float) (amp * std::sin (6.28318530718 * freqHz * i / sr));
                const int32_t xi = f24_fromFloat (x);
                const int32_t dry = f24_mulk (xi, dry14);
                int32_t lo = 0, ro = 0;
                fx.processSampleFx (xi, xi, lo, ro);
                wL[(size_t) i] = f24_toFloat (lo) - f24_toFloat (dry);
                wR[(size_t) i] = f24_toFloat (ro) - f24_toFloat (dry);
                if (rawL) (*rawL)[(size_t) i] = f24_toFloat (lo);
            }
        };

        // (a) OPPOSITE-PHASE LFO: the L/R wet paths drift apart and coincide
        // twice per LFO cycle. A pure tone keeps its envelope under delay
        // modulation (the modulation is a PHASE shift), so the observable is
        // the DIFFERENCE signal: its windowed power swings between ~0 (the
        // phases coincide; the lines read identically) and a maximum. If
        // both lines ran in phase, the difference would stay ~0 forever.
        {
            std::vector<float> wL, wR;
            renderWet (0.0f, 0.5f, 0.5f, 1.0f, 220.0, 0.5, 4.0, wL, wR);
            const int win = 512;
            const int frames = (int) wL.size() / win;
            std::vector<double> diffPow, wetPow;
            for (int f = 0; f < frames; ++f)
            {
                double sd = 0, sw = 0;
                for (int j = 0; j < win; ++j)
                {
                    const double d = wL[(size_t) (f * win + j)] - wR[(size_t) (f * win + j)];
                    const double a = wL[(size_t) (f * win + j)];
                    sd += d * d; sw += a * a;
                }
                diffPow.push_back (sd / win);
                wetPow.push_back (sw / win);
            }
            double dmin = 1e30, dmax = 0, dsum = 0, wsum = 0;
            for (size_t i = 4; i < diffPow.size() - 4; ++i)   // skip the fill edges
            {
                dmin = std::min (dmin, diffPow[i]);
                dmax = std::max (dmax, diffPow[i]);
                dsum += diffPow[i]; wsum += wetPow[i];
            }
            char m[128];
            std::snprintf (m, sizeof (m), "Juno L/R wet paths coincide then diverge (swing %.0fx)", dmax / std::max (1e-12, dmin));
            check (dmax > 10.0 * std::max (1e-12, dmin), m);
            std::snprintf (m, sizeof (m), "Juno L/R differ most of the time (mean diff %.1f%% of wet)", 100.0 * dsum / std::max (1e-12, wsum));
            check (dsum > 0.05 * wsum, m);
        }

        // (b) MODE I vs II: rate roughly doubles and the modulation is
        // deeper. Two honest observables (both validated on the real DSP):
        //   * RATE: cross-correlate the wet L channel against the input per
        //     2048-sample block over a multi-tone probe (one tone aliases
        //     the lag by whole periods; three incommensurate tones pin one
        //     peak). The argmax lag IS the instantaneous line delay. Count
        //     armed level cycles of the smoothed series: mode I ~4.5 over
        //     8 s, mode II ~9.
        //   * DEPTH: a 100 Hz carrier sits in the monotonic region of the PM
        //     carrier law (amplitude ~ J0(beta), beta = 2*pi*f*D). Deeper
        //     modulation drains the coherent carrier into sidebands, so the
        //     coherent amplitude of mode II falls far below mode I.
        {
            auto lfoCycles = [] (float mode) -> int
            {
                P fx;
                fx.setParams ({ { mode, 0.5f, 0.5f, 1.0f, 0.0f } });
                const int n = 8 * (int) kInternalRate;
                std::vector<float> x ((size_t) n), wL ((size_t) n);
                const int16_t dryW14 = q14 (1.0f - 0.65f);   // local name: no shadow
                for (int i = 0; i < n; ++i)
                {
                    const double t = (double) i / kInternalRate;
                    x[(size_t) i] = (float) (0.2 * std::sin (6.28318530718 * 100.0 * t)
                                          + 0.2 * std::sin (6.28318530718 * 233.0 * t)
                                          + 0.2 * std::sin (6.28318530718 * 477.0 * t));
                    const int32_t xi = f24_fromFloat (x[(size_t) i]);
                    int32_t lo = 0, ro = 0;
                    fx.processSampleFx (xi, xi, lo, ro);
                    wL[(size_t) i] = f24_toFloat (lo) - f24_toFloat (f24_mulk (xi, dryW14));
                }
                const int win = 2048;
                std::vector<double> d;
                for (int off = win; off + win < n && (int) d.size() < 120; off += win)
                {
                    double best = -1.0; int bestLag = -1;
                    for (int lag = 600; lag <= 1080; ++lag)
                    {
                        double s = 0;
                        for (int j = 0; j < win; ++j)
                            s += (double) x[(size_t) (off + j)] * (double) wL[(size_t) (off + j - lag)];
                        if (s > best) { best = s; bestLag = lag; }
                    }
                    d.push_back (bestLag);
                }
                std::vector<double> sm;
                for (size_t i = 2; i + 2 < d.size(); ++i)
                    sm.push_back ((d[i - 2] + d[i - 1] + d[i] + d[i + 1] + d[i + 2]) / 5.0);
                double lo = 1e9, hi = -1e9;
                for (double v : sm) { lo = std::min (lo, v); hi = std::max (hi, v); }
                const double mid = (lo + hi) / 2.0, arm = lo + 0.75 * (hi - lo);
                int cycles = 0; bool armed = false;
                for (double v : sm)
                {
                    if (v > arm) armed = true;
                    if (armed && v < mid) { ++cycles; armed = false; }
                }
                return cycles;
            };
            const int cycI  = lfoCycles (0.0f);
            const int cycII = lfoCycles (1.0f);
            char m[96];
            std::snprintf (m, sizeof (m), "mode II LFO rate ~2x mode I (cycles %d vs %d)", cycII, cycI);
            check (cycII * 10 > cycI * 16 && cycII * 10 < cycI * 24, m);
            check (cycI >= 3 && cycI <= 6, "mode I LFO cycle count sane (~4-5 over 8 s)");

            // Depth: coherent 100 Hz carrier amplitude over the full render.
            auto coherentAmp = [&] (float mode) -> double
            {
                std::vector<float> wL, wR;
                renderWet (mode, 0.5f, 0.5f, 1.0f, 100.0, 0.5, 8.0, wL, wR);
                double re = 0, im = 0;
                const int from = (int) (1.0 * kInternalRate);   // skip the fill
                for (size_t i = (size_t) from; i < wL.size(); ++i)
                {
                    const double ph = 6.28318530718 * 100.0 * (double) i / kInternalRate;
                    re += wL[i] * std::cos (ph);
                    im += wL[i] * std::sin (ph);
                }
                const int n = (int) wL.size() - from;
                return 2.0 * std::sqrt (re * re + im * im) / std::max (1, n);
            };
            const double ampI  = coherentAmp (0.0f);
            const double ampII = coherentAmp (1.0f);
            std::snprintf (m, sizeof (m), "mode II drains the carrier deeper (coherent %.4f vs %.4f)", ampII, ampI);
            check (ampII < 0.85 * ampI, m);
        }

        // (c) LINE DELAY: an impulse arrives at the documented line delay.
        // Depth 0 (p2=0) pins the read at the center delay exactly: 25.6 ms
        // at 32768 Hz = 838.86 samples. The first fractional read that
        // touches the impulse sample lands one sample later.
        {
            P fx;
            fx.setParams ({ { 0.0f, 0.5f, 0.0f, 1.0f, 0.0f } });
            const int n = 1200;
            int arrival = -1;
            for (int i = 0; i < n; ++i)
            {
                const int32_t x = f24_fromFloat (i == 0 ? 0.5f : 0.0f);
                int32_t lo = 0, ro = 0;
                fx.processSampleFx (x, x, lo, ro);
                if (i > 5 && std::fabs (f24_toFloat (lo)) > 0.02f)
                {
                    arrival = i;
                    break;
                }
            }
            char m[96];
            std::snprintf (m, sizeof (m), "wet impulse arrives at the line delay (t=%d, want ~839)", arrival);
            check (arrival >= 837 && arrival <= 842, m);
        }

        // (d) MIX LAW: Mix 0 is a dry passthrough (to 14-bit precision).
        {
            P fx;
            fx.setParams ({ { 0.0f, 0.5f, 0.5f, 0.0f, 0.0f } });
            double worst = 0.0;
            for (int i = 0; i < 4096; ++i)
            {
                const float x = 0.3f * std::sin (6.28318530718f * 440.0f * static_cast<float> (i) / (float) kInternalRate);
                const int32_t xi = f24_fromFloat (x);
                int32_t lo = 0, ro = 0;
                fx.processSampleFx (xi, xi, lo, ro);
                worst = std::max (worst, (double) std::fabs (f24_toFloat (lo) - x));
            }
            char m[96];
            std::snprintf (m, sizeof (m), "Mix 0 passes dry (worst %.5f, q14 floor ~1e-4)", worst);
            check (worst < 1.0e-3, m);
        }

        // (e) BAND-LIMITING: mode I rolls 12 kHz hard; mode II stays
        // brighter (the documented 106-II brightness difference).
        {
            auto wetRatio = [&] (float mode) -> double
            {
                std::vector<float> wL1, wR1, wL12, wR12;
                renderWet (mode, 0.5f, 0.0f, 1.0f, 1000.0, 0.5, 3.0, wL1, wR1);
                renderWet (mode, 0.5f, 0.0f, 1.0f, 12000.0, 0.5, 3.0, wL12, wR12);
                auto rmsTail = [] (const std::vector<float>& v) -> double
                {
                    double s = 0; int n = 0;
                    for (size_t i = v.size() / 3; i < v.size(); ++i) { s += (double) v[i] * v[i]; ++n; }
                    return std::sqrt (s / std::max (1, n));
                };
                return rmsTail (wL12) / std::max (1e-9, rmsTail (wL1));
            };
            const double rI  = wetRatio (0.0f);
            const double rII = wetRatio (1.0f);
            char m[96];
            std::snprintf (m, sizeof (m), "mode I wet path rolls off 12 kHz (ratio %.2f < 0.5)", rI);
            check (rI < 0.5, m);
            std::snprintf (m, sizeof (m), "mode II wet path stays brighter (ratio %.2f > 1.3x mode I)", rII);
            check (rII > rI * 1.3, m);
        }
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_fail ? "FV1 ENGINE TEST: FAILURES" : "FV1 ENGINE TEST: ALL CHECKS PASSED",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0;
}

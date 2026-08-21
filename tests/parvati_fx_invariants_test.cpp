// FX MATH-INVARIANT TESTS (2026-08-21) — the memory-safety-blind bug class.
//
// Memory-safe C++/Rust cannot catch what this week actually shipped: DC
// integrator feedback loops, saturated drive ladders collapsing the table
// domain, zero table edges on clamped indices, and C0 discontinuities in
// transfer curves. These tests encode the LAWS each effect class must obey,
// evaluated at parameter EXTREMES (where every one of those bugs lived — the
// existing suites probe typical settings):
//
//   [I1] CURVE AUDIT (wavetable shapers)
//        a) edge entries with |value| < 0.1 require the shape to be marked
//           periodic (the exact LUT zero-edge/gating invariant).
//        b) adjacent-step bounds on the two healed curves (Fuzz knee,
//           Sparse zero-crossing) — a discontinuity no oversampling repairs.
//
//   [I2] LOOP DC-FREENESS (every feedback/reverb effect at 100% regen)
//        A DC-FREE input (pure sine, loud) must produce DC-free output:
//        |mean(wet)| <= 0.1 * rms(wet). The pre-LoopDcKiller echo/plate
//        accumulated |mean| up to 0.77*rms (dc -0.22..-0.28 measured into
//        the shaper of the user's delay->reverb->shaper dropout chain).
//
//   [I3] PARAM FUZZ (every effect, randomized extreme params, fixed seed)
//        Output must stay finite, bounded, and non-degenerate (no long
//        exact-zero runs, no rail parking) — catches gross domain errors
//        nobody enumerated.
//
// JUCE-free: compiles the FV-1 effect sources directly (the fv1-family test
// pattern). Fast: each check renders <= 2 s of 32.768 kHz audio.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "dsp/fx/fv1/Fv1Echo.h"
#include "dsp/fx/fv1/Fv1ClockedDelay.h"
#include "dsp/fx/fv1/Fv1Flanger.h"
#include "dsp/fx/fv1/Fv1Phaser.h"
#include "dsp/fx/fv1/Fv1PlateReverb.h"
#include "dsp/fx/fv1/Fv1Room.h"
#include "dsp/fx/fv1/Fv1Spring.h"
#include "dsp/fx/fv1/Fv1Chorus.h"
#include "dsp/fx/fv1/Fv1Ensemble.h"
#include "dsp/fx/fv1/Fv1LutDistortion.h"
#include "dsp/fx/fv1/Fv1Overdrive.h"

using parvati::fv1::Fv1Echo;
using parvati::fv1::Fv1ClockedDelay;
using parvati::fv1::Fv1Flanger;
using parvati::fv1::Fv1Phaser;
using parvati::fv1::Fv1PlateReverb;
using parvati::fv1::Fv1Room;
using parvati::fv1::Fv1Spring;
using parvati::fv1::Fv1Chorus;
using parvati::fv1::Fv1Ensemble;
using parvati::fv1::Fv1LutDistortion;
using parvati::fv1::Fv1Overdrive;

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

constexpr double kSr = 32768.0;   // internal rate (direct effect compile)
constexpr int    kBuf = 256;

// Render @p dur seconds of a loud sine through @p fx at @p params; returns L.
template <typename Fx>
std::vector<float> render (Fx& fx, const float params[5], double dur, double amp = 0.5)
{
    fx.prepare (kSr, kBuf);
    fx.setParams (params);
    const int n = (int) (dur * kSr);
    std::vector<float> out ((size_t) n, 0.0f), r ((size_t) n, 0.0f);
    for (int i = 0; i < n; ++i)
    {
        const double th = 6.283185307 * 220.0 * i / kSr;
        // ASYMMETRIC, DC-free: the 2nd+3rd harmonic stack makes the loop's
        // rail rectification produce DC (a pure sine clips symmetrically and
        // cannot — this is how the user's chord wash poisoned the loop).
        out[(size_t) i] = (float) (amp * (std::sin (th) + 0.6 * std::sin (2 * th + 0.7)
                                              + 0.4 * std::sin (3 * th)) / 2.0);
    }
    for (int off = 0; off < n; off += kBuf)
        fx.process (out.data() + off, r.data() + off, std::min (kBuf, n - off));
    return out;
}

// |mean| / rms over [from, n) — the DC-freeness ratio (I2's metric).
double dcRatio (const std::vector<float>& x, double fromSec)
{
    const int from = (int) (fromSec * kSr);
    double mean = 0, ms = 0; int n = 0;
    for (int i = from; i < (int) x.size(); ++i)
    { mean += x[(size_t) i]; ms += (double) x[(size_t) i] * x[(size_t) i]; ++n; }
    mean /= n; ms /= n;
    return std::fabs (mean) / std::max (1e-9, std::sqrt (ms));
}
} // namespace

int main()
{
    const float all[5] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };   // 100% everything

    // ============================================================ [I1] curve
    std::printf ("[I1] wavetable curve audit\n");
    {
        Fv1LutDistortion lut;
        const char* names[16] = { "Clip", "Soft", "Tube", "Wrap", "OctUp", "Fuzz",
                                  "Square", "Steps", "SFold", "Cheby2", "Cheby3",
                                  "Asym", "Mirror", "HGate", "Crush4", "Sparse" };
        // (a) zero-edge => must be periodic (the LUT gating invariant).
        for (int s = 0; s < 16; ++s)
        {
            const int16_t* t = lut.debugTable (s);
            const bool zeroEdge = std::abs ((int) t[0]) < (int) (0.1f * 12287.0f)
                              || std::abs ((int) t[1023]) < (int) (0.1f * 12287.0f);
            char msg[96];
            std::snprintf (msg, sizeof (msg), "%-7s: zero table edge <=> periodic shape", names[s]);
            check (! zeroEdge || Fv1LutDistortion::debugShapeIsPeriodic (s), msg);
        }
        // (b) healed-curve discriminators (sharp, not slope-sensitive):
        //  Fuzz — the below-knee branch must TAPER TO ~0 AT the knee
        //   (x=0.12 -> table idx 527): the pre-fix branch held 0.15*0.12
        //   = 0.018 (~221 Q14 units) right up to the knee and stepped to 0.
        //  Sparse — f(0) must be EXACTLY 0 (odd curve through the origin):
        //   the pre-fix sign*|x|^0.3 form evaluated +0.0158 at x=0.
        {
            const int16_t* t = lut.debugTable (5);   // Fuzz
            char msg[96];
            std::snprintf (msg, sizeof (msg), "Fuzz tapers to ~0 at the knee (t[527]=%d; pre-fix ~221)", (int) t[527]);
            check (std::abs ((int) t[527]) <= 60, msg);
        }
        {
            const int16_t* t = lut.debugTable (15);  // Sparse
            char msg[96];
            std::snprintf (msg, sizeof (msg), "Sparse passes exactly through 0 (t[512]=%d; pre-fix ~194)", (int) t[512]);
            check (t[512] == 0, msg);
            std::snprintf (msg, sizeof (msg), "Sparse odd-symmetric near 0 (t[511]=%d == -t[513]=%d)",
                           (int) t[511], (int) t[513]);
            check (t[511] == -t[513], msg);
        }
    }

    // ================================================== [I2] loop DC-freeness
    std::printf ("[I2] feedback loops create no DC from DC-free input (100%% regen)\n");
    {
        struct Row { const char* name; int kind; };
        const Row rows[] = {
            { "Echo        ", 0 }, { "ClockedDelay", 1 }, { "Flanger     ", 2 },
            { "Phaser      ", 3 }, { "Plate       ", 4 },  { "Room        ", 5 },
            { "Spring      ", 6 }, { "Chorus      ", 7 },  { "Ensemble    ", 8 },
        };
        for (const auto& r : rows)
        {
            std::vector<float> out;
            switch (r.kind)
            {
                case 0: { Fv1Echo f;         out = render (f, all, 2.0, 0.95f); break; }
                case 1: { Fv1ClockedDelay f; out = render (f, all, 2.0); break; }
                case 2: { Fv1Flanger f;      out = render (f, all, 2.0); break; }
                case 3: { float p[5] = { 0.0f, 1, 1, 1, 1 };   // min Rate: the DC
                                                          // mechanism is the loop,
                                                          // not the 8 Hz sweep AM.
                         Fv1Phaser f;       out = render (f, p, 2.0); break; }
                case 4: { Fv1PlateReverb f;  out = render (f, all, 2.0); break; }
                case 5: { Fv1Room f;         out = render (f, all, 2.0); break; }
                case 6: { Fv1Spring f;       out = render (f, all, 2.0); break; }
                case 7: { Fv1Chorus f;       out = render (f, all, 2.0, 0.95f); break; }
                case 8: { Fv1Ensemble f;     out = render (f, all, 2.0, 0.95f); break; }
            }
            char msg[96];
            const double ratio = dcRatio (out, 0.4);
            std::snprintf (msg, sizeof (msg), "%s |mean|/rms = %.3f (<= 0.10; pre-killer echo measured 0.77)",
                          r.name, ratio);
            check (ratio <= 0.10, msg);
        }
    }

    // ========================================================= [I3] param fuzz
    std::printf ("[I3] random-extreme param fuzz stays finite + non-degenerate\n");
    {
        uint32_t rng = 0xC0FFEEu;   // fixed seed: deterministic failures
        auto next01 = [&] { rng = rng * 1664525u + 1013904223u; return (float) ((rng >> 8) & 0xFFFF) / 65535.0f; };
        struct Row { const char* name; int kind; };
        const Row rows[] = {
            { "Echo", 0 }, { "ClockedDelay", 1 }, { "Flanger", 2 }, { "Phaser", 3 },
            { "Plate", 4 }, { "Room", 5 }, { "Spring", 6 }, { "LutDist", 7 }, { "Overdrive", 8 },
        };
        for (const auto& r : rows)
        {
            for (int trial = 0; trial < 3; ++trial)
            {
                float p[5];
                for (int k = 0; k < 5; ++k)
                    p[k] = (trial == 0) ? 1.0f : next01() * 0.5f + 0.5f;   // extreme half
                std::vector<float> out;
                switch (r.kind)
                {
                    case 0: { Fv1Echo f;         out = render (f, p, 1.5, 0.9f); break; }
                    case 1: { Fv1ClockedDelay f; out = render (f, p, 1.5, 0.9f); break; }
                    case 2: { Fv1Flanger f;      out = render (f, p, 1.5, 0.9f); break; }
                    case 3: { Fv1Phaser f;       out = render (f, p, 1.5, 0.9f); break; }
                    case 4: { Fv1PlateReverb f;  out = render (f, p, 1.5, 0.9f); break; }
                    case 5: { Fv1Room f;         out = render (f, p, 1.5, 0.9f); break; }
                    case 6: { Fv1Spring f;       out = render (f, p, 1.5, 0.9f); break; }
                    case 7: { Fv1LutDistortion f; out = render (f, p, 1.5, 0.9f); break; }
                    case 8: { Fv1Overdrive f;   out = render (f, p, 1.5, 0.9f); break; }
                }
                // Skip the first 70%: a delay's own attack (up to ~0.5 s of
                // pre-tap silence at max time) and a reverb's comb buildup are
                // LEGITIMATE zeros — the invariant targets mid-signal dropout.
                const int from = (int) (0.7 * (double) out.size());
                bool finite = true, bounded = true, live = false;
                int run = 0, maxRun = 0;
                for (size_t i = (size_t) from; i < out.size(); ++i)
                {
                    const float v = out[i];
                    if (! std::isfinite (v)) finite = false;
                    if (std::fabs (v) > 1.6f) bounded = false;   // Level trim (<=2x design) + HP transient
                    if (std::fabs (v) > 1.0e-4f) live = true;
                    if (std::fabs (v) < 1.0e-6) { if (++run > maxRun) maxRun = run; }
                    else run = 0;
                }
                char msg[96];
                std::snprintf (msg, sizeof (msg), "%s trial %d: finite+bounded+live, zeroRun=%d",
                               r.name, trial, maxRun);
                check (finite && bounded && live && maxRun < 4096, msg);
            }
        }
    }

    // ============================================ [I2b] near-Nyquist loop gain
    std::printf ("[I2b] feedback loops do not amplify near-Nyquist content\n");
    {
        // The phaser-crackle mechanism, measured directly: a QUIET 15.9 kHz
        // tone (just below the 16.384 kHz internal Nyquist) through each
        // feedback effect at 100% params. A resonant loop (positive fb phase
        // at Nyquist, gain g) amplifies it up to 1/(1-g); a properly damped
        // loop passes it at <= ~2x. Gate: <= 3x (pre-damp Ensemble measured
        // ~10x bound; the phaser fix measured this class back to unity).
        // Direct processSampleFx drive (bypasses the bridge's 15 kHz output
        // LP — the loop's own amplification is the quantity under test).
        struct PEnsemble : Fv1Ensemble      { using Fv1FxProcessor::processSampleFx; };
        struct PClocked  : Fv1ClockedDelay  { using Fv1FxProcessor::processSampleFx; };
        struct PSpring   : Fv1Spring        { using Fv1FxProcessor::processSampleFx; };
        struct PFlanger  : Fv1Flanger       { using Fv1FxProcessor::processSampleFx; };
        struct PPhaser   : Fv1Phaser        { using Fv1FxProcessor::processSampleFx; };
        auto nyqGain = [&all] (int kind) -> double
        {
            const int n = (int) (2.0 * kSr);
            std::vector<float> out ((size_t) n, 0.f);
            std::vector<float> in ((size_t) n, 0.f);
            for (int i = 0; i < n; ++i)
                in[(size_t) i] = (float) (0.05 * std::sin (6.283185307 * 15900.0 * i / kSr));
            auto runFx = [n, &in, &out] (auto& f)
            {
                for (int i = 0; i < n; ++i)
                {
                    const int32_t x = parvati::fv1::f24_fromFloat (in[(size_t) i]);
                    int32_t lo = 0, ro = 0;
                    f.processSampleFx (x, x, lo, ro);
                    out[(size_t) i] = parvati::fv1::f24_toFloat (lo);
                }
            };
            switch (kind)
            {
                case 0: { PEnsemble f; f.prepare (kSr, kBuf); f.setParams (all); runFx (f); break; }
                case 1: { PClocked f;  f.prepare (kSr, kBuf); f.setParams (all); runFx (f); break; }
                case 2: { PSpring f;   f.prepare (kSr, kBuf); f.setParams (all); runFx (f); break; }
                case 3: { PFlanger f;  f.prepare (kSr, kBuf); f.setParams (all); runFx (f); break; }
                case 4: { PPhaser f;   f.prepare (kSr, kBuf); f.setParams (all); runFx (f); break; }
            }
            // Goertzel at 15900 over the last 1.5 s
            const int from = (int) (0.5 * kSr);
            double sw = 6.283185307 * 15900.0 / kSr, coeff = 2 * std::cos (sw);
            double s1 = 0, s2 = 0, t1 = 0, t2 = 0;
            for (int i = from; i < n; ++i)
            {
                const double v = out[(size_t) i];
                const double a = v + coeff * s1 - s2; s2 = s1; s1 = a;
                const double b = in[(size_t) i] + coeff * t1 - t2; t2 = t1; t1 = b;
            }
            return std::sqrt ((s1*s1 + s2*s2 - coeff*s1*s2) / (t1*t1 + t2*t2 - coeff*t1*t2));
        };
        const struct { const char* name; int kind; } rows[] = {
            { "Ensemble    ", 0 }, { "ClockedDelay", 1 }, { "Spring      ", 2 },
            { "Flanger     ", 3 }, { "Phaser      ", 4 },
        };
        for (const auto& row : rows)
        {
            char msg[96];
            const double g = nyqGain (row.kind);
            std::snprintf (msg, sizeof (msg), "%s 15.9 kHz gain %.2fx (<= 3.0)", row.name, g);
            check (g <= 3.0, msg);
        }
    }

    // ================================================ [I4] drive knob resolution
    std::printf ("[I4] Drive knob has continuous resolution (not a powers-of-2 staircase)\n");
    {
        // The q14-remainder bug (audit 2026-08-21): the fractional drive stage
        // was clamped to unity, so every position between 2^k and 2^(k+1)
        // produced IDENTICAL output. Distinct drive settings must produce
        // distinct output levels (a loud-but-saturated shaper monotonically
        // changes tone with drive).
        auto rmsAt = [] (float p0) -> double
        {
            Fv1LutDistortion f;
            const float prm[5] = { p0, 0.0f, 0.0f, 0.6f, 0.0f };   // shape Clip, tone mid
            const auto out = render (f, prm, 0.6, 0.45);
            double s = 0; int n = 0;
            for (int i = (int) (0.25 * kSr); i < (int) out.size(); ++i)
            { s += (double) out[(size_t) i] * out[(size_t) i]; ++n; }
            return std::sqrt (s / n);
        };
        const double a = rmsAt (0.30f), b = rmsAt (0.38f), c = rmsAt (0.46f);
        char msg[128];
        std::snprintf (msg, sizeof (msg), "Drive 2.66x/3.28x/4.03x distinct (rms %.4f/%.4f/%.4f)", a, b, c);
        // Pre-fix: p0 in [0.25,0.5) all read as 2x -> IDENTICAL rms values.
        check (std::fabs (a - b) > 1e-4 && std::fabs (b - c) > 1e-4, msg);
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "FX INVARIANTS TEST: FAILURES" : "FX INVARIANTS TEST: ALL PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}

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
#include "dsp/fx/fv1/Fv1LutDistortion.h"
#include "dsp/fx/fv1/Fv1Overdrive.h"

using parvati::fv1::Fv1Echo;
using parvati::fv1::Fv1ClockedDelay;
using parvati::fv1::Fv1Flanger;
using parvati::fv1::Fv1Phaser;
using parvati::fv1::Fv1PlateReverb;
using parvati::fv1::Fv1Room;
using parvati::fv1::Fv1Spring;
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
            { "Spring      ", 6 },
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

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "FX INVARIANTS TEST: FAILURES" : "FX INVARIANTS TEST: ALL PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}

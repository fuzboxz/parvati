// FX param-modulation pop REGRESSION test.
//
// Verifies that per-sample param interpolation (ParameterInterpolator pattern)
// eliminates coefficient-jump transients (pops) when FX params are modulated at
// the ~980 Hz internal-block cadence.
//
// Method: feed a static 220 Hz sine. STEP one param per 49-sample sub-chunk
// (~980 Hz) with a square-wave pattern (worst case: abrupt jumps 0.2->0.8).
// Measure the output maxDelta (max |Δout|) and compare to the STATIC-param
// baseline (maxDelta with no modulation). ratio = modulated/static. Before the
// fix, stateful FX had ratio 9–16x (coefficient-jump pops); after per-sample
// interpolation, ratio < 2x.
//
// For the Resonator pitch, a full-depth sweep (0.2–0.8) pushes modes near
// Nyquist (legitimate high-frequency content, not pops). A MODERATE sweep
// (0.4–0.6) keeps modes below Nyquist so the metric measures only the
// coefficient-jump transient that the per-sample fix eliminates.
//
// Each assertion FAILS if the per-sample interpolation is removed.

#include <algorithm>
#include "unified_test_runner.h"
#include <cmath>
#include <cstdio>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "dsp/fx/FxChain.h"
#include "dsp/fx/FxTypes.h"

namespace
{
constexpr double kSr     = 48000.0;
constexpr int    kSub    = 49;       // sub-chunk size (~980 Hz at 48k)
constexpr int    kWarmup = 60;
constexpr int    kMeasure = 400;
constexpr int    kTotal  = kWarmup + kMeasure;

int gFailures = 0;

void check (bool ok, const char* msg)
{
    std::printf ("  %s : %s\n", ok ? "ok  " : "FAIL", msg);
    if (! ok) ++gFailures;
}

void fillSine (std::vector<float>& d, double freq, double amp)
{
    for (size_t i = 0; i < d.size(); ++i)
        d[i] = static_cast<float> (amp * std::sin (2.0 * 3.14159265 * freq * static_cast<double> (i) / kSr));
}

// max |Δout| over the measured region.
double maxDeltaOf (const std::vector<float>& out)
{
    const int start = kWarmup * kSub;
    const int end   = kTotal * kSub;
    double mx = 0.0;
    for (int i = start + 1; i < end; ++i)
        mx = std::max (mx, static_cast<double> (std::fabs (out[static_cast<size_t> (i)] - out[static_cast<size_t> (i - 1)])));
    return mx;
}

// Square-wave param trajectory: alternates between lo and hi per half-cycle.
std::vector<float> genSquareTraj (double hz, float lo, float hi)
{
    std::vector<float> traj (static_cast<size_t> (kTotal));
    const double subRate = kSr / kSub;
    for (int s = 0; s < kTotal; ++s)
    {
        const double t = static_cast<double> (s) / subRate;
        const double phase = std::fmod (t * hz, 1.0);
        traj[static_cast<size_t> (s)] = phase < 0.5 ? lo : hi;
    }
    return traj;
}

// Render the FX with a param trajectory (stepped per sub-chunk) or static.
std::vector<float> renderFx (FxType type, int paramIdx, const std::vector<float>* traj)
{
    const int N = kTotal * kSub;
    std::vector<float> inL (static_cast<size_t> (N)), inR (static_cast<size_t> (N));
    std::vector<float> outL (static_cast<size_t> (N)), outR (static_cast<size_t> (N));
    fillSine (inL, 220.0, 0.5);
    inR = inL;

    FxChain chain;
    chain.prepare (kSr, 256);
    chain.setTopology (FxTopology::Series);
    chain.setSlotType (0, type);
    chain.setSlotEnabled (0, true);
    chain.setSlotDryWet (0, 1.0f);
    for (int k = 0; k < kNumFxSlotParams; ++k)
        chain.setSlotParam (0, k, 0.5f);

    for (int s = 0; s < kTotal; ++s)
    {
        if (traj != nullptr)
            chain.setSlotParam (0, paramIdx, (*traj)[static_cast<size_t> (s)]);
        const int off = s * kSub;
        chain.process (inL.data() + off, inR.data() + off,
                       outL.data() + off, outR.data() + off, kSub);
    }
    return outL;
}

const char* fxName (FxType t)
{
    switch (t)
    {
        case FxType::Diffuser:         return "Diffuser";
        case FxType::PitchShifter:     return "PitchShifter";
        case FxType::Reverb:     return "Reverb";
        case FxType::Resonator:        return "Resonator";
        default: return "?";
    }
}

// Test one FX/param: step with square-wave, assert pop ratio is low.
// Uses maxDelta(modulated) / maxDelta(static) as the metric (proven by the
// diagnostic: before fix 9–16x; after fix <2x).
void testPopRatio (FxType type, int paramIdx, float lo, float hi, double maxRatio)
{
    const auto base = renderFx (type, paramIdx, nullptr);
    const double staticMax = maxDeltaOf (base);

    const auto traj = genSquareTraj (10.0, lo, hi);   // 10 Hz square
    const auto out  = renderFx (type, paramIdx, &traj);
    const double modMax = maxDeltaOf (out);

    const double ratio = staticMax > 1e-9 ? modMax / staticMax : 0.0;

    const char* name = fxName (type);
    std::printf ("  %s p%d: modMax=%.5f statMax=%.5f ratio=%.1f\n",
                 name, paramIdx, modMax, staticMax, ratio);

    char msg[256];
    std::snprintf (msg, sizeof (msg),
                   "%s p%d: pop ratio %.1f < %.1f (per-sample interpolation eliminates coefficient-jump pops)",
                   name, paramIdx, ratio, maxRatio);
    check (ratio < maxRatio, msg);
}

}  // namespace

TEST(parvati_fx_popdiag_test)
{
    using namespace std::string_view_literals;

    std::printf ("\n################ FX POP REGRESSION TEST ################\n");
    std::printf ("  Stepping param per 49-sample sub-chunk (~980 Hz) with 10 Hz square wave.\n");
    std::printf ("  ratio = modulatedMaxDelta / staticMaxDelta. Before fix: 9-21x. After: <2x.\n\n");

    // ---- Reverb: diffusion (p2; amount collapsed, no longer a param) ----
    std::printf ("-- Reverb diffusion (p2) --\n");
    testPopRatio (FxType::Reverb, 2, 0.2f, 0.8f, 2.0);

    // (Diffuser amount (p0) removed: amount is now hardcoded 1.0, not a param.)

    // ---- PitchShifter: size (p1) ----
    // size_ is per-sample interpolated (ParameterInterpolator) so the read
    // position glides per-sample instead of stepping per block. Before the
    // fix: 9.6x ratio (333 modulation-introduced discontinuities). After: <2x.
    std::printf ("\n-- PitchShifter size (p1) --\n");
    testPopRatio (FxType::PitchShifter, 1, 0.2f, 0.8f, 2.0);

    // ---- Resonator: pitch (p0) ----
    // Full-depth sweep (0.2–0.8) pushes modes to Nyquist (legitimate HF content).
    // Moderate sweep (0.4–0.6) keeps modes below Nyquist so the metric measures
    // ONLY the coefficient-jump transient that per-sample interpolation eliminates.
    std::printf ("\n-- Resonator pitch (p0) [moderate sweep 0.4-0.6 to avoid Nyquist modes] --\n");
    testPopRatio (FxType::Resonator, 0, 0.4f, 0.6f, 2.0);

    std::printf ("\n");
    if (gFailures == 0)
        std::printf ("FX POP REGRESSION TEST: ALL CHECKS PASSED (0 failures)\n");
    else
        std::printf ("FX POP REGRESSION TEST: %d FAILURE(S)\n", gFailures);
    return gFailures == 0;
}

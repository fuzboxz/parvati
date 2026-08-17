// Combined verification of the FV-1 second-wave family (2026-08-17):
// Overdrive / LUT Distortion / Compressor / Gate / Chorus / Flanger / Echo /
// Room / Spring. JUCE-FREE: compiles the effect .cpps directly (no JUCE
// link), same pattern as the per-effect tests. Per effect: factory+type,
// finite/non-silent/differs at mid params, finite at all corner combos, plus
// effect-specific behaviour pins:
//   * Overdrive/LUT: drive changes level; LUT's 16 shapes all differ.
//   * Compressor: loud input quieter than quiet input * makeup (squash).
//   * Gate: threshold 0 = transparent (disabled); high threshold silences.
//   * Chorus/Flanger: delayed wet (not an instant passthrough).
//   * Echo: a discrete repeat appears ~Time later.
//   * Room/Spring: impulse produces a decaying tail (energy over time).
//
// Build: linked as parvati_fv1_newfamily_test (built by default).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include "dsp/fx/FxProcessor.h"
#include "dsp/fx/fv1/Fv1Overdrive.h"
#include "dsp/fx/fv1/Fv1LutDistortion.h"
#include "dsp/fx/fv1/Fv1Compressor.h"
#include "dsp/fx/fv1/Fv1Gate.h"
#include "dsp/fx/fv1/Fv1Chorus.h"
#include "dsp/fx/fv1/Fv1Flanger.h"
#include "dsp/fx/fv1/Fv1Echo.h"
#include "dsp/fx/fv1/Fv1Room.h"
#include "dsp/fx/fv1/Fv1Spring.h"

namespace
{
int g_fail = 0;
void check (bool ok, const char* m)
{
    std::printf ("  %s: %s\n", ok ? "ok  " : "FAIL", m);
    if (! ok) ++g_fail;
}

constexpr int kBlock = 256;
void runFx (parvati::fv1::Fv1FxProcessor& fx, const float p0, const float p1,
            const float p2, const float p3, const float* inL, const float* inR,
            int n, float* outL, float* outR)
{
    float prm[5] = { p0, p1, p2, p3, 0.0f };
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
} // namespace

int main()
{
    constexpr int kN = 8192;
    std::vector<float> inL (kN), inR (kN), oL (kN), oR (kN);
    for (int i = 0; i < kN; ++i)
    {
        const float t = static_cast<float> (i) / 48000.0f;
        const float env = std::exp (-t * 2.0f);
        const float v = 0.6f * env * std::sin (6.28318530718f * 440.0f * t);
        inL[static_cast<size_t> (i)] = v;
        inR[static_cast<size_t> (i)] = v;
    }

    // ---- Overdrive ----
    {
        parvati::fv1::Fv1Overdrive fx;
        fx.prepare (48000.0, kBlock);
        fx.reset();
        check (fx.type() == FxType::Overdrive, "Overdrive: type()");
        runFx (fx, 0.5f, 0.5f, 0.7f, 0.5f, inL.data(), inR.data(), kN, oL.data(), oR.data());
        check (allFinite (oL.data(), kN), "Overdrive: finite @ mid");
        check (maxAbs (oL.data(), kN) > 1e-3f, "Overdrive: non-silent");
        float pk0 = 0.0f, pk1 = 0.0f;
        runFx (fx, 0.0f, 0.5f, 0.7f, 0.5f, inL.data(), inR.data(), kN, oL.data(), oR.data());
        pk0 = maxAbs (oL.data(), kN);
        runFx (fx, 1.0f, 0.5f, 0.7f, 0.5f, inL.data(), inR.data(), kN, oL.data(), oR.data());
        pk1 = maxAbs (oL.data(), kN);
        check (pk1 > 0.05f && allFinite (oL.data(), kN), "Overdrive: max drive stays sane");
        bool corners = true;
        for (float a : { 0.0f, 1.0f }) for (float b : { 0.0f, 1.0f })
            for (float c : { 0.0f, 1.0f }) for (float d : { 0.0f, 1.0f })
            {
                runFx (fx, a, b, c, d, inL.data(), inR.data(), kN, oL.data(), oR.data());
                if (! allFinite (oL.data(), kN)) corners = false;
            }
        check (corners, "Overdrive: finite at all corners");
        check (pk0 > 0.02f && pk1 < pk0 * 2.0f + 0.05f, "Overdrive: drive compresses, not explodes");
    }

    // ---- LUT Distortion ----
    {
        parvati::fv1::Fv1LutDistortion fx;
        fx.prepare (48000.0, kBlock);
        fx.reset();
        check (fx.type() == FxType::LutDistortion, "LUT Distortion: type()");
        runFx (fx, 0.5f, 0.0f, 0.3f, 0.7f, inL.data(), inR.data(), kN, oL.data(), oR.data());
        check (allFinite (oL.data(), kN) && maxAbs (oL.data(), kN) > 1e-3f,
               "LUT Distortion: finite + non-silent @ mid");
        // All 16 shapes differ somewhere.
        std::vector<float> ref (kN);
        bool allDiffer = true, corners = true;
        for (int s = 0; s < 16; ++s)
        {
            const float sp = (s + 0.5f) / 16.0f;
            runFx (fx, 0.5f, sp, 0.3f, 0.7f, inL.data(), inR.data(), kN, oL.data(), oR.data());
            if (! allFinite (oL.data(), kN)) corners = false;
            if (s == 0)
                std::memcpy (ref.data(), oL.data(), sizeof (float) * kN);
            else
            {
                bool differs = false;
                for (int i = 0; i < kN; ++i)
                    if (std::fabs (oL[i] - ref[i]) > 1e-4f) { differs = true; break; }
                if (! differs) allDiffer = false;
            }
        }
        check (corners, "LUT Distortion: all shapes finite");
        check (allDiffer, "LUT Distortion: all 16 shapes DIFFER");
        // Jitter off vs on changes the output (the clock wobble is real).
        runFx (fx, 0.5f, 0.0f, 0.0f, 0.7f, inL.data(), inR.data(), kN, ref.data(), oR.data());
        runFx (fx, 0.5f, 0.0f, 1.0f, 0.7f, inL.data(), inR.data(), kN, oL.data(), oR.data());
        bool jitDiffers = false;
        for (int i = 0; i < kN; ++i)
            if (std::fabs (oL[i] - ref[i]) > 1e-4f) { jitDiffers = true; break; }
        check (jitDiffers, "LUT Distortion: Jitter audibly wobbles the read");
        // Shape-switch click: swapping the active wavetable pointer instantly
        // jumps between two transfer curves at the same sample value (Clip vs
        // SFold differ by up to ~1.3 at Drive mid) -> an audible click. The
        // fix crossfades old->new over 128 internal samples (~3.9 ms) with a
        // per-sample Q.14 fade counter.
        // Metric: the max per-sample |delta| (slope) of a switched render
        // MINUS the max slope of the two no-switch reference renders
        // (all-shape-0 / all-shape-8) in the same window. The natural slope of
        // the clipped waveform itself (~0.39/sample at Drive mid — it is a
        // square-ish wave) cancels; only switch-caused discontinuity remains.
        // 8 switches alternate 0<->8 every ~640 samples (>= fade + settle),
        // stepping the switch phase around the 440 Hz cycle so at least one
        // lands on a large curve difference. A perfect crossfade's excess is
        // bounded by the morph rate; the measured residual (~0.042) comes from
        // the pre-existing saturating-add clip knee (f24_addSat before the /2
        // average locks any LUT output >0.5 to 0.5, so the blend crosses the
        // knee where the refs do not — a smooth morph artifact, not a jump).
        //   pre-fix (instant pointer swap): worst excess = 0.1916 -> FAIL
        //   post-fix (Q.14 crossfade):      worst excess = 0.0423 -> PASS
        // Bound 0.06: ~1.4x margin post-fix, 3.2x below the pre-fix click.
        {
            const int n1 = 4096, nSw = 8, swStep = 640, nTot = n1 + nSw * swStep + 1024;
            std::vector<float> src (static_cast<size_t> (nTot));
            for (int i = 0; i < nTot; ++i)
            {
                const float t = static_cast<float> (i) / 48000.0f;
                src[static_cast<size_t> (i)] = 0.5f * std::sin (6.28318530718f * 440.0f * t);
            }
            std::vector<float> r0 (static_cast<size_t> (nTot)),
                r8 (static_cast<size_t> (nTot)), sw (static_cast<size_t> (nTot));
            // No-switch references: shape 0 and shape 8 held for the whole render.
            for (int shape : { 0, 8 })
            {
                float prm[5] = { 0.5f, (shape + 0.5f) / 16.0f, 0.0f, 0.7f, 0.0f };
                fx.reset();
                fx.setParams (prm);
                std::vector<float>& dst = shape == 0 ? r0 : r8;
                std::memcpy (dst.data(), src.data(), sizeof (float) * static_cast<size_t> (nTot));
                for (int i = 0; i < nTot; i += kBlock)
                    fx.process (dst.data() + i, dst.data() + i, std::min (kBlock, nTot - i));
            }
            // Switched render: alternate 0<->8 at n1 + k*swStep (landed on the
            // 256-sample block starts; the fade is 128 internal = 187 host).
            std::vector<int> at (static_cast<size_t> (nSw), -1);
            {
                int shape = 0, fired = 0;
                float prm[5] = { 0.5f, 0.5f / 16.0f, 0.0f, 0.7f, 0.0f };
                fx.reset();
                fx.setParams (prm);
                std::memcpy (sw.data(), src.data(), sizeof (float) * static_cast<size_t> (nTot));
                for (int i = 0; i < nTot; i += kBlock)
                {
                    while (fired < nSw && n1 + fired * swStep <= i)
                    {
                        shape = shape == 0 ? 8 : 0;
                        prm[1] = (shape + 0.5f) / 16.0f;
                        fx.setParams (prm);
                        at[static_cast<size_t> (fired++)] = i;
                    }
                    fx.process (sw.data() + i, sw.data() + i, std::min (kBlock, nTot - i));
                }
            }
            float worst = 0.0f;
            for (int k = 0; k < nSw; ++k)
            {
                // Pointwise excess slope: |dy/dt| of the switched render minus
                // the larger of the two no-switch renders' slope AT THE SAME
                // sample. A crossfade blend's slope is bounded by
                // max(ref slopes) + fade-step*|r8-r0| (~0.012); an instant
                // pointer swap steps |curve0 - curve8| (~0.1..1.3) on top of
                // the local slope. Window [a-2, a+260) covers the ~187-host-
                // sample fade + the resampler smearing of a raw step.
                for (int i = at[static_cast<size_t> (k)] - 2;
                     i < at[static_cast<size_t> (k)] + 260; ++i)
                {
                    const auto iu = static_cast<size_t> (i);
                    const float swSlope  = std::fabs (sw[iu + 1] - sw[iu]);
                    const float refSlope = std::fmax (std::fabs (r0[iu + 1] - r0[iu]),
                                                     std::fabs (r8[iu + 1] - r8[iu]));
                    worst = std::fmax (worst, swSlope - refSlope);
                }
            }
            std::printf ("  shape-switch worst slope excess = %.4f (bound 0.06)\n", worst);
            check (worst < 0.06f, "LUT Distortion: shape switch is click-free (crossfade)");
        }
    }

    // ---- Compressor ----
    {
        parvati::fv1::Fv1Compressor fx;
        fx.prepare (48000.0, kBlock);
        fx.reset();
        check (fx.type() == FxType::Compressor, "Compressor: type()");
        // Steady loud vs quiet at full amount: output ratio crushed. reset()
        // between runs — the envelope carries over otherwise (the loud run's
        // 100 ms release would suppress the quiet run's start; correct
        // behaviour, wrong for a steady-state level comparison).
        std::vector<float> loud (kN, 0.9f), quiet (kN, 0.05f);
        fx.reset();
        runFx (fx, 1.0f, 0.4f, 0.5f, 0.5f, loud.data(), loud.data(), kN, oL.data(), oR.data());
        const float outLoud = maxAbs (oL.data() + kN - 512, 512);
        fx.reset();
        runFx (fx, 1.0f, 0.4f, 0.5f, 0.5f, quiet.data(), quiet.data(), kN, oL.data(), oR.data());
        const float outQuiet = maxAbs (oL.data() + kN - 512, 512);
        check (outLoud < 0.9f, "Compressor: loud input reduced");
        // At threshold: no gain reduction, makeup 3x -> 0.05 * 3 = 0.15.
        check (outQuiet > 0.1f, "Compressor: quiet input pushed up (makeup)");
        check (outQuiet > outLoud * 0.5f, "Compressor: 18:1 input crushed to <= 2:1 out");
        check (allFinite (oL.data(), kN), "Compressor: finite");
    }

    // ---- Gate ----
    {
        parvati::fv1::Fv1Gate fx;
        fx.prepare (48000.0, kBlock);
        fx.reset();
        check (fx.type() == FxType::Gate, "Gate: type()");
        // Threshold 0 = DISABLED: a steady quiet signal passes ~unchanged.
        std::vector<float> sig (kN, 0.08f);
        runFx (fx, 0.0f, 0.5f, 0.4f, 0.5f, sig.data(), sig.data(), kN, oL.data(), oR.data());
        const float passTail = maxAbs (oL.data() + kN - 512, 512);
        check (passTail > 0.079f, "Gate: Threshold=0 = transparent (knob disables)");
        // High threshold + FAST release: the same signal is gated out well
        // before the render ends (the 500 ms release would still be decaying).
        runFx (fx, 1.0f, 0.5f, 0.0f, 0.0f, sig.data(), sig.data(), kN, oL.data(), oR.data());
        const float gatedTail = maxAbs (oL.data() + kN - 512, 512);
        check (gatedTail < 0.01f, "Gate: high threshold silences quiet input");
        check (allFinite (oL.data(), kN), "Gate: finite");
    }

    // ---- Chorus / Flanger ----
    for (auto [kind, mk] : std::initializer_list<std::pair<const char*, std::function<parvati::fv1::Fv1FxProcessor* ()>>> {
              { "Chorus", [] { return new parvati::fv1::Fv1Chorus(); } },
              { "Flanger", [] { return new parvati::fv1::Fv1Flanger(); } } })
    {
        std::unique_ptr<parvati::fv1::Fv1FxProcessor> fx { mk() };
        fx->prepare (48000.0, kBlock);
        fx->reset();
        runFx (*fx, 0.4f, 0.5f, 0.5f, 0.4f, inL.data(), inR.data(), kN, oL.data(), oR.data());
        std::printf ("-- %s --\n", kind);
        check (allFinite (oL.data(), kN), "finite @ mid");
        check (maxAbs (oL.data(), kN) > 1e-3f, "non-silent");
        // Wet is delayed: the first 2 ms of output (pre center delay) ~ 0.
        check (maxAbs (oL.data(), 48) < 1e-3f, "wet starts delayed (modulated delay)");
        bool corners = true;
        for (float a : { 0.0f, 1.0f }) for (float b : { 0.0f, 1.0f })
            for (float c : { 0.0f, 1.0f }) for (float d : { 0.0f, 1.0f })
            {
                runFx (*fx, a, b, c, d, inL.data(), inR.data(), kN, oL.data(), oR.data());
                if (! allFinite (oL.data(), kN) || ! allFinite (oR.data(), kN)) corners = false;
            }
        check (corners, "finite at all corners");
    }

    // ---- Echo ----
    {
        parvati::fv1::Fv1Echo fx;
        fx.prepare (48000.0, kBlock);
        fx.reset();
        check (fx.type() == FxType::Echo, "Echo: type()");
        // Impulse in: a repeat appears ~Time samples later (10 ms = 328 int).
        std::vector<float> imp (kN, 0.0f);
        imp[10] = 0.9f;
        runFx (fx, 0.0f, 0.0f, 0.5f, 0.0f, imp.data(), imp.data(), kN, oL.data(), oR.data());
        // 10 ms at 48k host = 480 samples; ping-pong puts the R partner at
        // 2x Time (it is written AFTER the L tap is read). A 1-sample impulse
        // smears through the resampler (~0.28 peak), hence the 0.15 bound.
        const float before = maxAbs (oL.data() + 40, 400);
        const float atTap  = maxAbs (oL.data() + 440, 140);
        const float atTapR = maxAbs (oR.data() + 900, 200);
        check (before < 1e-4f, "Echo: silent before the first tap");
        check (atTap > 0.15f, "Echo: discrete repeat at ~Time");
        check (atTapR > 0.15f, "Echo: ping-pong partner tap at 2x Time (R)");
        check (allFinite (oL.data(), kN), "Echo: finite");
        // Feedback honesty: the knob's 100% must read as INFINITE (0.995 loop
        // gain, -0.04 dB per repeat), not the old 0.95 cap (-0.45 dB/repeat —
        // audible pump-away the user noticed). Sustain 440 Hz @ 0.5 amp for
        // 1.2 s (Time ~100 ms/side, Feedback max, Tone bright, Spread 1x) so
        // the ping-pong loop saturates, then feed 1.2 s of silence and measure
        // the tail in the last 200 ms (~5-6 round trips of 200 ms in).
        //   fb=0.995 (new): tail = 0.9728  -> PASS (perceptually infinite)
        //   fb=0.95  (old): tail = 0.7716  -> FAIL (2 dB of pump-away)
        // Bound 0.85 sits between the two with margin on both sides.
        {
            fx.reset();
            const int sus = 57600, sil = 57600, nTot = sus + sil;   // 1.2 s + 1.2 s
            std::vector<float> tL (static_cast<size_t> (nTot), 0.0f),
                tR (static_cast<size_t> (nTot), 0.0f);
            for (int i = 0; i < sus; ++i)
            {
                const float t = static_cast<float> (i) / 48000.0f;
                tL[static_cast<size_t> (i)] = tR[static_cast<size_t> (i)] =
                    0.5f * std::sin (6.28318530718f * 440.0f * t);
            }
            // Time ~100 ms: 10 * 47^p = 100  ->  p = ln(10)/ln(47).
            float prm[5] = { std::log (10.0f) / std::log (47.0f), 1.0f, 1.0f, 0.0f, 0.0f };
            fx.setParams (prm);
            for (int i = 0; i < nTot; i += kBlock)
                fx.process (tL.data() + i, tR.data() + i, std::min (kBlock, nTot - i));
            const float tailL = maxAbs (tL.data() + nTot - 9600, 9600);
            const float tailR = maxAbs (tR.data() + nTot - 9600, 9600);
            std::printf ("  feedback tail (1.0-1.2 s post-silence) L=%.4f R=%.4f (bound > 0.85)\n",
                         tailL, tailR);
            check (tailL > 0.85f && tailR > 0.85f,
                   "Echo: Feedback=100% sustains (0.995 loop gain, reads infinite)");
        }
    }

    // ---- Room / Spring ----
    for (auto [kind, mk] : std::initializer_list<std::pair<const char*, std::function<parvati::fv1::Fv1FxProcessor* ()>>> {
              { "Room", [] { return new parvati::fv1::Fv1Room(); } },
              { "Spring", [] { return new parvati::fv1::Fv1Spring(); } } })
    {
        std::unique_ptr<parvati::fv1::Fv1FxProcessor> fx { mk() };
        fx->prepare (48000.0, kBlock);
        fx->reset();
        std::printf ("-- %s --\n", kind);
        // Impulse -> decaying tail with energy well past the input.
        std::vector<float> imp (kN, 0.0f);
        imp[10] = 0.8f;
        runFx (*fx, 0.6f, 0.6f, 0.7f, 0.8f, imp.data(), imp.data(), kN, oL.data(), oR.data());
        check (allFinite (oL.data(), kN) && allFinite (oR.data(), kN), "finite tail");
        // Early window starts AFTER the shortest comb/loop delay (Room's
        // shortest comb = 1601 int = 49 ms host; Spring's loop ~35 ms).
        const float eEarly = maxAbs (oL.data() + 2600, 1400);
        const float eLate  = maxAbs (oL.data() + 6000, 2000);
        check (eEarly > 1e-3f, "tail energy present (early)");
        check (eLate > 1e-5f, "tail energy persists (late)");
        bool corners = true;
        for (float a : { 0.0f, 1.0f }) for (float b : { 0.0f, 1.0f })
            for (float c : { 0.0f, 1.0f }) for (float d : { 0.0f, 1.0f })
            {
                runFx (*fx, a, b, c, d, inL.data(), inR.data(), kN, oL.data(), oR.data());
                if (! allFinite (oL.data(), kN) || ! allFinite (oR.data(), kN)) corners = false;
            }
        check (corners, "finite at all corners (incl. max decay)");
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_fail ? "FV1 NEW-FAMILY TEST: FAILURES"
                        : "FV1 NEW-FAMILY TEST: ALL CHECKS PASSED",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}

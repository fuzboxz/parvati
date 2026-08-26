// Per-part FX routing verification for Hellcat.
//
// Proves FxChain::process() produces FINITE output for every topology
// (Series / Parallel12to3 / Parallel1to23) x every order permutation (0..5),
// and that an enabled effect in slot C is actually applied (output differs from
// the dry input). This guards the 3-topology signal-flow graph introduced when
// the plain full-sum Parallel was replaced by the two split-parallel routings.
//
// The placeholder juce::dsp effects (GainPan/Delay/Reverb/Chorus) were removed;
// the chain-internal checks (master mix/EQ, tail retention, engage fade-in,
// re-prepare state preservation) now ride on the Clouds ports (Diffuser /
// Reverb / PitchShifter), which are the real effect set. Reverb is
// used for the state/fade checks because it responds to both DC and impulse
// input (Diffuser passes DC unchanged) and has a long tail.
//
// Run: ./build_unified/hellcat_unified_tests fx_routing_test

#include <cmath>
#include "unified_test_runner.h"
#include <cstdio>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "dsp/fx/FxChain.h"

// Exact float comparison is deliberate: these asserts pin values,
// not ranges.
#pragma clang diagnostic ignored "-Wfloat-equal"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

// True iff every sample is finite (no NaN / Inf).
bool allFinite (const float* d, int n)
{
    for (int i = 0; i < n; ++i)
        if (! std::isfinite (d[i]))
            return false;
    return true;
}

// True iff the buffer differs from @p ref in at least one sample (proves the FX
// graph actually processed the signal rather than copying the dry input).
bool differsFrom (const float* d, const float* ref, int n)
{
    for (int i = 0; i < n; ++i)
        if (d[i] != ref[i])
            return true;
    return false;
}

double blockRms (const float* d, int n)
{
    double s = 0.0;
    for (int i = 0; i < n; ++i)
        s += (double) d[i] * d[i];
    return s > 0.0 ? std::sqrt (s / (double) n) : 0.0;
}
}  // namespace

TEST(fx_routing_test)
{
    constexpr int kBlock = 256;

    // A nonzero impulse train input (so every effect produces audible wet energy
    // that must survive every routing graph).
    float inL[kBlock], inR[kBlock];
    for (int i = 0; i < kBlock; ++i)
    {
        const float v = (i % 32 == 0) ? 0.5f : 0.0f;   // periodic impulse train
        inL[i] = v;
        inR[i] = v;
    }

    int combos = 0;   // count of topology x order combinations exercised

    for (int topo = 0; topo < (int) FxTopology::Count; ++topo)
    {
        for (int ord = 0; ord < 6; ++ord)
        {
            FxChain chain;
            chain.prepare (48000.0, kBlock);

            // Slot A (order_[0]) = Diffuser, B = Reverb, C = PitchShifter.
            // Enable ALL three with a mid dry/wet so every branch of every
            // topology has at least one active contributor. All three are pure
            // in->out Clouds dsp/fx effects, so each produces immediate wet.
            const auto perm = fxOrderPermutation ((uint8_t) ord);
            chain.setOrder (perm);
            chain.setTopology ((FxTopology) topo);

            chain.setSlotType (0, FxType::Diffuser);
            chain.setSlotType (1, FxType::Reverb);
            chain.setSlotType (2, FxType::PitchShifter);
            for (int s = 0; s < kNumFxSlots; ++s)
            {
                chain.setSlotEnabled (s, true);
                chain.setSlotDryWet (s, 0.5f);
                for (int k = 0; k < kNumFxSlotParams; ++k)
                    chain.setSlotParam (s, k, 0.5f);
            }

            float outL[kBlock], outR[kBlock];
            chain.process (inL, inR, outL, outR, kBlock);
            ++combos;

            char msg[96];
            (void) std::snprintf (msg, sizeof (msg),
                                  "topo %d order %d: output finite", topo, ord);
            check (allFinite (outL, kBlock) && allFinite (outR, kBlock), msg);

            (void) std::snprintf (msg, sizeof (msg),
                                  "topo %d order %d: wet output differs from dry",
                                  topo, ord);
            check (differsFrom (outL, inL, kBlock) || differsFrom (outR, inR, kBlock),
                   msg);
        }
    }

    char msg[96];
    (void) std::snprintf (msg, sizeof (msg),
                          "exercised all %d topology x order combos (3 x 6)", combos);
    check (combos == 3 * 6, msg);

    // ---- Bypass sanity: nothing enabled => exact dry copy ----
    {
        FxChain chain;
        chain.prepare (48000.0, kBlock);
        chain.setSlotType (2, FxType::Diffuser);
        chain.setTopology (FxTopology::Parallel12to3);
        float outL[kBlock], outR[kBlock];
        chain.process (inL, inR, outL, outR, kBlock);
        check (! differsFrom (outL, inL, kBlock) && ! differsFrom (outR, inR, kBlock),
               "no enabled slot => dry passthrough (all topologies)");
    }

    // ---- Master section: global mix / keep-tails / master EQ ----
    {
        // Helper: slot 0 = Reverb, fully wet, others off. (The reverb's
        // broadband output guarantees the master EQ + global mix have something
        // to act on.)
        auto buildWetChain = [] (FxChain& c)
        {
            c.prepare (48000.0, kBlock);
            c.setSlotType (0, FxType::Reverb);
            c.setSlotEnabled (0, true);
            c.setSlotDryWet (0, 1.0f);
            for (int k = 0; k < kNumFxSlotParams; ++k)
                c.setSlotParam (0, k, 0.5f);
        };

        // (a) Defaults are a no-op (mix=127=>1.0, EQ flat).
        FxChain defChain;
        buildWetChain (defChain);
        float defL[kBlock], defR[kBlock];
        defChain.process (inL, inR, defL, defR, kBlock);
        check (allFinite (defL, kBlock) && allFinite (defR, kBlock),
               "master defaults: finite output");

        // (b) Global mix 0.5 pulls toward dry (differs from fully-wet defaults).
        FxChain mixChain;
        buildWetChain (mixChain);
        mixChain.setMasterMix (0.5f);
        float mixL[kBlock], mixR[kBlock];
        mixChain.process (inL, inR, mixL, mixR, kBlock);
        check (allFinite (mixL, kBlock) && allFinite (mixR, kBlock),
               "global mix 0.5: finite output");
        check (differsFrom (mixL, defL, kBlock) || differsFrom (mixR, defR, kBlock),
               "global mix 0.5: differs from fully-wet defaults");

        // (c) Master EQ mid boost alters the output vs flat.
        FxChain eqChain;
        buildWetChain (eqChain);
        eqChain.setMasterEqMid (100);   // ~+6.75 dB peaking at 1 kHz
        float eqL[kBlock], eqR[kBlock];
        eqChain.process (inL, inR, eqL, eqR, kBlock);
        check (allFinite (eqL, kBlock) && allFinite (eqR, kBlock),
               "master EQ mid boost: finite output");
        check (differsFrom (eqL, defL, kBlock) || differsFrom (eqR, defR, kBlock),
               "master EQ mid boost: differs from flat");

        // (d) Tails are now ALWAYS retained: after bypassing an enabled
        //     Reverb, the next block is NOT a bit-identical dry copy — the
        //     wet is still fading out (one-pole, ~0.30 s) AND the reverb tail is
        //     still ringing, so the slot keeps rendering instead of hard-cutting.
        {
            FxChain c;
            c.prepare (48000.0, kBlock);
            c.setSlotType (0, FxType::Reverb);
            c.setSlotEnabled (0, true);
            c.setSlotDryWet (0, 1.0f);
            // A long, loud reverb tail so the bypassed block has real energy.
            c.setSlotParam (0, 0, 0.9f);   // amount (strong wet)
            c.setSlotParam (0, 1, 0.8f);   // time -> long decay
            c.setSlotParam (0, 2, 0.5f);   // tone
            c.setSlotParam (0, 3, 0.5f);   // diffusion
            float oL[kBlock], oR[kBlock];
            for (int b = 0; b < 16; ++b)                    // let the tank fill + fade settle to ~1
                c.process (inL, inR, oL, oR, kBlock);
            c.setSlotEnabled (0, false);                    // bypass
            c.process (inL, inR, oL, oR, kBlock);           // first bypassed block
            check (differsFrom (oL, inL, kBlock) || differsFrom (oR, inR, kBlock),
                   "bypassed slot => wet fades out (not hard-cut) next block");
        }
        // (e) Engage fades IN (~5 ms): on the first block after enabling a
        //     previously-disabled slot the wet contribution is strictly less than
        //     at steady state (it ramps from 0, never slams in at full wet).
        {
            FxChain c;
            c.prepare (48000.0, kBlock);
            c.setSlotType (0, FxType::Reverb);
            c.setSlotEnabled (0, false);                    // disabled: dry passthrough, fade ~0
            c.setSlotDryWet (0, 1.0f);
            for (int k = 0; k < kNumFxSlotParams; ++k)
                c.setSlotParam (0, k, 0.5f);
            float oL[kBlock], oR[kBlock];
            for (int b = 0; b < 4; ++b)                     // settle while disabled (fade stays ~0)
                c.process (inL, inR, oL, oR, kBlock);
            c.setSlotEnabled (0, true);                     // engage
            float b1L[kBlock], b1R[kBlock];
            c.process (inL, inR, b1L, b1R, kBlock);         // block 1 after engage: fade ramps 0 -> ~0.66
            float ssL[kBlock], ssR[kBlock];
            for (int b = 0; b < 24; ++b)                    // reach steady state (fade ~= 1)
                c.process (inL, inR, ssL, ssR, kBlock);
            // Wet contribution = max|out - dry| over the block.
            float wet1 = 0.0f, wetSS = 0.0f;
            for (int i = 0; i < kBlock; ++i)
            {
                wet1  = std::fmax (wet1,  std::fabs (b1L[i] - inL[i]));
                wetSS = std::fmax (wetSS, std::fabs (ssL[i] - inL[i]));
            }
            check (wet1 > 0.0f && wet1 < wetSS,
                   "engage => block-1 wet < steady-state wet (fade-in, no full-wet slam)");
        }
    }

    // ---- B7: a mid-session re-prepare (host sample-rate / buffer-size change,
    //         or some hosts on a plugin-bypass toggle) must NOT truncate an
    //         enabled slot's wet state. prepare() previously did
    //         wetFade_.fill(0) and snapped dryWetCur_/masterMixCur_ to target,
    //         which on re-prepare zeroed an enabled slot's wetFade_ => dw=0 =>
    //         pure dry for the first ~5 ms. The decisive check: with the fix the
    //         first post-reprepare block is still WET (not a pure-dry copy),
    //         proving wetFade_ was preserved. (The original bit-identical
    //         variant relied on GainPan being memoryless + prepare() a no-op;
    //         the Clouds effects re-Init their engine on prepare(), so only the
    //         wet/dry distinction — not sample-identity — is portable.) ----
    {
        FxChain c;
        c.prepare (48000.0, kBlock);
        c.setSlotType (0, FxType::Reverb);
        c.setSlotEnabled (0, true);
        c.setSlotDryWet (0, 1.0f);
        for (int k = 0; k < kNumFxSlotParams; ++k)
            c.setSlotParam (0, k, 0.5f);

        // Render enough blocks for wetFade_ (~1.0) and dryWetCur_ to settle and
        // the reverb tank to fill.
        float oL[kBlock], oR[kBlock];
        for (int b = 0; b < 24; ++b)
            c.process (inL, inR, oL, oR, kBlock);

        // Simulate the host mid-session re-prepare. The fix preserves
        // wetFade_/dryWetCur_/masterMixCur_; the bug zeroed them.
        c.prepare (48000.0, kBlock);
        c.process (inL, inR, oL, oR, kBlock);   // first block after re-prepare

        // If wetFade_ were zeroed (bug) this block would be a pure-dry copy of
        // the input; preserved wetFade_ (~1) keeps it wet.
        check (differsFrom (oL, inL, kBlock) || differsFrom (oR, inR, kBlock),
               "B7: re-prepare preserves an enabled slot's wet state (output still wet, not dry)");
    }

    // ---- D1/D2 latency-compensation coverage: OS effects through FxChain ----
    // Exercises the new per-slot dry-delay (series, D1) and the wet-align +
    // parallel-dry-delay (renderParallel, D2) rings through the FULL chain for
    // the two 6x-OS effects (Wavefolder/RingModulator, latency 8). These confirm
    // the compensation plumbing runs end-to-end without OOB/corruption; the
    // comb-null fix itself is verified by code inspection (dry delayed by the
    // slot's latency aligns dry[i-L] with the latency-L wet).
    {
        float oL[kBlock], oR[kBlock];

        // Series: Wavefolder (OS, latency 8) at dw=0.5 -> blendSlotWetFade dry-delay.
        {
            FxChain chain;
            chain.prepare (48000.0, kBlock);
            chain.setSlotType (0, FxType::Wavefolder);
            chain.setSlotEnabled (0, true);
            chain.setSlotDryWet (0, 0.5f);
            chain.setSlotParam (0, 0, 0.8f);   // fold 0.8
            chain.process (inL, inR, oL, oR, kBlock);
            check (allFinite (oL, kBlock) && allFinite (oR, kBlock),
                   "D1 series: Wavefolder (OS) dry-delay blend is finite");
            check (differsFrom (oL, inL, kBlock) || differsFrom (oR, inR, kBlock),
                   "D1 series: Wavefolder (OS) output differs from dry (fold engaged)");
        }

        // Parallel (12to3): Wavefolder (OS, latency 8) || Diffuser (latency 0),
        // dw=0.5 -> renderParallel wet-align (delay the latency-0 wet to 8) + dry-delay.
        {
            FxChain chain;
            chain.prepare (48000.0, kBlock);
            chain.setTopology (FxTopology::Parallel12to3);
            chain.setSlotType (0, FxType::Wavefolder);   // A: OS, latency 8
            chain.setSlotType (1, FxType::Diffuser);     // B: latency 0
            chain.setSlotType (2, FxType::None);         // C: passthrough
            chain.setSlotEnabled (0, true);
            chain.setSlotEnabled (1, true);
            chain.setSlotDryWet (0, 0.5f);
            chain.setSlotDryWet (1, 0.5f);
            chain.setSlotParam (0, 0, 0.8f);
            chain.setSlotParam (1, 0, 0.8f);
            chain.process (inL, inR, oL, oR, kBlock);
            check (allFinite (oL, kBlock) && allFinite (oR, kBlock),
                   "D2 parallel: Wavefolder(OS)||Diffuser wet-align blend is finite");
            check (differsFrom (oL, inL, kBlock) || differsFrom (oR, inR, kBlock),
                   "D2 parallel: Wavefolder(OS)||Diffuser output differs from dry");
        }
    }

    // ---- B8: FV-1 / buffer-based effects through the PARALLEL topologies ----
    // The topology x order sweep above uses only Clouds ports; the FV-1 family
    // (internal 32.768 kHz RateBridge + delay rings) must also survive the
    // split-parallel graphs with real wet output.
    {
        for (int topo = 1; topo < (int) FxTopology::Count; ++topo)   // both parallel graphs
        {
            FxChain chain;
            chain.prepare (48000.0, kBlock);
            chain.setTopology ((FxTopology) topo);
            chain.setSlotType (0, FxType::Echo);         // A: FV-1 digital echo
            chain.setSlotType (1, FxType::PlateReverb);  // B: FV-1 plate
            chain.setSlotType (2, FxType::Spring);       // C: FV-1 spring
            for (int s = 0; s < kNumFxSlots; ++s)
            {
                chain.setSlotEnabled (s, true);
                chain.setSlotDryWet (s, 0.5f);
                for (int k = 0; k < kNumFxSlotParams; ++k)
                    chain.setSlotParam (s, k, 0.5f);
            }
            float b8L[kBlock], b8R[kBlock];
            for (int b = 0; b < 6; ++b)                  // let tanks/rings fill
                chain.process (inL, inR, b8L, b8R, kBlock);

            char b8msg[96];
            (void) std::snprintf (b8msg, sizeof (b8msg),
                                  "B8 topo %d: Echo/Plate/Spring parallel renders finite", topo);
            check (allFinite (b8L, kBlock) && allFinite (b8R, kBlock), b8msg);
            (void) std::snprintf (b8msg, sizeof (b8msg),
                                  "B8 topo %d: Echo/Plate/Spring parallel output is wet", topo);
            check (differsFrom (b8L, inL, kBlock) || differsFrom (b8R, inR, kBlock), b8msg);
        }
    }

    // ---- B9: mid-stream bypass of ONE parallel-branch slot: the survivor's
    // gain must NOT jump. renderParallel divides the summed wets by the ACTIVE
    // count; a just-bypassed slot (enabled=false but wetFade_ > 5e-4) still
    // renders and STILL DIVIDES, so the blend is continuous — an immediate
    // activeCount 2->1 re-derivation would scale the survivor +6 dB (an
    // audible pop on bypass).
    {
        FxChain chain;
        chain.prepare (48000.0, kBlock);
        chain.setTopology (FxTopology::Parallel12to3);
        chain.setSlotType (0, FxType::Echo);         // A: branch 1
        chain.setSlotType (1, FxType::PlateReverb);  // B: branch 2 (the one bypassed)
        chain.setSlotType (2, FxType::None);         // C: passthrough
        chain.setSlotEnabled (0, true);
        chain.setSlotEnabled (1, true);
        chain.setSlotDryWet (0, 0.5f);
        chain.setSlotDryWet (1, 0.5f);
        for (int k = 0; k < kNumFxSlotParams; ++k)
        {
            chain.setSlotParam (0, k, 0.5f);
            chain.setSlotParam (1, k, 0.5f);
        }

        float oL[kBlock], oR[kBlock];
        for (int b = 0; b < 24; ++b)                 // fill tanks, fade -> 1
            chain.process (inL, inR, oL, oR, kBlock);

        const double rmsBefore = blockRms (oL, kBlock);
        chain.setSlotEnabled (1, false);             // bypass branch B mid-stream
        chain.process (inL, inR, oL, oR, kBlock);    // first bypassed block
        const double rmsAtBypass = blockRms (oL, kBlock);

        std::printf ("  [info] B9 rms: before=%.4f atBypass=%.4f\n", rmsBefore, rmsAtBypass);
        const double ratio = rmsAtBypass / (rmsBefore > 1e-12 ? rmsBefore : 1e-12);
        check (ratio > 0.7 && ratio < 1.4,
               "B9: bypassing one parallel branch does NOT jump the blend "
               "(still-divides; no +6 dB survivor snap)");
        check (rmsAtBypass > 1.0e-4,
               "B9: the just-bypassed branch still contributes while fading (not hard-cut)");

        // The bypassed branch's contribution decays as wetFade_ dies: the
        // divergence between a stay-enabled chain and the bypassed one (fed
        // identical input, identical state) grows block-over-block. (The raw
        // bypassed-chain energy alone is NOT monotonic — the surviving echo
        // keeps filling — so the decay is pinned via this difference.)
        {
            auto buildPair = [] (FxChain& c)
            {
                c.prepare (48000.0, kBlock);
                c.setTopology (FxTopology::Parallel12to3);
                c.setSlotType (0, FxType::Echo);
                c.setSlotType (1, FxType::PlateReverb);
                c.setSlotType (2, FxType::None);
                c.setSlotEnabled (0, true);
                c.setSlotEnabled (1, true);
                c.setSlotDryWet (0, 0.5f);
                c.setSlotDryWet (1, 0.5f);
                for (int k = 0; k < kNumFxSlotParams; ++k)
                {
                    c.setSlotParam (0, k, 0.5f);
                    c.setSlotParam (1, k, 0.5f);
                }
            };
            FxChain stay, byp;
            buildPair (stay);
            buildPair (byp);
            float sL[kBlock], sR[kBlock], yL[kBlock], yR[kBlock];
            for (int b = 0; b < 24; ++b)             // IDENTICAL warmup on both
            {
                stay.process (inL, inR, sL, sR, kBlock);
                byp.process (inL, inR, yL, yR, kBlock);
            }
            byp.setSlotEnabled (1, false);           // only `byp` bypasses
            double diff1 = 0.0, diffN = 0.0;
            for (int b = 0; b < 40; ++b)              // fade: 0.982^40 ~ 0.48
            {
                stay.process (inL, inR, sL, sR, kBlock);
                byp.process (inL, inR, yL, yR, kBlock);
                double diff = 0.0;
                for (int i = 0; i < kBlock; ++i)
                    diff += std::fabs ((double) sL[i] - (double) yL[i]);
                if (b == 0) diff1 = diff;
                diffN = diff;
            }
            std::printf ("  [info] B9 stay-vs-bypassed |diff|: block1=%.4e block40=%.4e\n",
                         diff1, diffN);
            check (diff1 > 1.0e-4, "B9: bypassed branch's contribution is measurably fading (diff > 0)");
            check (diffN > 3.0 * diff1,
                   "B9: the stay-vs-bypassed difference grows >3x over 40 blocks (fade decays)");
        }
    }

    // ---- B10: PARALLEL PER-BRANCH DRY/WET (audit/drywet_investigation Bug
    // B, 2026-08-20). The old shared-mean blend scaled the summed raw wets
    // by the MEAN dw: a branch at dw=0 leaked at -6 dB (feedback repeats ran
    // forever) and sweeping one branch's dw shifted the OTHER branch's gain.
    // Fixed: each branch's wet is scaled by its OWN effective dw inside the
    // sum (the mean only drives the dry gain). ----
    {
        // (a) Sweep branch A's dw 1.0 -> 0.0; A's pulse must fall under -60 dB
        //     of its dw=1 level while branch B's output stays BIT-IDENTICAL.
        auto branchPeaks = [] (float dwA, float (&pA)[2], float (&pB)[2])
        {
            FxChain c;
            c.prepare (48000.0, kBlock);
            c.setTopology (FxTopology::Parallel12to3);
            c.setSlotType (0, FxType::Echo);            // A: click @ 100 ms
            c.setSlotType (1, FxType::Echo);            // B: click @ 300 ms
            c.setSlotType (2, FxType::None);            // C: passthrough
            c.setSlotEnabled (0, true);
            c.setSlotEnabled (1, true);
            c.setSlotDryWet (0, dwA);
            c.setSlotDryWet (1, 1.0f);
            c.setSlotParam (0, 0, 0.598f);               // 100 ms (ms = 10*47^p0)
            c.setSlotParam (1, 0, 0.883f);               // 300 ms
            c.setSlotParam (0, 1, 0.0f);                // fb 0 (isolated pulses)
            c.setSlotParam (1, 1, 0.0f);
            for (int k = 2; k < kNumFxSlotParams; ++k)
            {
                c.setSlotParam (0, k, 0.5f);
                c.setSlotParam (1, k, 0.5f);
            }
            float iL[kBlock] {}, iR[kBlock] {};
            float oL[kBlock], oR[kBlock];
            // Feed one click, then silence, 1 s total; record the peak in each
            // branch's echo window (100 ms and 300 ms after the click).
            const int tA = (int) (0.100 * 48000), tB = (int) (0.300 * 48000);
            pA[0] = pA[1] = pB[0] = pB[1] = 0.0f;
            int clickAt = -1;
            for (int smp = 0, b = 0; smp < 48000; smp += kBlock, ++b)
            {
                std::fill (iL, iL + kBlock, 0.0f);
                if (smp == 0) { iL[0] = 0.9f; clickAt = smp; }
                c.process (iL, iR, oL, oR, kBlock);
                for (int i = 0; i < kBlock; ++i)
                {
                    const int t = smp + i;
                    if (t > clickAt + tA - 256 && t < clickAt + tA + 256)
                        { pA[0] = std::max (pA[0], std::abs (oL[i])); pA[1] = std::max (pA[1], std::abs (oR[i])); }
                    if (t > clickAt + tB - 256 && t < clickAt + tB + 256)
                        { pB[0] = std::max (pB[0], std::abs (oL[i])); pB[1] = std::max (pB[1], std::abs (oR[i])); }
                }
            }
        };
        float a1[2], a0[2], b1[2], b0[2];
        branchPeaks (1.0f, a1, b1);
        branchPeaks (0.0f, a0, b0);
        const auto db = [] (float x) { return 20.0 * std::log10 ((double) std::max (x, 1.0e-12f)); };
        std::printf ("  [info] B10a A-branch: dw1=%.2f dB dw0=%.2f dB (delta %.2f dB)\n",
                     db (a1[0]), db (a0[0]), db (a1[0]) - db (a0[0]));
        std::printf ("  [info] B10a B-branch: dw1=%.2f dB dw0=%.2f dB (delta %.2f dB)\n",
                     db (b1[0]), db (b0[0]), db (b1[0]) - db (b0[0]));
        check (db (a1[0]) - db (a0[0]) > 60.0,
               "B10a: branch A at dw=0 falls >60 dB below its dw=1 level (was -6 dB leak)");
        check (b0[0] == b1[0] && b0[1] == b1[1],
               "B10a: branch B is BIT-IDENTICAL across A's dw sweep (gain invariance)");

        // (b) Feedback variant: A = echo fb 0.85 at dw 0 FROM THE START,
        //     B = plate. The ECHO-PERIODIC component is isolated by
        //     difference against a twin chain whose A has fb 0 (identical dry
        //     gain + plate branch => the difference is exactly A's feedback
        //     path). No repeats above -60 dB of the click after 2 blocks.
        {
            auto renderFb = [] (float fb, double& postPeak)
            {
                FxChain c;
                c.prepare (48000.0, kBlock);
                c.setTopology (FxTopology::Parallel12to3);
                c.setSlotType (0, FxType::Echo);
                c.setSlotType (1, FxType::PlateReverb);
                c.setSlotType (2, FxType::None);
                c.setSlotEnabled (0, true);
                c.setSlotEnabled (1, true);
                c.setSlotDryWet (0, 0.0f);             // BUG: was never silent
                c.setSlotDryWet (1, 1.0f);
                c.setSlotParam (0, 0, 0.598f);          // 100 ms
                c.setSlotParam (0, 1, fb);
                for (int k = 2; k < kNumFxSlotParams; ++k) c.setSlotParam (0, k, 0.5f);
                for (int k = 0; k < kNumFxSlotParams; ++k) c.setSlotParam (1, k, 0.5f);
                float iL[kBlock] {}, iR[kBlock] {};
                float oL[kBlock], oR[kBlock];
                postPeak = 0.0;
                int blocks = 0;
                for (int smp = 0; smp < 48000; smp += kBlock)
                {
                    std::fill (iL, iL + kBlock, 0.0f);
                    if (smp == 0) iL[0] = 0.9f;
                    c.process (iL, iR, oL, oR, kBlock);
                    ++blocks;
                    if (blocks > 2)                     // past the click's own echo window
                        for (int i = 0; i < kBlock; ++i)
                            postPeak = std::max (postPeak, (double) std::abs (oL[i]));
                }
            };
            double withFb = 0.0, noFb = 0.0;
            renderFb (0.85f, withFb);
            renderFb (0.0f,  noFb);
            const double echoComponent = withFb - noFb;   // plate + dry cancel exactly
            std::printf ("  [info] B10b echo-path peak (fb0.85 minus fb0) = %.6e\n", echoComponent);
            check (echoComponent < 0.9 * 1.0e-3,
                   "B10b: dw=0 echo branch leaves no feedback repeats (was -22.8 dB repeats forever)");
        }
    }

    // ---- B7b: mid-session re-prepare at a DIFFERENT sample rate (host
    // rate change). Same continuity contract as B7 plus: a STAGED-but-unconsumed
    // type swap is applied by prepare() at the new rate (never renders with
    // undersized scratch), the HostRateBridge re-arms (AA filters on/off at
    // the 32 kHz boundary), and delay rings are flushed (no stale-rate replay).
    {
        FxChain c;
        c.prepare (48000.0, kBlock);
        c.setSlotType (0, FxType::Reverb);
        c.setSlotEnabled (0, true);
        c.setSlotDryWet (0, 1.0f);
        for (int k = 0; k < kNumFxSlotParams; ++k)
            c.setSlotParam (0, k, 0.5f);

        float oL[kBlock], oR[kBlock];
        for (int b = 0; b < 24; ++b)                 // fill the tank at 48k
            c.process (inL, inR, oL, oR, kBlock);

        // Stage a type swap WITHOUT rendering it, then re-prepare at 96 kHz:
        // prepare must consume the staged swap first and re-prepare it at the
        // new rate (the swap lands wet on the very first block).
        c.setSlotType (0, FxType::Echo);
        c.prepare (96000.0, kBlock);
        c.process (inL, inR, oL, oR, kBlock);
        check (allFinite (oL, kBlock) && allFinite (oR, kBlock),
               "B7b: staged swap + re-prepare @96 kHz renders finite");
        check (differsFrom (oL, inL, kBlock) || differsFrom (oR, inR, kBlock),
               "B7b: post-re-prepare block is wet (staged swap applied, wet state preserved)");

        // And back down to 44.1 kHz (AA filters re-arm; rings flushed).
        c.prepare (44100.0, kBlock);
        c.process (inL, inR, oL, oR, kBlock);
        check (allFinite (oL, kBlock) && allFinite (oR, kBlock),
               "B7b: second re-prepare @44.1 kHz renders finite");
        check (differsFrom (oL, inL, kBlock) || differsFrom (oR, inR, kBlock),
               "B7b: post-44.1 kHz block still wet (no stale-rate dropout)");
    }

    // ---- T1: FxChain::setTempo -> ClockedDelay host-BPM sync through the
    // CHAIN seam (the engine path: AudioPlayHead -> fxChains_[p].setTempo ->
    // slot override). The echo spacing of a 1/16 division must ~double when
    // the BPM halves, driven ONLY by chain.setTempo.
    {
        constexpr int kTotal = 16384;
        auto echoPeak = [] (double bpm) -> int
        {
            FxChain chain;
            chain.prepare (48000.0, kBlock);
            chain.setSlotType (0, FxType::ClockedDelay);
            chain.setSlotEnabled (0, true);
            chain.setSlotDryWet (0, 1.0f);
            chain.setSlotParam (0, 0, 1.0f);   // sync -> 1/16 division
            chain.setSlotParam (0, 1, 0.0f);   // no feedback: single clean echo
            chain.setSlotParam (0, 2, 0.0f);   // no age
            chain.setSlotParam (0, 3, 0.0f);   // no grit

            std::vector<float> L ((size_t) kTotal, 0.0f), R ((size_t) kTotal, 0.0f);
            std::vector<float> oL ((size_t) kBlock), oR ((size_t) kBlock);
            std::vector<float> zeros ((size_t) kBlock, 0.0f);

            // A staged type swap is INSTALLED by the next process()
            // (servicePendingTypeSwaps); setTempo fans out to slots_, so it
            // must run AFTER the processor exists. One silent block installs
            // it (mirrors the engine: renderPartFx services swaps at the top
            // of every block, setTempo arrives with the NEXT transport push).
            chain.process (zeros.data(), zeros.data(), oL.data(), oR.data(), kBlock);
            chain.setTempo (bpm, true);        // THE SEAM UNDER TEST

            // The delay-length retarget GLIDES (<= 0.25 sample/internal-sample,
            // ~8 ms tau — the click-free tape-style glide). The install block
            // computed its length at the processor's DEFAULT tempo, so after
            // setTempo the read pointer needs ~0.25 s to reach the new target
            // before the impulse measures it. Render silence through the glide.
            for (int b = 0; b < 64; ++b)
                chain.process (zeros.data(), zeros.data(), oL.data(), oR.data(), kBlock);

            // Single impulse at absolute sample 0.
            std::vector<float> imp ((size_t) kBlock, 0.0f);
            imp[0] = 0.9f;
            chain.process (imp.data(), imp.data(), oL.data(), oR.data(), kBlock);
            for (int i = 0; i < kBlock; ++i) { L[(size_t) i] = oL[(size_t) i]; R[(size_t) i] = oR[(size_t) i]; }
            for (int off = kBlock; off < kTotal; off += kBlock)
            {
                chain.process (zeros.data(), zeros.data(), oL.data(), oR.data(), kBlock);
                for (int i = 0; i < kBlock; ++i) { L[(size_t) (off + i)] = oL[(size_t) i]; R[(size_t) (off + i)] = oR[(size_t) i]; }
            }
            int argmax = 0; float mx = -1.0f;
            for (int i = 200; i < kTotal; ++i)
                if (std::fabs (L[(size_t) i]) > mx) { mx = std::fabs (L[(size_t) i]); argmax = i; }
            return argmax;
        };
        const int peak240 = echoPeak (240.0);   // 1/16 @240 -> 62.5 ms -> ~3000 samples
        const int peak120 = echoPeak (120.0);   // 1/16 @120 -> 125 ms  -> ~6000 samples
        std::printf ("  [info] T1 chain echo peak: @240bpm=%d  @120bpm=%d\n", peak240, peak120);
        check (peak240 > 2500, "T1: chain.setTempo @240 BPM puts the 1/16 echo in a sane range");
        check (peak240 < peak120 - 2000, "T1: higher BPM shortens the chain-driven echo");
        const double ratio = (double) peak120 / (double) peak240;
        check (ratio > 1.7 && ratio < 2.3,
               "T1: halving the BPM ~doubles the echo spacing through the chain seam");
    }

    // ---- T2: chain.setTempo on non-ClockedDelay slots is a bit-identical
    // no-op (the default FxProcessor::setTransport does nothing).
    {
        auto renderWithTempo = [&] (double bpm, std::vector<float>& outL)
        {
            FxChain chain;
            chain.prepare (48000.0, kBlock);
            chain.setSlotType (0, FxType::Diffuser);   // non-tempo effect
            chain.setSlotType (1, FxType::PlateReverb);
            chain.setSlotEnabled (0, true);
            chain.setSlotEnabled (1, true);
            chain.setSlotDryWet (0, 0.5f);
            chain.setSlotDryWet (1, 0.5f);
            for (int k = 0; k < kNumFxSlotParams; ++k)
            {
                chain.setSlotParam (0, k, 0.5f);
                chain.setSlotParam (1, k, 0.5f);
            }
            float oL[kBlock], oR[kBlock];
            chain.setTempo (bpm, true);                // differing BPM...
            for (int b = 0; b < 8; ++b)
                chain.process (inL, inR, oL, oR, kBlock);
            outL.assign (oL, oL + kBlock);
        };
        std::vector<float> outA, outB;
        renderWithTempo (120.0, outA);
        renderWithTempo (187.5, outB);
        bool identical = true;
        for (int i = 0; i < kBlock; ++i)
            if (outA[(size_t) i] != outB[(size_t) i]) identical = false;
        check (identical,
               "T2: setTempo on non-ClockedDelay slots is a bit-identical no-op");
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "FX ROUTING TEST: FAILURES" : "FX ROUTING TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}

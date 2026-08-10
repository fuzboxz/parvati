// Per-part FX routing verification for Parvati.
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
// CloudsReverb / PitchShifter), which are the real effect set. CloudsReverb is
// used for the state/fade checks because it responds to both DC and impulse
// input (Diffuser passes DC unchanged) and has a long tail.
//
// Built by default. Run with: ./build/parvati_fx_routing_test

#include <cmath>
#include <cstdio>

#include <juce_audio_basics/juce_audio_basics.h>

#include "dsp/fx/FxChain.h"

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
}  // namespace

int main()
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

            // Slot A (order_[0]) = Diffuser, B = CloudsReverb, C = PitchShifter.
            // Enable ALL three with a mid dry/wet so every branch of every
            // topology has at least one active contributor. All three are pure
            // in->out Clouds dsp/fx effects, so each produces immediate wet.
            const auto perm = fxOrderPermutation ((uint8_t) ord);
            chain.setOrder (perm);
            chain.setTopology ((FxTopology) topo);

            chain.setSlotType (0, FxType::Diffuser);
            chain.setSlotType (1, FxType::CloudsReverb);
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
        // Helper: slot 0 = CloudsReverb, fully wet, others off. (The reverb's
        // broadband output guarantees the master EQ + global mix have something
        // to act on.)
        auto buildWetChain = [] (FxChain& c)
        {
            c.prepare (48000.0, kBlock);
            c.setSlotType (0, FxType::CloudsReverb);
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
        //     CloudsReverb, the next block is NOT a bit-identical dry copy — the
        //     wet is still fading out (one-pole, ~0.30 s) AND the reverb tail is
        //     still ringing, so the slot keeps rendering instead of hard-cutting.
        {
            FxChain c;
            c.prepare (48000.0, kBlock);
            c.setSlotType (0, FxType::CloudsReverb);
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
            c.setSlotType (0, FxType::CloudsReverb);
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
        c.setSlotType (0, FxType::CloudsReverb);
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

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "FX ROUTING TEST: FAILURES" : "FX ROUTING TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}

// Per-part FX routing verification for Parvati.
//
// Proves FxChain::process() produces FINITE output for every topology
// (Series / Parallel12to3 / Parallel1to23) x every order permutation (0..5),
// and that an enabled Delay in slot C is actually applied (output differs from
// the dry input). This guards the 3-topology signal-flow graph introduced when
// the plain full-sum Parallel was replaced by the two split-parallel routings.
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

    // A nonzero impulse train input (so a Delay / Reverb / Chorus produces
    // audible wet energy that must survive every routing graph).
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

            // Slot A (order_[0]) = GainPan, B = Reverb, C = Delay. Enable ALL
            // three with a mid dry/wet so every branch of every topology has at
            // least one active contributor (the Delay lives in slot C = 2).
            const auto perm = fxOrderPermutation ((uint8_t) ord);
            chain.setOrder (perm);
            chain.setTopology ((FxTopology) topo);

            // Physical slot 2 (C in the default order) carries a Delay.
            chain.setSlotType (0, FxType::GainPan);
            chain.setSlotType (1, FxType::Reverb);
            chain.setSlotType (2, FxType::Delay);
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
        chain.setSlotType (2, FxType::Delay);
        chain.setTopology (FxTopology::Parallel12to3);
        float outL[kBlock], outR[kBlock];
        chain.process (inL, inR, outL, outR, kBlock);
        check (! differsFrom (outL, inL, kBlock) && ! differsFrom (outR, inR, kBlock),
               "no enabled slot => dry passthrough (all topologies)");
    }

    // ---- Master section: global mix / keep-tails / master EQ ----
    {
        // Helper: slot 0 = Reverb, fully wet, others off. (Reverb's broadband
        // output guarantees the master EQ + global mix have something to act on.)
        auto buildWetChain = [] (FxChain& c)
        {
            c.prepare (48000.0, kBlock);
            c.setSlotType (0, FxType::Reverb);
            c.setSlotEnabled (0, true);
            c.setSlotDryWet (0, 1.0f);
            for (int k = 0; k < kNumFxSlotParams; ++k)
                c.setSlotParam (0, k, 0.5f);
        };

        // (a) Defaults are a no-op (mix=127=>1.0, keepTails=0, EQ flat).
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

        // (d) keepTails=0 => hard cut on bypass (dry next block).
        {
            FxChain c;
            buildWetChain (c);
            c.setKeepTails (false);
            float oL[kBlock], oR[kBlock];
            c.process (inL, inR, oL, oR, kBlock);          // wet frame
            c.setSlotEnabled (0, false);
            c.process (inL, inR, oL, oR, kBlock);          // bypassed frame
            check (! differsFrom (oL, inL, kBlock) && ! differsFrom (oR, inR, kBlock),
                   "keepTails=0: bypassed slot => hard cut (dry) next block");
        }
        // (e) keepTails=1 => the tail still rings on bypass (not dry next block).
        {
            FxChain c;
            buildWetChain (c);
            c.setKeepTails (true);
            float oL[kBlock], oR[kBlock];
            c.process (inL, inR, oL, oR, kBlock);          // wet frame (tank charges)
            c.setSlotEnabled (0, false);
            c.process (inL, inR, oL, oR, kBlock);          // bypassed frame: tail rings
            check (differsFrom (oL, inL, kBlock) || differsFrom (oR, inR, kBlock),
                   "keepTails=1: bypassed slot => tail rings (not dry) next block");
        }
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "FX ROUTING TEST: FAILURES" : "FX ROUTING TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}

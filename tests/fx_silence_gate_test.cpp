// fx_silence_gate_test — the FxChain SILENCE GATE (2026-08-23 idle-CPU fix).
//
// SynthEngine::renderPartFx runs EVERY part's chain every block regardless of
// voice activity, and once a slot is enabled the chain's only fast path was the
// "nothing enabled" bypass — the full topology + slot DSP ran forever on
// dead-silent input (the "CPU stays ~20% after playing" report). The gate arms
// after kGateSilentBlocks consecutive blocks that are silent at the INPUT and
// <=-120 dB at the OUTPUT (tail-length-agnostic by construction: a ringing
// reverb keeps the counter at 0 until its tail has actually decayed), then
// serves silent blocks from a zero-output early return. Any state change that
// could alter the output — a real input, an enable/dry-wet/param/master-mix/EQ/
// topology/order VALUE change, an installed type swap, a re-prepare — resets it
// (value-guarded: renderPartFx re-pushes identical values every ~980 Hz
// sub-chunk at rest, and that must not starve the gate).
//
// Sections:
//   [1] arming: exact K-block debounce on a fresh silent chain; gated serves
//       emit exact zeros; the gated path is actually TAKEN (counter).
//   [2] tail-awareness: a hot reverb tail blocks arming; arms only after the
//       tail decays (+K).
//   [3] resets: a real param/enable/master-mix change disarms; an IDENTICAL
//       value re-push does NOT (the engine's per-sub-chunk re-push pin); a
//       wake block disarms and passes audio.
//   [4] latency invariance: an armed Wavefolder chain (latency()==8) passes an
//       impulse with IDENTICAL timing and <=1e-6 deviation vs a control chain
//       whose gate is pinned OFF — the arm/wake cycle preserves N1/N2 latency
//       exactly (rings zeroed at arm; all-zero rings are phase-invariant).
//   [5] engine end-to-end: through HellcatAudioProcessor + processBlock with
//       FX enabled, pure idle arms part 0's chain; a note-on wakes it and
//       produces finite audible output; an fx1_param1 write on the armed idle
//       chain disarms it within a block or two (the smoothedBase_ sub-chunk
//       path delivers a REAL setSlotParam value change).
//
// Built by default. Run: ./build_unified/hellcat_unified_tests fx_silence_gate_test

#include <cmath>
#include <cstdio>
#include <vector>

#include "test_utils.h"          // setParam (host-style APVTS writes)
#include "unified_test_runner.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include "PluginProcessor.h"
#include "SynthEngine.h"
#include "dsp/fx/FxChain.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

bool allFinite (const float* d, int n)
{
    for (int i = 0; i < n; ++i)
        if (! std::isfinite (d[i]))
            return false;
    return true;
}

float maxAbs (const float* d, int n)
{
    float m = 0.0f;
    for (int i = 0; i < n; ++i)
        m = std::max (m, std::fabs (d[i]));
    return m;
}

// First index with |d[i]| > @p th, or -1 when the buffer stays under it.
int firstAbove (const float* d, int n, float th)
{
    for (int i = 0; i < n; ++i)
        if (std::fabs (d[i]) > th)
            return i;
    return -1;
}

// One enabled Resonator slot at mid dry/wet (the user-patch shape that made
// the idle-CPU report) + the given param spread.
void armResonatorChain (FxChain& chain)
{
    chain.prepare (48000.0, 256);
    chain.setSlotType (0, FxType::Resonator);
    chain.setSlotEnabled (0, true);
    chain.setSlotDryWet (0, 0.5f);
    for (int k = 0; k < kNumFxSlotParams; ++k)
        chain.setSlotParam (0, k, 0.5f);
}

// Render one block of pure silence through @p chain.
void renderSilence (FxChain& chain, float* outL, float* outR, int n)
{
    static std::vector<float> zero;   // shared: silence is immutable
    zero.assign ((size_t) n, 0.0f);
    chain.process (zero.data(), zero.data(), outL, outR, n);
}
}  // namespace

TEST(fx_silence_gate_test)
{
    constexpr int kBlock = 256;
    std::vector<float> outL ((size_t) kBlock), outR ((size_t) kBlock);

    // =========================================================================
    std::printf ("[1] arming: K-block debounce on a fresh silent chain\n");
    {
        FxChain chain;
        armResonatorChain (chain);

        // kGateSilentBlocks consecutive quiet blocks arm the gate; K-1 do not.
        // (A fresh chain on silence renders exact zeros, so the OUTPUT term of
        // the arm condition is satisfied from block 1 — this pins the DEBOUNCE
        // exactly.)
        for (int b = 0; b < FxChain::kGateSilentBlocksForTest() - 1; ++b)
            renderSilence (chain, outL.data(), outR.data(), kBlock);
        check (! chain.silenceGateArmedForTest(),
               "not armed before K quiet blocks");
        check (chain.gatedProcessCountForTest() == 0,
               "no gated serve before arming (full path ran)");

        renderSilence (chain, outL.data(), outR.data(), kBlock);
        check (chain.silenceGateArmedForTest(),
               "armed exactly at K quiet blocks");

        // Gated serves: exact zeros out, and the gated path is taken.
        chain.resetGatedProcessCountForTest();
        for (int b = 0; b < 3; ++b)
            renderSilence (chain, outL.data(), outR.data(), kBlock);
        check (chain.gatedProcessCountForTest() == 3, "gated serves counted");
        check (maxAbs (outL.data(), kBlock) == 0.0f && maxAbs (outR.data(), kBlock) == 0.0f,
               "gated output is exact silence");
    }

    // =========================================================================
    std::printf ("[2] tail-awareness: a hot reverb tail blocks arming\n");
    {
        FxChain chain;
        chain.prepare (48000.0, kBlock);
        chain.setSlotType (0, FxType::Reverb);
        chain.setSlotEnabled (0, true);
        chain.setSlotDryWet (0, 0.5f);
        for (int k = 0; k < kNumFxSlotParams; ++k)
            chain.setSlotParam (0, k, 0.5f);

        // Excite a real tail: 20 blocks of impulse train.
        std::vector<float> inL ((size_t) kBlock, 0.0f), inR ((size_t) kBlock, 0.0f);
        for (int i = 0; i < kBlock; ++i)
            inL[(size_t) i] = inR[(size_t) i] = (i % 32 == 0) ? 0.5f : 0.0f;
        for (int b = 0; b < 20; ++b)
            chain.process (inL.data(), inR.data(), outL.data(), outR.data(), kBlock);
        check (maxAbs (outL.data(), kBlock) > 1.0e-3f,
               "reverb tail is audible right after the burst");

        // Silence: the tail must BLOCK arming while it rings (>= 60 blocks =
        // 320 ms of silence — far past the K-block debounce, so only the
        // output-energy term can be holding it back).
        for (int b = 0; b < 60; ++b)
            renderSilence (chain, outL.data(), outR.data(), kBlock);
        check (! chain.silenceGateArmedForTest(),
               "not armed while the reverb tail still rings (output-tracking)");

        // And eventually arms once the tail has decayed (+K). Generous cap:
        // 8000 blocks (~43 s) — a Clouds reverb tail from this burst is far
        // shorter; failure here would mean the gate never arms on tails.
        bool armedEventually = false;
        for (int b = 0; b < 8000 && ! armedEventually; ++b)
        {
            renderSilence (chain, outL.data(), outR.data(), kBlock);
            armedEventually = chain.silenceGateArmedForTest();
        }
        check (armedEventually, "arms after the tail decays below the eps");
    }

    // =========================================================================
    std::printf ("[3] resets: value changes disarm, identical re-pushes do not\n");
    {
        // (a) a real param change disarms; an IDENTICAL re-push does not.
        FxChain chain;
        armResonatorChain (chain);
        for (int b = 0; b < FxChain::kGateSilentBlocksForTest() + 1; ++b)
            renderSilence (chain, outL.data(), outR.data(), kBlock);
        check (chain.silenceGateArmedForTest(), "armed before the reset checks");

        chain.setSlotParam (0, 0, 0.5f);   // same stored value (params all 0.5)
        chain.setSlotEnabled (0, true);    // same stored value
        chain.setSlotDryWet (0, 0.5f);     // same stored value
        check (chain.silenceGateArmedForTest(),
               "identical value re-pushes do NOT disarm (engine sub-chunk pin)");

        chain.setSlotParam (0, 0, 0.9f);   // a real move
        check (! chain.silenceGateArmedForTest(),
               "a real param change disarms");

        // (b) enable + master-mix toggles disarm.
        for (int b = 0; b < FxChain::kGateSilentBlocksForTest() + 1; ++b)
            renderSilence (chain, outL.data(), outR.data(), kBlock);
        check (chain.silenceGateArmedForTest(), "re-armed before the toggle checks");
        chain.setMasterMix (0.5f);
        check (! chain.silenceGateArmedForTest(), "a master-mix change disarms");
        chain.setMasterMix (1.0f);

        for (int b = 0; b < FxChain::kGateSilentBlocksForTest() + 1; ++b)
            renderSilence (chain, outL.data(), outR.data(), kBlock);
        check (chain.silenceGateArmedForTest(), "re-armed before the enable toggle");
        chain.setSlotEnabled (0, false);
        check (! chain.silenceGateArmedForTest(), "an enable toggle disarms");

        // (c) WAKE: an enabled Resonator chain, armed, fed a nonzero block —
        // passes audio (not stuck silent) and disarms.
        FxChain chain2;
        armResonatorChain (chain2);
        for (int b = 0; b < FxChain::kGateSilentBlocksForTest() + 1; ++b)
            renderSilence (chain2, outL.data(), outR.data(), kBlock);
        check (chain2.silenceGateArmedForTest(), "chain2 armed before the wake");
        std::vector<float> inL ((size_t) kBlock, 0.0f), inR ((size_t) kBlock, 0.0f);
        for (int i = 0; i < kBlock; ++i)
            inL[(size_t) i] = inR[(size_t) i] = (i % 32 == 0) ? 0.5f : 0.0f;
        chain2.process (inL.data(), inR.data(), outL.data(), outR.data(), kBlock);
        check (! chain2.silenceGateArmedForTest(), "a nonzero block wakes the gate");
        check (maxAbs (outL.data(), kBlock) > 1.0e-3f && allFinite (outL.data(), kBlock),
               "woken chain passes audible finite audio");
    }

    // =========================================================================
    std::printf ("[4] latency invariance: arm/wake preserves latency() timing\n");
    {
        // Wavefolder is the 6x-OS slot -> latency() == 8. Two IDENTICAL chains:
        // A (gate live) arms on silence, B (gate pinned OFF) runs the full
        // pre-gate path as the control. The same impulse stream then feeds
        // both; outputs must agree to <=1e-6 with IDENTICAL first-response
        // index (the rings were zeroed at arm; the control's rings hold exact
        // zeros from its own silent blocks — an all-zero ring is
        // phase-invariant, so the frozen ring positions cannot shift timing).
        auto makeChain = [&] (FxChain& c)
        {
            c.prepare (48000.0, kBlock);
            c.setSlotType (0, FxType::Wavefolder);
            c.setSlotEnabled (0, true);
            c.setSlotDryWet (0, 0.5f);
            for (int k = 0; k < kNumFxSlotParams; ++k)
                c.setSlotParam (0, k, 0.5f);
        };
        FxChain a, b;
        makeChain (a);
        makeChain (b);
        b.setSilenceGateEnabledForTest (false);   // control: exact pre-gate path
        check (a.latency() == 8, "Wavefolder slot reports latency 8 (6x OS)");

        for (int blk = 0; blk < FxChain::kGateSilentBlocksForTest() + 2; ++blk)
        {
            renderSilence (a, outL.data(), outR.data(), kBlock);
            renderSilence (b, outL.data(), outR.data(), kBlock);
        }
        check (a.silenceGateArmedForTest() && a.gatedProcessCountForTest() > 0,
               "chain A armed (and served gated blocks) before the impulse");
        check (! b.silenceGateArmedForTest(), "control chain B never armed");

        // Impulse stream: a 1.0 spike at sample 16 of block 0, silence after.
        std::vector<float> imp ((size_t) kBlock, 0.0f);
        imp[16] = 1.0f;
        std::vector<float> aL ((size_t) kBlock), aR ((size_t) kBlock);
        a.process (imp.data(), imp.data(), aL.data(), aR.data(), kBlock);
        b.process (imp.data(), imp.data(), outL.data(), outR.data(), kBlock);
        check (! a.silenceGateArmedForTest(), "the impulse woke chain A");

        float maxDiff = 0.0f;
        for (int i = 0; i < kBlock; ++i)
            maxDiff = std::max (maxDiff,
                                std::max (std::fabs (aL[(size_t) i] - outL[(size_t) i]),
                                          std::fabs (aR[(size_t) i] - outR[(size_t) i])));
        char msg[96];
        std::snprintf (msg, sizeof (msg), "gated-vs-control impulse outputs match (max diff %.2e)", (double) maxDiff);
        check (maxDiff <= 1.0e-6f, msg);


        const int idxA = firstAbove (aL.data(), kBlock, 1.0e-3f);
        const int idxB = firstAbove (outL.data(), kBlock, 1.0e-3f);
        std::snprintf (msg, sizeof (msg), "impulse timing identical (A@%d == B@%d, latency-preserved)", idxA, idxB);
        check (idxA == idxB && idxA >= 0, msg);
        check (idxA >= 8, "response is delayed by the 8-sample OS latency (not early)");
    }

    // =========================================================================
    std::printf ("[5] engine end-to-end: idle arms part 0, a note wakes it, a param edit disarms\n");
    {
        HellcatAudioProcessor proc;
        proc.prepareToPlay (48000.0, kBlock);
        auto& eng = proc.getEngine();

        // FX1 = Resonator, enabled, mid dry/wet (host-style writes -> the
        // engine's fxState + fxDirty_ service path, exactly like the UI).
        // syncAllParamsToEngine is the canonical test bridge: parameterChanged
        // DEFERS fx writes to the ~60 Hz timer off the message thread, and the
        // fork-per-test runner never pumps the loop (ui_telemetry_test's
        // pattern).
        setParam (proc, "fx1_type",    (int) FxType::Resonator);
        setParam (proc, "fx1_enabled", 1);
        setParam (proc, "fx1_drywet",  64);
        setParam (proc, "fx1_param1",  64);
        proc.syncAllParamsToEngine();

        // Pure idle: no note ever -> the part's mono sum is exact zeros; the
        // chain must arm through the FULL path (processBlock -> renderPartFx
        // sub-chunks -> chain.process). 150 host blocks (~0.8 s) is well past
        // the ~102 ms sub-chunk-cadence debounce.
        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer noMidi;
        for (int blk = 0; blk < 150; ++blk)
        {
            buf.clear();
            proc.processBlock (buf, noMidi);
        }
        check (eng.fxChainSilenceGateArmedForTest (0),
               "engine: part 0's chain arms on pure idle (FX enabled)");

        // A param WRITE on the armed idle chain disarms it within a couple of
        // blocks: fxDirty_ -> cache.baseParam -> the smoothedBase_ sub-chunk
        // glide -> a REAL setSlotParam value change (the value-guarded reset).
        setParam (proc, "fx1_param1", 100);
        proc.syncAllParamsToEngine();
        bool disarmed = false;
        for (int blk = 0; blk < 6 && ! disarmed; ++blk)
        {
            buf.clear();
            proc.processBlock (buf, noMidi);
            disarmed = ! eng.fxChainSilenceGateArmedForTest (0);
        }
        check (disarmed,
               "engine: an fx param write disarms the idle chain (live-param reset)");

        // A note-on wakes the chain and produces finite audible output.
        // (Generous pump: the fx1_param1 glide keeps disarming for a few
        // blocks; ~90 blocks ~= 470 sub-chunks leaves clear margin past the
        // 100-sub-chunk debounce.)
        for (int blk = 0; blk < FxChain::kGateSilentBlocksForTest() / 2 + 40; ++blk)
        {
            buf.clear();
            proc.processBlock (buf, noMidi);
        }
        check (eng.fxChainSilenceGateArmedForTest (0),
               "engine: chain re-armed on idle before the note");
        {
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
            bool audible = false;
            for (int blk = 0; blk < 12; ++blk)
            {
                buf.clear();
                proc.processBlock (buf, midi);
                if (maxAbs (buf.getReadPointer (0), kBlock) > 1.0e-3f)
                    audible = true;
                midi.clear();   // the note-on lands exactly once
            }
            check (! eng.fxChainSilenceGateArmedForTest (0),
                   "engine: the note woke the chain");
            check (audible && allFinite (buf.getReadPointer (0), kBlock),
                   "engine: woken chain renders audible finite audio");
        }
    }

    std::printf ("\nFX SILENCE GATE TEST: %s (%d failure%s)\n",
                 g_failures ? "FAILURES" : "ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}

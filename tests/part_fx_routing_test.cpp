// Per-part FX input-routing regression test for Parvati.
//
// renderPartFx builds each Part's FX-chain input by summing that Part's
// voicecard buffers. The buggy implementation indexed voiceCardBuffers_ by the
// Part's POOL voice indices (0..95), which only coincide with card indices
// (0..5) in the default single-part layout. With per-part voice slots or custom
// card bitmasks the old code cross-bled OTHER Parts' cards into a Part's FX
// input (every part's audio on the main bus, since the processor sums all FX
// output buffers) and left Parts whose pool slice starts at >= 6 with a SILENT
// FX input. This test pins the fixed behaviour: the sum runs over the Part's
// OWNED-card bitmask resolved by rebuildVoiceAllocation.
//
// Scenarios (each cross-bleed direction uses a FRESH processor so the "silent"
// Part's FX chain has never been excited -- a wet chain rings out for seconds
// after its own notes, which would otherwise mask the cross-bleed measurement):
//   [1] 2 Parts, disjoint cards, 16 slots each (the exact layout the old code
//       silenced Part 1 in): a note in either Part produces wet FX output in
//       that Part only; the other Part's FX output is EXACTLY zero.
//   [2] Factory multi bitmasks 0x15/0x2a (cards {0,2,4} / {1,3,5}): a note in
//       either Part produces FX output in that Part only.
//   [3] Default single-part layout (Part 0 = all 6 cards, AUTO slots): with the
//       FX chain fully dry, Part 0's FX output equals the mono sum of ALL six
//       voicecard buffers passed through the documented CHAIN-INPUT SAFETY
//       KNEE (SynthEngine::renderPartFx step 2b: 8 * SoftLimit(s/8), then the
//       ±16 hard ceiling — added in the FX crackle audit; "transparent by
//       design", -0.04 dB at |s|=1). This test predates that knee and asserted
//       the RAW sum sample-exact, which has been red ever since — the knee is
//       level-dependent, so the correct contract is knee(sum), pinned here
//       (the same honest-calibration approach as the mix_fuzz ZCR check).
//
// Built by default. Run with: ./build/parvati_part_fx_routing_test

#include <cmath>
#include "unified_test_runner.h"
#include <cstdio>

#include "stmlib/dsp/dsp.h"   // stmlib::SoftLimit (the FX chain-input knee)

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "PluginProcessor.h"
#include "SynthEngine.h"
#include "dsp/fx/FxTypes.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

constexpr int kRate  = 48000;
constexpr int kBlock = 512;

// Enable an audible FX slot on the CURRENT part (engine setters route to
// currentPart_, so switch first). Resonator with a mid dry/wet is clearly
// audible wet output while remaining short-tailed for fast settling.
void enableAudibleFx (ParvatiAudioProcessor& proc, int part)
{
    auto& eng = proc.getEngine();
    eng.setCurrentPart (part);
    eng.setFxSlotType    (0, static_cast<uint8_t> (FxType::Resonator));
    eng.setFxSlotEnabled (0, 1);
    eng.setFxSlotDryWet  (0, 90);
    eng.setFxSlotParam   (0, 0, 64);
    eng.setFxSlotParam   (0, 1, 64);
}

// Render `blocks` blocks with a note-on (channel/note) on block 0, then return
// the peak |sample| of a Part's stereo FX-output buffer over the LAST 4 blocks
// (the head is skipped so the dry/wet + fade-in smoothing has settled).
double renderFxPeak (ParvatiAudioProcessor& proc, int channel, int note, int blocks, int part)
{
    juce::AudioBuffer<float> buf (2, kBlock);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (channel, note, (uint8_t) 110), 0);
    juce::MidiBuffer empty;
    double peak = 0.0;
    for (int b = 0; b < blocks; ++b)
    {
        buf.clear();
        proc.processBlock (buf, b == 0 ? midi : empty);
        if (b >= blocks - 4)
        {
            const auto& fx = proc.getEngine().getFxOutputBuffers()[(size_t) part];
            for (int ch = 0; ch < fx.getNumChannels(); ++ch)
                for (int i = 0; i < fx.getNumSamples(); ++i)
                    peak = std::max (peak, std::fabs (static_cast<double> (fx.getSample (ch, i))));
        }
    }
    return peak;
}

// One cross-bleed direction on a FRESH processor configured by `setup`: a note
// on `channel` must produce audible wet FX in `playedPart` and EXACT silence in
// `otherPart` (whose chain has never been excited on this instance, so any
// non-zero output is true routing cross-bleed, not a decaying FX tail).
template <typename Setup>
void checkDirection (const char* label, int channel, int playedPart, int otherPart, Setup setup)
{
    ParvatiAudioProcessor proc;
    proc.prepareToPlay (kRate, kBlock);
    setup (proc);

    const double played = renderFxPeak (proc, channel, 60, 24, playedPart);
    const double other  = renderFxPeak (proc, channel, 60, 24, otherPart);

    char msg[160];
    (void) std::snprintf (msg, sizeof (msg), "%s: played part FX audible (peak %.4f)", label, played);
    check (played > 1e-4, msg);
    (void) std::snprintf (msg, sizeof (msg), "%s: other part FX exactly silent (peak %.2e)", label, other);
    check (other == 0.0, msg);
}
}  // namespace

TEST(part_fx_routing_test)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // ---------------------------------------------------------------------
    // [1] Disjoint cards, slots != card count (the old-silence layout).
    // ---------------------------------------------------------------------
    std::printf ("[1] 2 parts x (1 card, 16 slots): only the played part's FX gets signal\n");
    {
        // P0 = card 0 (channel 1), P1 = card 1 (channel 2); 16 slots each, so
        // P0's pool slice is [0..15] and P1's is [16..31]. The old code summed
        // buffers [0..5] for P0 (every part's audio) and NOTHING for P1 (all
        // its pool indices are >= kNumParts).
        const auto setup = [] (ParvatiAudioProcessor& proc)
        {
            auto& eng = proc.getEngine();
            eng.setPartVoiceAllocation (0, 0x01);
            eng.setPartVoiceAllocation (1, 0x02);
            eng.setPartVoiceSlots (0, 16);
            eng.setPartVoiceSlots (1, 16);
            enableAudibleFx (proc, 0);
            enableAudibleFx (proc, 1);
        };
        checkDirection ("ch2 -> Part 1", 2, 1, 0, setup);
        checkDirection ("ch1 -> Part 0", 1, 0, 1, setup);
    }

    // ---------------------------------------------------------------------
    // [2] Factory multi bitmasks 0x15 / 0x2a (AUTO slots => 3 voices each).
    // ---------------------------------------------------------------------
    std::printf ("\n[2] factory 0x15/0x2a bitmasks: only the played part's FX gets signal\n");
    {
        // P0 = cards {0,2,4} (channel 1), P1 = cards {1,3,5} (channel 2). The
        // old code summed buffers 0,1,2 for P0 (card 1 is Part 1's!) and 3,4,5
        // for P1 (card 4 is Part 0's) -- both directions cross-bled.
        const auto setup = [] (ParvatiAudioProcessor& proc)
        {
            auto& eng = proc.getEngine();
            eng.setPartVoiceAllocation (0, 0x15);
            eng.setPartVoiceAllocation (1, 0x2a);
            enableAudibleFx (proc, 0);
            enableAudibleFx (proc, 1);
        };
        checkDirection ("ch1 -> Part 0", 1, 0, 1, setup);
        checkDirection ("ch2 -> Part 1", 2, 1, 0, setup);
    }

    // ---------------------------------------------------------------------
    // [3] Default single-part layout: FX input equals the full card sum.
    // ---------------------------------------------------------------------
    std::printf ("\n[3] default layout: dry FX output == sum of all 6 card buffers\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (kRate, kBlock);

        // All FX disabled => the chain is a dry copy, so Part 0's FX-output
        // buffer must equal the mono sum of its OWNED cards -- with the default
        // allocation (0x3f) that is ALL six voicecard buffers -- passed through
        // the documented chain-input safety knee (renderPartFx step 2b). The
        // knee is the ENGINE's contract, not drift: replicate it here exactly
        // (8 * SoftLimit(s/8), then the ±16 hard ceiling) and expect a
        // near-sample-exact match. Tolerance 1e-6 (not 1e-9): the engine sums
        // the six card buffers in FLOAT, the reference here in double, so the
        // residual is float-accumulation rounding (~1e-8 at these levels),
        // while any real routing drift (a missing/foreign card) misses by the
        // full card level (>= 1e-2).
        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (uint8_t) 110), 0);
        juce::MidiBuffer empty;
        double maxErr = 0.0;
        double maxKnee = 0.0;   // the knee's own deviation from the raw sum
        for (int b = 0; b < 20; ++b)
        {
            buf.clear();
            proc.processBlock (buf, b == 0 ? midi : empty);
            if (b < 4)
                continue;   // let dry/wet + fade settle to the fully-dry copy

            const auto& vcs = proc.getEngine().getVoiceCardBuffers();
            const auto& fx  = proc.getEngine().getFxOutputBuffers()[0];
            for (int i = 0; i < kBlock; ++i)
            {
                double sum = 0.0;
                for (int vc = 0; vc < SynthEngine::getNumParts(); ++vc)
                    sum += static_cast<double> (vcs[(size_t) vc].getSample (0, i));
                const double kneed = juce::jlimit (-16.0, 16.0,
                                                    8.0 * stmlib::SoftLimit (static_cast<float> (sum) * 0.125f));
                const double got = static_cast<double> (fx.getSample (0, i));
                maxErr  = std::max (maxErr, std::fabs (got - kneed));
                maxKnee = std::max (maxKnee, std::fabs (kneed - sum));
            }
        }
        // Sanity: the knee must actually be exercised in this signal (else the
        // reference degenerates to the raw sum and a future knee removal would
        // go unnoticed). At these levels the knee deviates ~4e-4 from unity.
        check (maxKnee > 1e-5, "safety knee is exercised (max deviation from raw sum > 1e-5)");
        check (maxErr < 1e-6, "dry FX output == knee(owned-card (all 6) sum), sample-exact");
    }

    std::printf ("\nPART-FX-ROUTING TEST: ALL CHECKS PASSED (0 failures)\n");
    return g_failures == 0;
}

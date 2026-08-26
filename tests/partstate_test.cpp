// Part-state fidelity regression tests for Hellcat.
//
// (a) arp/sequencer data-loss regression: previously, edits to a part's arp/seq
//     (stored in the per-part Arpeggiator/Sequencer OBJECTS) were LOST when
//     saving a .MUL from a DIFFERENT current part, because saveMultiFile copied
//     the stale partBytes (the engine setters never mirrored into partBytes).
//     saveMultiFile now serializes arp/seq from the live objects for every part.
//     This test edits Part 0's arp + a modulation-sequencer step, switches to
//     Part 1, saves the .MUL, reloads it into a 2nd processor, and asserts
//     Part 0's edits survived.
// (b) all-parts-audible-init: a freshly-prepared processor now seeds EVERY part
//     with the controller init patch (osc1 = Saw), not the silent voicecard
//     fallback, so Parts 1..5 are audible by default (correct firmware behaviour).
//
// Run: ./build_unified/hellcat_unified_tests partstate_test

#include <cstdio>
#include "unified_test_runner.h"
#include "test_utils.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "PluginProcessor.h"
#include "SynthEngine.h"
#include "dsp/patch.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

// Reference Ambika factory multi (GPL-3.0), discovered via HELLCAT_SOURCE_DIR.
juce::File refMUL()
{
    return juce::File (HELLCAT_SOURCE_DIR)
        .getChildFile ("ambika_reference/controller/data/goldencard/MULTI/BANK/A/000.MUL");
}

// Part-select consistency after any load: the part_select parameter, the
// processor's currentPart_ (observable via which part a byte edit lands on)
// and the engine's current part must all agree. The .MUL load path once
// forgot to write part_select, leaving the combo showing the previous part
// while the engine had already moved to Part 0.
void assertPartSelectInSync (HellcatAudioProcessor& proc, int expectedPart0Based)
{
    const int raw = juce::roundToInt (proc.getApvts().getRawParameterValue ("part_select")->load());
    check (raw == expectedPart0Based + 1,
           "part_select parameter matches the loaded state");
    check (proc.getEngine().getCurrentPart() == expectedPart0Based,
           "engine current part matches the loaded state");
    // currentPart_ (0-based, 1 behind the parameter) is private; prove it via a
    // byte edit routing to exactly the expected part's storage.
    uint8_t before[6];
    for (int p = 0; p < 6; ++p)
        before[p] = proc.getEngine().getPart (p).partBytes[2];
    setParam (proc, "part_tuning", 21.0f);
    bool onlyExpected = proc.getEngine().getPart (expectedPart0Based).partBytes[2] == 21;
    for (int p = 0; p < 6; ++p)
        if (p != expectedPart0Based && proc.getEngine().getPart (p).partBytes[2] != before[p])
            onlyExpected = false;
    check (onlyExpected, "byte edits route to the selected part (currentPart_ in sync)");
    for (int p = 0; p < 6; ++p)
        proc.getEngine().getPart (p).partBytes[2] = before[p];   // restore
}
}  // namespace

TEST(partstate_test)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // ---------------------------------------------------------------------
    // (a) arp/sequencer data-loss regression.
    // ---------------------------------------------------------------------
    std::printf ("[1] arp/seq edits survive a part-switch + .MUL save\n");
    {
        HellcatAudioProcessor procA;
        procA.prepareToPlay (48000.0, 512);
        check (procA.loadMultiFile (refMUL()), "proc A loads reference .MUL");

        // currentPart_ is 0 after loadMultiFile. Edit Part 0's arp octave and a
        // modulation-sequencer step while Part 0 is current.
        constexpr int kArpOctave = 3;   // arp_octave is clamped to [1,4] by the bridge
        constexpr int kSeqStep0  = 99;
        setParam (procA, "arp_octave",  kArpOctave);
        setParam (procA, "seq1_step0", kSeqStep0);

        // Confirm the authoritative Part 0 config carries the edits. (Read
        // pendingConfig_, the message-thread-authoritative arp/seq config: the
        // live objects lag it until the audio thread services configDirty_.)
        check (procA.getEngine().getPart (0).pendingConfig_.arpOctave == (uint8_t) kArpOctave,
               "Part 0 arp octave updated in the live object");
        check (procA.getEngine().getPart (0).pendingConfig_.seqData[0] == (uint8_t) kSeqStep0,
               "Part 0 seq step 0 updated in the live object");

        // Switch to Part 1 (part_select is 1-based: value 2 => Part 1). This
        // loads Part 1 into the APVTS but must NOT discard Part 0's arp/seq.
        setParam (procA, "part_select", 2);
        check (procA.getEngine().getCurrentPart() == 1, "switched to Part 1");

        // Save the full multi from Part 1, then load it into a fresh processor.
        const juce::File tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                   .getChildFile ("hellcat_partstate_test.MUL");
        check (procA.saveMultiFile (tmp), "proc A saves .MUL from Part 1");

        HellcatAudioProcessor procB;
        procB.prepareToPlay (48000.0, 512);
        check (procB.loadMultiFile (tmp), "proc B loads the saved .MUL");
        assertPartSelectInSync (procB, 0);   // a .MUL load shows Part 0 everywhere

        // Part 0's arp/seq edits must have survived: they were made on Part 0
        // while it was current, then the .MUL was saved from Part 1. Before the
        // fix these reverted to the .MUL-load values (stale partBytes).
        check (procB.getEngine().getPart (0).pendingConfig_.arpOctave == (uint8_t) kArpOctave,
               "Part 0 arp_octave preserved across part-switch + .MUL save");
        check (procB.getEngine().getPart (0).pendingConfig_.seqData[0] == (uint8_t) kSeqStep0,
               "Part 0 seq1_step0 preserved across part-switch + .MUL save");

        tmp.deleteFile();
    }

    // ---------------------------------------------------------------------
    // (b) all-parts-audible-init.
    // ---------------------------------------------------------------------
    std::printf ("\n[2] every part seeds the audible controller init patch\n");
    {
        HellcatAudioProcessor proc;
        proc.prepareToPlay (48000.0, 512);

        // osc1 shape lives at patchBytes[0]; the controller init patch sets it to
        // WAVEFORM_SAW (previously the voicecard WAVEFORM_NONE silence fallback).
        bool allSaw = true;
        for (int p = 0; p < SynthEngine::getNumParts(); ++p)
            if (proc.getEngine().getPart (p).patchBytes[0] != ambika::dsp::WAVEFORM_SAW)
                allSaw = false;
        check (allSaw, "all 6 parts seed osc1 = WAVEFORM_SAW (audible init patch)");

        // The firmware arp/seq init values (octave 1, resolution 10, seq length
        // 16) must also land in pendingConfig_ (the authoritative config read by
        // serialize / loadPartIntoApvts) for every part.
        bool arpSeqInit = true;
        for (int p = 0; p < SynthEngine::getNumParts(); ++p)
        {
            const auto& part = proc.getEngine().getPart (p);
            if (part.pendingConfig_.arpOctave != 1 || part.pendingConfig_.arpResolution != 10
                || part.pendingConfig_.seqLength[0] != 16)
                arpSeqInit = false;
        }
        check (arpSeqInit, "all 6 parts carry firmware arp/seq init (octave 1 / res 10 / len 16)");
    }

    // ---------------------------------------------------------------------
    // (c) .MUL load syncs part_select: load a multi while part_select shows a
    //     DIFFERENT part (4 => Part 3, 0-based); the load must move the
    //     parameter AND the engine back to Part 0 together.
    // ---------------------------------------------------------------------
    std::printf ("\n[3] .MUL load resets part_select (combo no longer desynced)\n");
    {
        HellcatAudioProcessor proc;
        proc.prepareToPlay (48000.0, 512);
        setParam (proc, "part_select", 4);   // Part 3 (0-based) before the load
        check (proc.getEngine().getCurrentPart() == 3,
               "pre-load: Part 3 selected (sanity)");
        check (proc.loadMultiFile (refMUL()), "loads reference .MUL with Part 3 selected");
        check (juce::roundToInt (proc.getApvts().getRawParameterValue ("part_select")->load()) == 1,
               "part_select parameter is 1 after the .MUL load (was 4)");
        assertPartSelectInSync (proc, 0);
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "PART-STATE TEST: FAILURES" : "PART-STATE TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}

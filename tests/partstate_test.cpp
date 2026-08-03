// Part-state fidelity regression tests for Parvati.
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
// Built by default. Run with: ./build/parvati_partstate_test

#include <cstdio>

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

// Sets an APVTS parameter by raw (denormalized) value and fires the engine
// bridge synchronously (works for AudioParameterInt and AudioParameterChoice).
void setParam (ParvatiAudioProcessor& proc, const char* id, int value)
{
    if (auto* p = proc.getApvts().getParameter (id))
        p->setValueNotifyingHost (p->convertTo0to1 (static_cast<float> (value)));
}

// Reference Ambika factory multi (GPL-3.0), discovered via PARVATI_SOURCE_DIR.
juce::File refMUL()
{
    return juce::File (PARVATI_SOURCE_DIR)
        .getChildFile ("ambika_reference/controller/data/goldencard/MULTI/BANK/A/000.MUL");
}
}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // ---------------------------------------------------------------------
    // (a) arp/sequencer data-loss regression.
    // ---------------------------------------------------------------------
    std::printf ("[1] arp/seq edits survive a part-switch + .MUL save\n");
    {
        ParvatiAudioProcessor procA;
        procA.prepareToPlay (48000.0, 512);
        check (procA.loadMultiFile (refMUL()), "proc A loads reference .MUL");

        // currentPart_ is 0 after loadMultiFile. Edit Part 0's arp octave and a
        // modulation-sequencer step while Part 0 is current.
        constexpr int kArpOctave = 3;   // arp_octave is clamped to [1,4] by the bridge
        constexpr int kSeqStep0  = 99;
        setParam (procA, "arp_octave",  kArpOctave);
        setParam (procA, "seq1_step0", kSeqStep0);

        // Confirm the live Part 0 object carries the edits.
        check (procA.getEngine().getPart (0).arp.getOctave() == (uint8_t) kArpOctave,
               "Part 0 arp octave updated in the live object");
        check (procA.getEngine().getPart (0).seq.getSequenceDataByte (0) == (uint8_t) kSeqStep0,
               "Part 0 seq step 0 updated in the live object");

        // Switch to Part 1 (part_select is 1-based: value 2 => Part 1). This
        // loads Part 1 into the APVTS but must NOT discard Part 0's arp/seq.
        setParam (procA, "part_select", 2);
        check (procA.getEngine().getCurrentPart() == 1, "switched to Part 1");

        // Save the full multi from Part 1, then load it into a fresh processor.
        const juce::File tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                   .getChildFile ("parvati_partstate_test.MUL");
        check (procA.saveMultiFile (tmp), "proc A saves .MUL from Part 1");

        ParvatiAudioProcessor procB;
        procB.prepareToPlay (48000.0, 512);
        check (procB.loadMultiFile (tmp), "proc B loads the saved .MUL");

        // Part 0's arp/seq edits must have survived: they were made on Part 0
        // while it was current, then the .MUL was saved from Part 1. Before the
        // fix these reverted to the .MUL-load values (stale partBytes).
        check (procB.getEngine().getPart (0).arp.getOctave() == (uint8_t) kArpOctave,
               "Part 0 arp_octave preserved across part-switch + .MUL save");
        check (procB.getEngine().getPart (0).seq.getSequenceDataByte (0) == (uint8_t) kSeqStep0,
               "Part 0 seq1_step0 preserved across part-switch + .MUL save");

        tmp.deleteFile();
    }

    // ---------------------------------------------------------------------
    // (b) all-parts-audible-init.
    // ---------------------------------------------------------------------
    std::printf ("\n[2] every part seeds the audible controller init patch\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 512);

        // osc1 shape lives at patchBytes[0]; the controller init patch sets it to
        // WAVEFORM_SAW (previously the voicecard WAVEFORM_NONE silence fallback).
        bool allSaw = true;
        for (int p = 0; p < SynthEngine::getNumParts(); ++p)
            if (proc.getEngine().getPart (p).patchBytes[0] != ambika::dsp::WAVEFORM_SAW)
                allSaw = false;
        check (allSaw, "all 6 parts seed osc1 = WAVEFORM_SAW (audible init patch)");

        // The firmware arp/seq init values (octave 1, resolution 10, seq length
        // 16) must also land in the live objects so loadPartIntoApvts / saveMultiFile
        // see consistent state.
        bool arpSeqInit = true;
        for (int p = 0; p < SynthEngine::getNumParts(); ++p)
        {
            const auto& part = proc.getEngine().getPart (p);
            if (part.arp.getOctave() != 1 || part.arp.getResolution() != 10
                || part.seq.getSequenceLength (0) != 16)
                arpSeqInit = false;
        }
        check (arpSeqInit, "all 6 parts carry firmware arp/seq init (octave 1 / res 10 / len 16)");
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "PART-STATE TEST: FAILURES" : "PART-STATE TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}

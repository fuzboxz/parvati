// Host plugin-state persistence regression test for Parvati.
//
// Verifies getStateInformation / setStateInformation round-trip preserves the
// FULL 6-Part multitimbral state (patch bytes, arp/seq config, MIDI routing,
// voice allocation, current part) -- not just the current Part. Before the fix
// the host state carried only the current Part's APVTS values, so Parts 1..5
// (patch / arp / seq / routing) reverted to init on every DAW project reload.
//
// Built by default. Run with: ./build/parvati_host_state_test

#include <cstdio>
#include <memory>

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

void renderOnce (ParvatiAudioProcessor& p)
{
    juce::AudioBuffer<float> buf (2, 256);
    buf.clear();
    juce::MidiBuffer midi;
    p.processBlock (buf, midi);
}
}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // ---------------------------------------------------------------------
    // [1] Full 6-Part state survives getStateInformation / setStateInformation.
    // ---------------------------------------------------------------------
    std::printf ("[1] Full 6-Part state survives a host-state round-trip\n");
    {
        ParvatiAudioProcessor a;
        a.prepareToPlay (48000.0, 256);

        // Customize Parts 1 and 2 distinctly (directly on the engine -- they are
        // NOT the current part, so without full-persistence they would be lost).
        a.getEngine().getPart (1).patchBytes[0] = 7;            // osc1_shape
        a.getEngine().setPartChannel (1, 5);
        a.getEngine().setPartKeyrange (1, 36, 72);
        a.getEngine().setPartVoiceAllocation (1, 0x06);         // vc1 + vc2
        a.getEngine().getPart (1).pendingConfig_.arpOctave = 4;
        a.getEngine().getPart (1).pendingConfig_.seqData[0] = 77;
        a.getEngine().getPart (1).configDirty_.store (true);

        a.getEngine().getPart (2).patchBytes[0] = 11;
        a.getEngine().setPartChannel (2, 9);
        a.getEngine().setPartKeyrange (2, 48, 84);
        a.getEngine().setPartVoiceAllocation (2, 0x18);         // vc3 + vc4
        a.getEngine().getPart (2).pendingConfig_.arpResolution = 3;
        a.getEngine().getPart (2).configDirty_.store (true);

        // Make Part 2 the current part (saved + restored).
        a.getEngine().setCurrentPart (2);
        renderOnce (a);

        // Capture host state.
        juce::MemoryBlock blob;
        a.getStateInformation (blob);
        check (blob.getSize() > 0, "getStateInformation produced a non-empty block");

        // Restore into a fresh processor + service the deferred rebuild.
        ParvatiAudioProcessor b;
        b.prepareToPlay (48000.0, 256);
        b.setStateInformation (blob.getData(), (int) blob.getSize());
        renderOnce (b);

        // Part 1 survived verbatim.
        check (b.getEngine().getPart (1).patchBytes[0] == 7, "Part 1 osc1_shape preserved");
        check (b.getEngine().getPartChannel (1) == 5, "Part 1 midi channel preserved");
        check (b.getEngine().getPartKeyrangeLow (1) == 36 && b.getEngine().getPartKeyrangeHigh (1) == 72,
               "Part 1 keyzone preserved");
        check (b.getEngine().getPartVoiceAllocation (1) == 0x06, "Part 1 voice allocation preserved");
        check (b.getEngine().getPart (1).pendingConfig_.arpOctave == 4, "Part 1 arp octave preserved");
        check (b.getEngine().getPart (1).pendingConfig_.seqData[0] == 77, "Part 1 seq step preserved");

        // Part 2 survived + is the restored current part.
        check (b.getEngine().getPart (2).patchBytes[0] == 11, "Part 2 osc1_shape preserved");
        check (b.getEngine().getPartChannel (2) == 9, "Part 2 midi channel preserved");
        check (b.getEngine().getPartVoiceAllocation (2) == 0x18, "Part 2 voice allocation preserved");
        check (b.getEngine().getPart (2).pendingConfig_.arpResolution == 3, "Part 2 arp resolution preserved");
        check (b.getEngine().getCurrentPart() == 2, "current part preserved");
    }

    // ---------------------------------------------------------------------
    // [2] Backward compat: a legacy state with no engine_state falls back
    //     gracefully (no crash; Parts seed init; current Part from APVTS).
    // ---------------------------------------------------------------------
    std::printf ("\n[2] Legacy state (no engine_state) falls back gracefully\n");
    {
        // Capture a real state, then strip the engine_state attribute to mimic
        // a pre-persistence saved project.
        ParvatiAudioProcessor a;
        a.prepareToPlay (48000.0, 256);
        juce::MemoryBlock full;
        a.getStateInformation (full);

        auto xml = juce::AudioProcessor::getXmlFromBinary (full.getData(), (int) full.getSize());
        check (xml != nullptr && xml->hasAttribute ("engine_state"), "fresh state carries engine_state");
        xml->removeAttribute ("engine_state");

        juce::MemoryBlock legacy;
        juce::AudioProcessor::copyXmlToBinary (*xml, legacy);

        ParvatiAudioProcessor b;
        b.prepareToPlay (48000.0, 256);
        bool threw = false;
        try { b.setStateInformation (legacy.getData(), (int) legacy.getSize()); }
        catch (...) { threw = true; }
        renderOnce (b);
        check (! threw, "legacy state restores without throwing");
        // Fallback path: Part 0 keeps the seeded audible init patch (no blob apply).
        check (b.getEngine().getPart (0).patchBytes[0] == ambika::dsp::WAVEFORM_SAW,
               "Part 0 seeds init patch after legacy restore");
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "HOST-STATE TEST: FAILURES" : "HOST-STATE TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}

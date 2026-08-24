// CC routing test — sustain pedal (CC64), all-notes-off (CC123/CC120) and the
// channel-scoped continuous controllers (CC1/2/4), all added in W7 from the
// Round-3 lane-B review (firmware parity against ambika_reference).
//
//   [1] Sustain pedal: CC64 down + note + key release -> the voice KEEPS
//       sounding (the release is swallowed); CC64 up -> the stored release
//       runs and the voice stops. Without the pedal the release is immediate.
//   [2] Sustain + arp: the pedal keeps the arp's held-key stack populated
//       (no re-trigger churn), and pedal-up drains it (stack empty).
//   [3] CC123 clears a part's arp held-key stack + stops its voices (the
//       base-class all-notes-off left the stack alive, so the arp kept
//       re-triggering from "held" keys).
//   [4] CC1 (mod wheel) writes only the parts whose channel matches: a
//       two-part split (ch1 / ch2) sees the CC land on part B's voices only.
//
// Run: ./build_unified/parvati_unified_tests cc_routing_test

#include <cstdio>
#include "unified_test_runner.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "SynthEngine.h"
#include "dsp/patch.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg) { std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg); if (! cond) ++g_failures; }

constexpr int kRate = 48000;
constexpr int kBlock = 256;

void render (ParvatiAudioProcessor& p, const juce::MidiBuffer& midi, int blocks)
{
    juce::MidiBuffer none;
    for (int i = 0; i < blocks; ++i)
    {
        juce::AudioBuffer<float> buf (2, kBlock);
        buf.clear();
        p.processBlock (buf, const_cast<juce::MidiBuffer&> (i == 0 ? midi : none));
    }
}

juce::MidiBuffer one (const juce::MidiMessage& m)
{
    juce::MidiBuffer b;
    b.addEvent (m, 0);
    return b;
}

// Sounding (envelope not finished) voices of @p part.
int activeVoices (ParvatiAudioProcessor& p, int part)
{
    auto& e = p.getEngine();
    int n = 0;
    for (int i = 0; i < e.getNumVoices(); ++i)
        if (auto* av = e.getAmbikaVoice (i);
            av != nullptr && av->getPartIndex() == part && av->isDisplayedActive())
            ++n;
    return n;
}

// The MOD_SRC_WHEEL value on part @p 's first voice (0 when it has none).
int wheelValue (ParvatiAudioProcessor& p, int part)
{
    auto& e = p.getEngine();
    for (int vi : e.getPart (part).voiceIndices)
        if (auto* av = e.getAmbikaVoice (vi))
            return (int) av->getModulationSource (ambika::dsp::MOD_SRC_WHEEL);
    return -1;
}
}  // namespace

TEST(cc_routing_test)
{
    // ------------------------------------------------------------------
    // [1] Sustain pedal CC64: swallow releases, drain on pedal-up.
    // ------------------------------------------------------------------
    std::printf ("[1] sustain pedal: CC64 holds a released note, pedal-up releases\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (kRate, kBlock);

        render (proc, one (juce::MidiMessage::controllerEvent (1, 64, 127)), 1);   // pedal down
        render (proc, one (juce::MidiMessage::noteOn (1, 60, (uint8_t) 100)), 4);
        check (activeVoices (proc, 0) == 1, "note sounds under the pedal");
        render (proc, one (juce::MidiMessage::noteOff (1, 60, (uint8_t) 0)), 8);
        check (activeVoices (proc, 0) == 1,
               "key release under the pedal KEEPS the voice sounding (swallowed)");
        render (proc, one (juce::MidiMessage::controllerEvent (1, 64, 0)), 8);   // pedal up
        render (proc, juce::MidiBuffer(), 80);   // let the release tail finish
        check (activeVoices (proc, 0) == 0, "pedal-up releases the sustained voice");

        // Without the pedal the same sequence releases immediately.
        render (proc, one (juce::MidiMessage::noteOn (1, 62, (uint8_t) 100)), 4);
        render (proc, one (juce::MidiMessage::noteOff (1, 62, (uint8_t) 0)), 8);
        render (proc, juce::MidiBuffer(), 80);
        check (activeVoices (proc, 0) == 0, "without the pedal the release is immediate");
    }

    // ------------------------------------------------------------------
    // [2] Sustain + arp: the pedal keeps the held-key stack, pedal-up drains.
    // ------------------------------------------------------------------
    std::printf ("\n[2] sustain + arp: held-key stack survives the key release, drains on pedal-up\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (kRate, kBlock);
        auto& e = proc.getEngine();

        // Arp ON for part 0 (engine-direct setter, message thread), flush one block.
        e.setArpMode (1);
        { juce::AudioBuffer<float> b (2, kBlock); b.clear(); juce::MidiBuffer m; proc.processBlock (b, m); }

        render (proc, one (juce::MidiMessage::controllerEvent (1, 64, 127)), 1);
        render (proc, one (juce::MidiMessage::noteOn (1, 64, (uint8_t) 100)), 2);
        check (e.getPart (0).arp.hasHeldKeys(), "arp holds the key");
        render (proc, one (juce::MidiMessage::noteOff (1, 64, (uint8_t) 0)), 2);
        check (e.getPart (0).arp.hasHeldKeys(),
               "key release under the pedal keeps the arp's held-key stack (pedal semantics)");
        render (proc, one (juce::MidiMessage::controllerEvent (1, 64, 0)), 2);
        check (! e.getPart (0).arp.hasHeldKeys(), "pedal-up drains the held-key stack");
        e.setArpMode (0);
    }

    // ------------------------------------------------------------------
    // [3] CC123: clears the arp stack + stops the part's voices.
    // ------------------------------------------------------------------
    std::printf ("\n[3] CC123 all-notes-off clears arp stacks and stops voices\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (kRate, kBlock);
        auto& e = proc.getEngine();

        e.setArpMode (1);
        { juce::AudioBuffer<float> b (2, kBlock); b.clear(); juce::MidiBuffer m; proc.processBlock (b, m); }
        render (proc, one (juce::MidiMessage::noteOn (1, 65, (uint8_t) 100)), 2);
        check (e.getPart (0).arp.hasHeldKeys(), "arp holds the key before CC123");
        render (proc, one (juce::MidiMessage::controllerEvent (1, 123, 0)), 8);
        check (! e.getPart (0).arp.hasHeldKeys(), "CC123 clears the arp held-key stack");
        check (activeVoices (proc, 0) == 0, "CC123 stops the part's sounding voices");
        // And with the pedal down, CC123 is a no-op (firmware part.cc:541).
        // (Arp OFF for this segment so the note plays DIRECTLY — an idle
        // transport never sounds arp-held keys.)
        e.setArpMode (0);
        { juce::AudioBuffer<float> b (2, kBlock); b.clear(); juce::MidiBuffer m; proc.processBlock (b, m); }
        render (proc, one (juce::MidiMessage::controllerEvent (1, 64, 127)), 1);
        render (proc, one (juce::MidiMessage::noteOn (1, 67, (uint8_t) 100)), 4);
        render (proc, one (juce::MidiMessage::controllerEvent (1, 123, 0)), 4);
        check (activeVoices (proc, 0) == 1,
               "CC123 under a held pedal is a no-op (firmware ignore_note_off gate)");
        render (proc, juce::MidiBuffer(), 4);
        render (proc, one (juce::MidiMessage::controllerEvent (1, 64, 0)), 8);
        e.setArpMode (0);
    }

    // ------------------------------------------------------------------
    // [4] CC1 routes per channel: only the matching part's voices change.
    // ------------------------------------------------------------------
    std::printf ("\n[4] CC1 mod wheel reaches only the channel-matching part's voices\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (kRate, kBlock);
        auto& e = proc.getEngine();

        // Two parts: part 0 on ch1, part 1 on ch2, one voice each.
        e.setPartMidiChannel (0, 1);
        e.setPartMidiChannel (1, 2);
        e.setPartVoiceSlots (0, 1);
        e.setPartVoiceSlots (1, 1);
        render (proc, one (juce::MidiMessage::noteOn (1, 60, (uint8_t) 100)), 1);
        render (proc, one (juce::MidiMessage::noteOn (2, 60, (uint8_t) 100)), 4);
        check (activeVoices (proc, 0) == 1 && activeVoices (proc, 1) == 1, "both parts sound");
        check (wheelValue (proc, 0) == 0 && wheelValue (proc, 1) == 0, "wheel starts at 0 on both parts");

        // CC1=64 on channel 2 -> only part 1's voices see 128.
        render (proc, one (juce::MidiMessage::controllerEvent (2, 1, 64)), 2);
        check (wheelValue (proc, 1) == 128, "CC1 on ch2 writes part 1's voices (value<<1)");
        check (wheelValue (proc, 0) == 0, "CC1 on ch2 does NOT touch part 0's voices");
    }

    std::printf ("\n%s (%d failures)\n",
                 g_failures ? "CC ROUTING TEST: FAILURES" : "CC ROUTING TEST: ALL CHECKS PASSED",
                 g_failures);
    return g_failures == 0;
}

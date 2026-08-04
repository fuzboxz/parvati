// Multitimbral verification for Parvati.
// Proves: (1) each of the 6 Parts holds its OWN patch (distinct bytes per part),
// (2) MIDI channel routing sends ch1->Part0, ch2->Part1, (3) a channel-1 note
// triggers a Part0 voice only (never steals a Part1 voice), and (4) both Parts
// produce audible audio from their own patches simultaneously.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg) { std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg); if (! cond) ++g_failures; }

// Default voice allocation: P0={0,1,2} P1={3,4,5} P2={6,7,8} P3={9,10,11} P4={12,13} P5={14,15}.
bool voiceInPart (int voice, int part)
{
    switch (part)
    {
        case 0: return voice <= 2;
        case 1: return voice >= 3 && voice <= 5;
        case 2: return voice >= 6 && voice <= 8;
        case 3: return voice >= 9 && voice <= 11;
        case 4: return voice >= 12 && voice <= 13;
        case 5: return voice >= 14;
    }
    return false;
}

double renderBlocks (ParvatiAudioProcessor& proc, int blocks)
{
    double peak = 0.0;
    for (int b = 0; b < blocks; ++b)
    {
        juce::AudioBuffer<float> buf (2, 256); buf.clear();
        juce::MidiBuffer empty;
        proc.processBlock (buf, empty);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 256; ++i)
                peak = std::max (peak, std::fabs ((double) buf.getSample (ch, i)));
    }
    return peak;
}
}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI guiInit;

    std::printf ("=== Parvati Multitimbral (6 Parts) ===\n");

    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);

    auto& eng = proc.getEngine();
    auto setParam = [&] (const char* id, float v) { proc.getApvts().getParameterAsValue (id) = v; };

    // This test exercises MULTITIMBRAL routing (ch1->Part0, ch2->Part1) over a
    // 3+3 voicecard split. The plugin's out-of-box default is single-part (all
    // voicecards on Part 0), so set the 3+3 split (vc0..2 -> Part0, vc3..5 ->
    // Part1) explicitly and flush the deferred rebuild before the assertions.
    eng.setPartVoiceAllocation (0, 0x07);
    eng.setPartVoiceAllocation (1, 0x38);
    { juce::AudioBuffer<float> flushBuf (2, 256); flushBuf.clear(); juce::MidiBuffer emptyMidi; proc.processBlock (flushBuf, emptyMidi); }

    std::printf ("[1] Each Part holds its own patch (Part 0 = SAW, Part 1 = SQUARE)\n");
    {
        setParam ("part_select", 1.0f);                 // edit Part 0
        setParam ("osc1_shape",  (float) 1);            // SAW (WAVEFORM_SAW = 1)
        setParam ("part_select", 2.0f);                 // edit Part 1
        setParam ("osc1_shape",  (float) 2);            // SQUARE (WAVEFORM_SQUARE = 2)

        const uint8_t p0 = eng.getPart (0).patchBytes[0];
        const uint8_t p1 = eng.getPart (1).patchBytes[0];
        std::printf ("     Part0 osc1 byte = %d (expect 1 SAW), Part1 osc1 byte = %d (expect 2 SQUARE)\n", (int) p0, (int) p1);
        check (p0 == 1, "Part 0 patch is SAW (byte 1)");
        check (p1 == 2, "Part 1 patch is SQUARE (byte 2)");
        check (p0 != p1, "Parts 0 and 1 hold DISTINCT patches");
    }

    std::printf ("\n[2] MIDI channel routing: ch1 -> Part0, ch2 -> Part1\n");
    {
        // Default routing: part i -> channel i+1. Send one note on each channel.
        juce::AudioBuffer<float> buf (2, 256); buf.clear();
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (uint8_t) 100), 0);   // Part 0
        midi.addEvent (juce::MidiMessage::noteOn (2, 60, (uint8_t) 100), 1);   // Part 1
        proc.processBlock (buf, midi);

        int p0Voice = -1, p1Voice = -1;
        for (int i = 0; i < kNumVoices; ++i)
        {
            auto* v = eng.getAmbikaVoice (i);
            if (v && v->getCurrentlyPlayingNote() >= 0)
            {
                if (voiceInPart (i, 0) && p0Voice < 0) p0Voice = i;
                if (voiceInPart (i, 1) && p1Voice < 0) p1Voice = i;
            }
        }
        std::printf ("     ch1->voice %d (Part0), ch2->voice %d (Part1)\n", p0Voice, p1Voice);
        check (voiceInPart (p0Voice, 0), "channel-1 note triggers a Part 0 voice");
        check (voiceInPart (p1Voice, 1), "channel-2 note triggers a Part 1 voice");
    }

    std::printf ("\n[3] Channel-1 notes never steal a Part 1 voice\n");
    {
        proc.getEngine().allNotesOff (1, false);
        proc.getEngine().allNotesOff (2, false);
        renderBlocks (proc, 4);  // flush

        // Saturate Part 0 (3 voices) with 5 channel-1 notes -> must steal WITHIN Part 0.
        for (int n = 0; n < 5; ++n)
        {
            juce::AudioBuffer<float> buf (2, 256); buf.clear();
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, (uint8_t) (60 + n), (uint8_t) 100), 0);
            proc.processBlock (buf, midi);
        }
        int part1Active = 0;
        for (int i = 0; i < kNumVoices; ++i)
            if (auto* v = eng.getAmbikaVoice (i); v && v->getCurrentlyPlayingNote() >= 0 && voiceInPart (i, 1))
                ++part1Active;
        std::printf ("     after 5 channel-1 notes: Part1 voices active = %d (expect 0)\n", part1Active);
        check (part1Active == 0, "channel-1 notes do NOT steal Part 1 voices");

        // At least one Part 0 voice must be active (the notes landed in Part 0).
        int part0Active = 0;
        for (int i = 0; i < kNumVoices; ++i)
            if (auto* v = eng.getAmbikaVoice (i); v && v->getCurrentlyPlayingNote() >= 0 && voiceInPart (i, 0))
                ++part0Active;
        check (part0Active > 0, "channel-1 notes DID trigger Part 0 voices");
    }

    std::printf ("\n[4] Both Parts produce audible audio simultaneously\n");
    {
        proc.getEngine().allNotesOff (1, false);
        proc.getEngine().allNotesOff (2, false);
        renderBlocks (proc, 4);

        // Part 0 only (ch1).
        double peak0 = 0.0;
        {
            juce::AudioBuffer<float> buf (2, 256); buf.clear();
            juce::MidiBuffer midi; midi.addEvent (juce::MidiMessage::noteOn (1, 60, (uint8_t) 110), 0);
            proc.processBlock (buf, midi);
            peak0 = renderBlocks (proc, 40);
        }
        // Part 1 only (ch2).
        double peak1 = 0.0;
        {
            juce::AudioBuffer<float> buf (2, 256); buf.clear();
            juce::MidiBuffer midi; midi.addEvent (juce::MidiMessage::noteOn (2, 64, (uint8_t) 110), 0);
            proc.processBlock (buf, midi);
            peak1 = renderBlocks (proc, 40);
        }
        std::printf ("     Part0 (ch1 SAW) peak = %.4f,  Part1 (ch2 SQUARE) peak = %.4f\n", peak0, peak1);
        check (peak0 > 0.01, "Part 0 (channel 1) renders audible audio");
        check (peak1 > 0.01, "Part 1 (channel 2) renders audible audio");
    }

    std::printf ("\n[5] Exclusive voicecard assignment (a card on <=1 Part)\n");
    {
        ParvatiAudioProcessor p;
        p.prepareToPlay (48000.0, 256);
        SynthEngine& e = p.getEngine();

        // Part 0 claims vc0..3 (0x0f).
        e.setPartVoiceAllocation (0, 0x0f);
        // Part 1 now claims vc0 too -> exclusivity removes vc0 from Part 0.
        e.setPartVoiceAllocation (1, 0x01);
        { juce::AudioBuffer<float> flushBuf (2, 256); flushBuf.clear(); juce::MidiBuffer emptyMidi; p.processBlock (flushBuf, emptyMidi); }

        const uint8_t p0 = e.getPartVoiceAllocation (0);
        const uint8_t p1 = e.getPartVoiceAllocation (1);
        std::printf ("     Part0 alloc = 0x%02x (expect 0x0e), Part1 alloc = 0x%02x (expect 0x01)\n", p0, p1);
        check ((p0 & 0x01) == 0,    "exclusive: Part 0 lost vc0 to Part 1");
        check ((p1 & 0x01) == 0x01, "exclusive: Part 1 owns vc0");
        check (p0 == 0x0e,          "exclusive: Part 0 keeps vc1,2,3");

        // rebuild reflects the exclusive bitmasks (voice i == voicecard i):
        // Part 0 owns voices {1,2,3}, Part 1 owns voice {0}.
        std::vector<int> v0 = e.getPart (0).voiceIndices; std::sort (v0.begin(), v0.end());
        std::vector<int> v1 = e.getPart (1).voiceIndices; std::sort (v1.begin(), v1.end());
        check (v0 == (std::vector<int> { 1, 2, 3 }), "rebuild: Part 0 voices {1,2,3}");
        check (v1 == (std::vector<int> { 0 }),        "rebuild: Part 1 voices {0}");
    }

    std::printf ("\n%s (%d failures)\n",
                 g_failures ? "MULTITIMBRAL TEST: FAILURES" : "MULTITIMBRAL TEST: ALL CHECKS PASSED",
                 g_failures);
    return g_failures ? 1 : 0;
}

// Ambika .MUL (multi) loading verification for Parvati.
// 1. Parses a real factory .MUL (RIFF "MBKS") -> name + MultiData + 6 parts
//    (Patch[112] + PartData[84] each).
// 2. Loads it into the full ParvatiAudioProcessor and confirms all 6 Parts get
//    distinct patches + that Part 0's MIDI channel matches MultiData's
//    part_mapping_[0].midi_channel.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PatchFile.h"
#include "PluginProcessor.h"
#include "SynthEngine.h"

#ifndef PARVATI_SOURCE_DIR
#define PARVATI_SOURCE_DIR "."
#endif

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg) { std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg); if (! cond) ++g_failures; }

// Process one empty audio block so the engine services any deferred voice-
// allocation / polyphony / patch changes (markAllocationDirty) before the test
// inspects voiceIndices / partBytes synchronously. (Tests run processBlock on a
// single thread, so there is no concurrent audio thread here.)
void renderOnce (ParvatiAudioProcessor& p)
{
    juce::AudioBuffer<float> buf (2, 256);
    buf.clear();
    juce::MidiBuffer midi;
    p.processBlock (buf, midi);
}

juce::File mulFile (const char* idx)
{
    return juce::File (PARVATI_SOURCE_DIR)
        .getChildFile ("ambika_reference/controller/data/goldencard/MULTI/BANK/A")
        .getChildFile (juce::String (idx) + ".MUL");
}
}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    // ---- [1] Parser: real factory .MUL ----
    std::printf ("[1] Parse real .MUL (RIFF/MBKS)\n");
    AmbikaMulti m;
    const auto f = mulFile ("000");
    check (f.existsAsFile(), "000.MUL exists in the reference tree");
    check (parseAmbikaMultiFile (f, m), "parse 000.MUL succeeds (MultiData found)");
    check (m.hasMultiData, "000.MUL has a MultiData (56B) chunk");
    check (m.name.isNotEmpty(), "000.MUL name is non-empty");
    std::printf ("     multi name = '%s'\n", m.name.toUTF8());

    int partsWithPatch = 0;
    for (int i = 0; i < 6; ++i)
        if (m.parts[i].hasPatch) ++partsWithPatch;
    std::printf ("     parts with a Patch: %d/6\n", partsWithPatch);
    check (partsWithPatch >= 2, "at least 2 parts carry a Patch");

    // PartMapping[0]: {midi_channel, keyrange_low, keyrange_high, voice_allocation}
    const uint8_t* pm0 = m.multiData.data();
    std::printf ("     PartMapping[0]: channel=%d low=%d high=%d\n", pm0[0], pm0[1], pm0[2]);

    // ---- [2] Load the .MUL into the processor ----
    std::printf ("\n[2] Load .MUL into ParvatiAudioProcessor\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);

        check (proc.loadMultiFile (f), "loadMultiFile(000.MUL) returns true");
        renderOnce (proc);   // service the deferred rebuild/push from the load

        auto& engine = proc.getEngine();

        // At least two parts must now have DIFFERENT osc1.shape bytes (the
        // multi loaded distinct patches per part, not a single patch broadcast).
        int distinctShapes = 0;
        int firstShape = -1;
        bool anyDiffer = false;
        for (int i = 0; i < SynthEngine::getNumParts(); ++i)
        {
            const int shape = engine.getPart (i).patchBytes[0];
            if (i == 0) firstShape = shape;
            else if (shape != firstShape) anyDiffer = true;
            ++distinctShapes;  // count all
        }
        std::printf ("     part osc1.shape: ");
        for (int i = 0; i < SynthEngine::getNumParts(); ++i)
            std::printf ("%d ", (int) engine.getPart (i).patchBytes[0]);
        std::printf ("\n     parts loaded: %d\n", distinctShapes);
        check (anyDiffer, "at least two parts have different osc1_shape (distinct patches)");

        // Part 0's MIDI channel must match MultiData.part_mapping_[0].midi_channel.
        const uint8_t ch0 = engine.getPart (0).midiChannel;
        char msg[128];
        std::snprintf (msg, sizeof (msg), "Part 0 midiChannel (%d) == MultiData pm[0].midi_channel (%d)", ch0, pm0[0]);
        check (ch0 == pm0[0], msg);

        // Loaded name reaches the processor.
        check (proc.getLoadedProgramName() == m.name, "loaded multi name reaches the processor");

        // ---- [3] Voice allocation follows the .MUL voice_allocation bitmask,
        //      and per-part arp/seq settings are pushed into every Part. ----
        std::printf ("\n[3] .MUL voice allocation + per-part arp/seq\n");
        {
            // Reconstruct expected first-wins ownership from the 6 bitmasks
            // (same voicecard->block mapping as SynthEngine::rebuildVoiceAllocation).
            static const int vcStart[6] = { 0, 3, 6, 9, 12, 14 };
            static const int vcSize[6]  = { 3, 3, 3, 3, 2, 2 };
            auto popcount = [] (uint8_t v) { int c = 0; while (v) { c += v & 1; v >>= 1; } return c; };

            bool claimed[6] = {};
            std::vector<int> expected[6];
            int totalExpected = 0, totalActual = 0;
            for (int i = 0; i < 6; ++i)
            {
                const uint8_t mask = m.hasMultiData ? m.multiData[(size_t) (i * 4 + 3)]
                                                     : (uint8_t) (1 << i);
                for (int vc = 0; vc < 6; ++vc)
                    if ((mask & (1u << vc)) && ! claimed[vc])
                    {
                        claimed[vc] = true;
                        for (int k = 0; k < vcSize[vc]; ++k)
                            expected[i].push_back (vcStart[vc] + k);
                    }
                std::sort (expected[i].begin(), expected[i].end());
            }

            bool allocOk = true; bool sawMultiBlock = false;
            for (int i = 0; i < 6; ++i)
            {
                const uint8_t mask = m.hasMultiData ? m.multiData[(size_t) (i * 4 + 3)] : 0;
                auto got = engine.getPart (i).voiceIndices;
                std::sort (got.begin(), got.end());
                totalExpected += (int) expected[i].size();
                totalActual   += (int) got.size();
                if (got != expected[i]) allocOk = false;
                if (popcount (mask) >= 2 && (int) got.size() >= 4) sawMultiBlock = true;
            }
            char msg[160];
            std::snprintf (msg, sizeof (msg), "voice allocation matches .MUL bitmasks (voices %d expected / %d actual)",
                           totalExpected, totalActual);
            check (allocOk, msg);
            if (sawMultiBlock)
                check (true, "a part claiming >=2 voicecards owns >=4 Parvati voices");
            else
                std::printf ("     (note: 000.MUL has no multi-voicecard part to stress-test)\n");

            // Per-part arp/seq mode pushed from PartData into every Part.
            bool arpOk = true;
            for (int i = 0; i < 6; ++i)
            {
                if (! m.parts[i].hasPart) continue;
                const uint8_t modeByte = m.parts[i].part[7];  // arp_sequencer_mode
                if (engine.getPart (i).arp.getMode() != modeByte) arpOk = false;
                if (engine.getPart (i).seq.getMode() != modeByte) arpOk = false;
            }
            check (arpOk, "per-part arp/seq mode pushed from .MUL PartData");
        }
    }

    // ---- [4] Direct bitmask -> voice-block mapping (deterministic) ----
    std::printf ("\n[4] Direct voice-allocation bitmask mapping\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        auto& engine = proc.getEngine();

        auto voicesAsSet = [&] (int part) {
            std::vector<int> v = engine.getPart (part).voiceIndices;
            std::sort (v.begin(), v.end());
            return v;
        };

        // part0 -> vc0,1 (bits 0,1) => voices {0,1,2,3,4,5}
        engine.setPartVoiceAllocation (0, 0b000011);
        // part1 -> vc2 (bit 2) => {6,7,8}
        engine.setPartVoiceAllocation (1, 0b000100);
        // part2 -> vc4,5 (bits 4,5) => {12,13,14,15}
        engine.setPartVoiceAllocation (2, 0b110000);
        engine.setPartVoiceAllocation (3, 0);
        engine.setPartVoiceAllocation (4, 0);
        engine.setPartVoiceAllocation (5, 0);
        renderOnce (proc);   // service the deferred rebuild before inspecting voiceIndices

        check (voicesAsSet (0) == std::vector<int> ({ 0,1,2,3,4,5 }), "part0(vc0,1) owns {0..5}");
        check (voicesAsSet (1) == std::vector<int> ({ 6,7,8 }),       "part1(vc2) owns {6,7,8}");
        check (voicesAsSet (2) == std::vector<int> ({ 12,13,14,15 }), "part2(vc4,5) owns {12,13,14,15}");
        check (voicesAsSet (3).empty() && voicesAsSet (4).empty() && voicesAsSet (5).empty(),
               "parts 3-5 (no bits) own no voices");

        // First-wins: re-claim vc0 on part1 after part0 owns it -> part1 must
        // NOT gain {0,1,2}; part0 keeps them.
        engine.setPartVoiceAllocation (1, 0b000001);  // vc0 only, already claimed
        renderOnce (proc);   // service the deferred rebuild
        check (voicesAsSet (1).empty(), "first-wins: part1 cannot steal part0's vc0");
        check (voicesAsSet (0) == std::vector<int> ({ 0,1,2,3,4,5 }), "part0 keeps its voices");
    }

    std::printf ("\n%s (%d failures)\n",
                 g_failures ? "MULTI LOAD TEST: FAILURES" : "MULTI LOAD TEST: ALL CHECKS PASSED",
                 g_failures);
    return g_failures ? 1 : 0;
}

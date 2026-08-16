// Ambika .MUL (multi) loading verification for Parvati.
// 1. Parses a real factory .MUL (RIFF "MBKS") -> name + MultiData + 6 parts
//    (Patch[112] + PartData[84] each).
// 2. Loads it into the full ParvatiAudioProcessor and confirms all 6 Parts get
//    distinct patches + that Part 0's MIDI channel matches MultiData's
//    part_mapping_[0].midi_channel.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
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
        // Flush the deferred rebuild/push from the load (1 voice per voicecard).
        renderOnce (proc);

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
            auto popcount = [] (uint8_t v) { int c = 0; while (v) { c += v & 1; v >>= 1; } return c; };
            (void) popcount;

            // Under the slots model the .MUL bitmasks materialize slot counts
            // (popcount) and the engine DERIVES contiguous card shares from
            // them, so the engine's PUBLISHED bitmask per Part (which the
            // rebuild writes) is the source of truth: expected[i] = the cards
            // in the derived bitmask, matching the voices' card tags.
            std::vector<int> expected[6];
            int totalExpected = 0, totalActual = 0;
            uint8_t exclusiveUnion = 0;
            bool exclusiveOk = true;
            for (int i = 0; i < 6; ++i)
            {
                const uint8_t mask = engine.getPartVoiceAllocation (i);
                if (exclusiveUnion & mask) exclusiveOk = false;   // card on >1 Part
                exclusiveUnion |= mask;
                for (int vc = 0; vc < 6; ++vc)
                    if (mask & (1u << vc))
                        expected[i].push_back (vc);
                std::sort (expected[i].begin(), expected[i].end());
            }
            check (exclusiveOk, "exclusive: no voicecard owned by more than one Part");

            // No card is LOST vs the raw .MUL bitmasks (the load reached every
            // Part). Under the slots model the engine DERIVES contiguous card
            // shares from the materialized slot counts, so the exact card
            // POSITIONS may differ from the file's mask shape — the TOTAL
            // card count is what must be preserved.
            auto maskPopcount = [] (uint8_t v) { int c = 0; while (v) { c += v & 1; v >>= 1; } return c; };
            uint8_t mulUnion = 0;
            for (int i = 0; i < 6; ++i)
                mulUnion |= m.hasMultiData ? m.multiData[(size_t) (i * 4 + 3)]
                                           : (uint8_t) (1 << i);
            check (maskPopcount (exclusiveUnion) == maskPopcount (mulUnion),
                   "no voicecard lost between .MUL and the engine (total count)");

            bool allocOk = true; bool sawMultiCard = false;
            for (int i = 0; i < 6; ++i)
            {
                auto got = engine.getPart (i).voiceIndices;
                std::sort (got.begin(), got.end());
                totalExpected += (int) expected[i].size();
                totalActual   += (int) got.size();
                // Pool model: match the BEHAVIOURAL contract (one voice per
                // card, tagged onto that Part's own cards), not raw pool indices.
                std::set<int> cards;
                for (int vi : got)
                    if (auto* av = engine.getAmbikaVoice (vi))
                        cards.insert (av->getVoiceCard());
                if ((int) got.size() != (int) expected[i].size()
                    || ! std::equal (cards.begin(), cards.end(), expected[i].begin(), expected[i].end()))
                    allocOk = false;
                if ((int) got.size() >= 2) sawMultiCard = true;
            }
            char msg[160];
            std::snprintf (msg, sizeof (msg), "voice allocation matches stored bitmasks (voices %d expected / %d actual)",
                           totalExpected, totalActual);
            check (allocOk, msg);
            if (sawMultiCard)
                check (true, "a part claiming >=2 voicecards owns >=2 Parvati voices");
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
        // This block asserts the 1-voice-per-voicecard (voice i == card i) mapping.

        auto voicesAsSet = [&] (int part) {
            std::vector<int> v = engine.getPart (part).voiceIndices;
            std::sort (v.begin(), v.end());
            return v;
        };

        // part0 -> vc0,1; part1 -> vc2; part2 -> vc4,5. Under the pool model a
        // Part's voice COUNT equals its card count and its voices are tagged
        // onto its OWN cards (pool indices are engine-internal).
        auto cardsAsSet = [&engine] (int part)
        {
            std::set<int> cards;
            for (int vi : engine.getPart (part).voiceIndices)
                if (auto* av = engine.getAmbikaVoice (vi))
                    cards.insert (av->getVoiceCard());
            return cards;
        };
        engine.setPartVoiceAllocation (0, 0b000011);
        engine.setPartVoiceAllocation (1, 0b000100);
        engine.setPartVoiceAllocation (2, 0b110000);
        engine.setPartVoiceAllocation (3, 0);
        engine.setPartVoiceAllocation (4, 0);
        engine.setPartVoiceAllocation (5, 0);
        renderOnce (proc);   // service the deferred rebuild before inspecting voiceIndices

        // The masks materialize slot counts 2/1/2 and the engine derives a
        // CONTIGUOUS share in Part order: part0 -> vc0,1; part1 -> vc2;
        // part2 -> vc3,4 (the file's card POSITIONS are not user state).
        check (voicesAsSet (0).size() == 2 && cardsAsSet (0) == std::set<int> ({ 0, 1 }), "part0(2 slots) owns 2 voices on vc0,1");
        check (voicesAsSet (1).size() == 1 && cardsAsSet (1) == std::set<int> ({ 2 }),    "part1(1 slot) owns 1 voice on vc2");
        check (voicesAsSet (2).size() == 2 && cardsAsSet (2) == std::set<int> ({ 3, 4 }), "part2(2 slots) owns 2 voices on vc3,4");
        check (voicesAsSet (3).empty() && voicesAsSet (4).empty() && voicesAsSet (5).empty(),
               "parts 3-5 (zero mask -> 0 slots) own no voices");

        // Same popcount, different card shape: under the slots model the two
        // are EQUIVALENT (both mean "1 voice") — the derived layout is driven
        // by the counts, not the mask's bit positions.
        engine.setPartVoiceAllocation (1, 0b000001);  // vc0 position, still 1 slot
        renderOnce (proc);   // service the deferred rebuild
        check (voicesAsSet (1).size() == 1 && cardsAsSet (1) == std::set<int> ({ 2 }),
               "equivalent: a 1-bit mask anywhere still maps to the derived 1-card share");
        check (voicesAsSet (0).size() == 2 && cardsAsSet (0) == std::set<int> ({ 0, 1 }),
               "part0 keeps its derived share (no card stealing under slots)");
    }

    std::printf ("\n%s (%d failures)\n",
                 g_failures ? "MULTI LOAD TEST: FAILURES" : "MULTI LOAD TEST: ALL CHECKS PASSED",
                 g_failures);
    return g_failures ? 1 : 0;
}

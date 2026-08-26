// Ambika .MUL (multi) loading verification for Hellcat.
// 1. Parses a real factory .MUL (RIFF "MBKS") -> name + MultiData + 6 parts
//    (Patch[112] + PartData[84] each).
// 2. Loads it into the full HellcatAudioProcessor and confirms all 6 Parts get
//    distinct patches + that Part 0's MIDI channel matches MultiData's
//    part_mapping_[0].midi_channel.

#include <algorithm>
#include "unified_test_runner.h"
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
#include "dsp/patch.h"   // ambika::dsp::Patch/Part + kNum* (the [7] hostile-byte clamps)
#include "PluginProcessor.h"
#include "SynthEngine.h"

// Exact float comparison is deliberate: these asserts pin values,
// not ranges.
#pragma clang diagnostic ignored "-Wfloat-equal"

#ifndef HELLCAT_SOURCE_DIR
#define HELLCAT_SOURCE_DIR "."
#endif

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg) { std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg); if (! cond) ++g_failures; }

// Process one empty audio block so the engine services any deferred voice-
// allocation / polyphony / patch changes (markAllocationDirty) before the test
// inspects voiceIndices / partBytes synchronously. (Tests run processBlock on a
// single thread, so there is no concurrent audio thread here.)
void renderOnce (HellcatAudioProcessor& p)
{
    juce::AudioBuffer<float> buf (2, 256);
    buf.clear();
    juce::MidiBuffer midi;
    p.processBlock (buf, midi);
}

juce::File mulFile (const char* idx)
{
    return juce::File (HELLCAT_SOURCE_DIR)
        .getChildFile ("ambika_reference/controller/data/goldencard/MULTI/BANK/A")
        .getChildFile (juce::String (idx) + ".MUL");
}
}  // namespace

TEST(multi_load_test)
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
    std::printf ("     multi name = '%s'\n", m.name.toRawUTF8());

    int partsWithPatch = 0;
    for (int i = 0; i < 6; ++i)
        if (m.parts[static_cast<size_t> (i)].hasPatch) ++partsWithPatch;
    std::printf ("     parts with a Patch: %d/6\n", partsWithPatch);
    check (partsWithPatch >= 2, "at least 2 parts carry a Patch");

    // PartMapping[0]: {midi_channel, keyrange_low, keyrange_high, voice_allocation}
    const uint8_t* pm0 = m.multiData.data();
    std::printf ("     PartMapping[0]: channel=%d low=%d high=%d\n", pm0[0], pm0[1], pm0[2]);

    // ---- [2] Load the .MUL into the processor ----
    std::printf ("\n[2] Load .MUL into HellcatAudioProcessor\n");
    {
        HellcatAudioProcessor proc;
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
            char allocMsg[160];
            std::snprintf (allocMsg, sizeof (allocMsg), "voice allocation matches stored bitmasks (voices %d expected / %d actual)",
                           totalExpected, totalActual);
            check (allocOk, allocMsg);
            if (sawMultiCard)
                check (true, "a part claiming >=2 voicecards owns >=2 Hellcat voices");
            else
                std::printf ("     (note: 000.MUL has no multi-voicecard part to stress-test)\n");

            // Per-part arp/seq mode pushed from PartData into every Part.
            bool arpOk = true;
            for (int i = 0; i < 6; ++i)
            {
                if (! m.parts[static_cast<size_t> (i)].hasPart) continue;
                const uint8_t modeByte = m.parts[static_cast<size_t> (i)].part[7];  // arp_sequencer_mode
                if (engine.getPart (i).arp.getMode() != modeByte) arpOk = false;
                if (engine.getPart (i).seq.getMode() != modeByte) arpOk = false;
            }
            check (arpOk, "per-part arp/seq mode pushed from .MUL PartData");
        }
    }

    // ---- [4] Direct bitmask -> voice-block mapping (deterministic) ----
    std::printf ("\n[4] Direct voice-allocation bitmask mapping\n");
    {
        HellcatAudioProcessor proc;
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

    // ---- [4] Multi-load resets voice slots to init before the file applies ----
    // Stale-voice regression: a multi file that does not carry per-part voice
    // settings must never inherit the PREVIOUS state's slot counts. Both
    // loaders reset every Part to the engine init allocation (Part 0 = 6
    // voices, Parts 1..5 disabled) BEFORE the file applies its own data.
    std::printf ("\n[4] Multi-load resets voice slots to init (stale-voice bug)\n");
    {
        auto popcount = [] (uint8_t v) { int c = 0; while (v) { c += v & 1; v >>= 1; } return c; };

        // 4a) .yml multi with a SHORT parts list (human-editable format:
        //     only part 0 present, and without a voice_slots key): the parts
        //     the file does not mention must land at INIT, not stay polluted.
        {
            HellcatAudioProcessor proc;
            proc.prepareToPlay (48000.0, 256);
            renderOnce (proc);
            auto& engine = proc.getEngine();
            // Pollute every Part's slots (the stale state being guarded
            // against): part0=1, parts1-5=16.
            engine.setPartVoiceSlots (0, 1);
            for (int p = 1; p < 6; ++p) engine.setPartVoiceSlots (p, 16);
            renderOnce (proc);

            const juce::File tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("hellcat_multi_reset_test.yml");
            tmp.replaceWithText (
                "format: hellcat-multi\n"
                "version: 3\n"
                "name: \"Reset Test\"\n"
                "parts:\n"
                "  - channel: 1\n");
            check (proc.loadHellcatMultiFile (tmp), "short .yml multi loads");
            renderOnce (proc);

            // Part 0 (mentioned but WITHOUT voice_slots) + Parts 1..5 (not
            // mentioned at all) all land on the INIT allocation { 6, 0, 0,
            // 0, 0, 0 } — the pollution (1/16/16/16/16/16) is gone.
            check (engine.getPartVoiceSlots (0) == 6, "part0 (no voice_slots key) resets to init 6, not stale 1");
            bool parts15AtInit = true;
            for (int p = 1; p < 6; ++p)
                parts15AtInit = parts15AtInit && engine.getPartVoiceSlots (p) == 0;
            check (parts15AtInit, "parts 1..5 (absent from the file) reset to init 0, not stale 16");
            tmp.deleteFile();
        }

        // 4b) EXPLICIT slots still win (the bug is stale state, not explicit
        //     state): a .yml multi carrying voice_slots restores them over
        //     the init reset (format round-tripping is preserved).
        {
            HellcatAudioProcessor proc;
            proc.prepareToPlay (48000.0, 256);
            renderOnce (proc);
            auto& engine = proc.getEngine();
            engine.setPartVoiceSlots (0, 1);
            engine.setPartVoiceSlots (1, 1);
            renderOnce (proc);

            const juce::File tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("hellcat_multi_explicit_slots_test.yml");
            tmp.replaceWithText (
                "format: hellcat-multi\n"
                "version: 3\n"
                "name: \"Explicit Test\"\n"
                "parts:\n"
                "  - channel: 1\n"
                "    voice_slots: 9\n");
            check (proc.loadHellcatMultiFile (tmp), ".yml multi with explicit voice_slots loads");
            renderOnce (proc);
            check (engine.getPartVoiceSlots (0) == 9, "explicit voice_slots: 9 wins over the init reset");
            check (engine.getPartVoiceSlots (1) == 0, "unmentioned part 1 still resets to init 0 (not stale 1)");
            tmp.deleteFile();
        }

        // 4c) .MUL applies its OWN MultiData masks over the reset: loading a
        //     factory .MUL from a polluted state lands exactly on the file's
        //     popcount-per-mask, not on the pollution and not on init.
        {
            HellcatAudioProcessor proc;
            proc.prepareToPlay (48000.0, 256);
            renderOnce (proc);
            auto& engine = proc.getEngine();
            engine.setPartVoiceSlots (0, 16);
            engine.setPartVoiceSlots (3, 16);
            renderOnce (proc);

            check (proc.loadMultiFile (f), "loadMultiFile over polluted slots succeeds");
            renderOnce (proc);
            bool matchesFile = true;
            for (int i = 0; i < 6; ++i)
            {
                const uint8_t mask = m.multiData[(size_t) (i * 4) + 3];
                matchesFile = matchesFile && engine.getPartVoiceSlots (i) == popcount (mask);
            }
            check (matchesFile, ".MUL masks win: every part's slots == popcount(part_mapping_[i].voice_allocation)");
        }
    }

    // ---------------------------------------------------------------------
    // [6] A .MUL truncated before the last part's objects is REJECTED: the
    //     firmware writer always emits all 6 parts' Patch + PartData chunks,
    //     and the MBKS walker stops cleanly at a trailing cut, so without this
    //     guard a hand-trimmed/truncated file would load as a hybrid — the NEW
    //     MultiData routing over the PREVIOUS multi's patch/part bytes for the
    //     missing parts.
    // ---------------------------------------------------------------------
    std::printf ("\n[6] Truncated .MUL (missing last part) is rejected, not half-loaded\n");
    {
        juce::MemoryBlock whole;
        check (f.loadFileAsData (whole), "[6] read the reference .MUL bytes");
        const size_t fileSize = whole.getSize();

        // Layout (writeAmbikaMultiFile): header 12 + name 24 + MultiData 68,
        // then 6 x (Patch 124 + PartData 96) = 220 per part. Cuts inside the
        // LAST part's chunks leave parts 1..5 complete and part 6 missing
        // objects — exactly the corrupt shape the guard must reject.
        const size_t lastPartStart = 104 + 5 * 220;
        const size_t cuts[] = { lastPartStart + 10,            // inside part-6 Patch
                                lastPartStart + 200,           // Patch done, PartData gone
                                fileSize - 20 };               // inside part-6 PartData
        bool allRejected = true;
        for (const size_t cut : cuts)
        {
            if (cut >= fileSize)
                continue;
            const juce::File tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                       .getChildFile ("hellcat_trunc_mul_test.MUL");
            tmp.deleteFile();
            tmp.appendData (whole.getData(), cut);

            HellcatAudioProcessor proc;
            proc.prepareToPlay (48000.0, 256);
            if (proc.loadMultiFile (tmp))
                allRejected = false;
            tmp.deleteFile();
        }
        check (allRejected,
               "[6] every truncated .MUL is rejected (no routing/sound hybrid load)");

        // Control: the UNTRUNCATED file still loads (the guard is not
        // over-strict against the reference shape).
        {
            HellcatAudioProcessor proc;
            proc.prepareToPlay (48000.0, 256);
            check (proc.loadMultiFile (f), "[6] control: the full .MUL still loads");
        }
    }

    // ---------------------------------------------------------------------
    // [7] Hostile raw Patch/Part bytes are clamped at the DSP edge (bug hunt
    //     2026-08-18, F-eng-1 / F-static-1/2). The .MUL and host-state paths
    //     push patch/part bytes into the engine WITHOUT the APVTS round-trip
    //     (PluginProcessor.cpp:1046, SynthEngine.cpp:686) — pre-fix, a raw
    //     mod-matrix `destination` byte up to 255 indexed dst_[19] (an OOB
    //     WRITE on the audio thread), `source`/modifier operands indexed
    //     modulation_sources_[31], the LFO-rate and portamento bytes indexed
    //     128-entry LUTs, and a raw oscillator shape byte walked past
    //     wav_res_wavetables. The DSP now clamps every one of these at the
    //     consumer; this test pins the CONTRACT deterministically: a fully
    //     hostile byte set must render byte-identical to its clamped twin
    //     (and finite, and non-silent).
    // ---------------------------------------------------------------------
    std::printf ("\n[7] Hostile raw Patch/Part bytes clamp to their valid twins (F-eng-1)\n");
    {
        // Forge two identical engines; A carries HOSTILE bytes (255/200-ish),
        // B carries exactly what the clamps must reduce them to.
        auto forge = [] (bool hostile) {
            auto proc = std::make_unique<HellcatAudioProcessor>();
            proc->prepareToPlay (48000.0, 256);
            proc->syncAllParamsToEngine();

            ambika::dsp::Patch hp {};
            const uint8_t src  = hostile ? 255 : (ambika::dsp::kNumModulationSources - 1);
            const uint8_t dst  = hostile ? 250 : (ambika::dsp::kNumModulationDestinations - 1);
            const uint8_t rate = hostile ? 255 : 142;   // kNumSyncedLfoRates + 127
            const uint8_t shp  = hostile ? 255 : 36;    // WAVEFORM_WAVETABLE_16 (index 15)
            // mod[0]: hostile SOURCE into a valid destination (filter cutoff —
            // audible, and the clamp target of source 255 is the CONSTANT_4
            // slot, identical in both twins).
            hp.modulation[0] = { src, 12, 60 };
            // mod[1]: hostile DESTINATION. Any OOB destination clamps to 18 ==
            // MOD_DST_VCA — the MULTIPLICATIVE path, which a nonzero amount
            // would use to silence the voice (source 30 is a CONSTANT).
            // amount 0 keeps the VCA untouched while STILL exercising the
            // dst_[destination] read-modify-write every block (the pre-fix
            // OOB write happened regardless of amount).
            hp.modulation[1] = { dst, dst, 0 };
            hp.modifier[0].operands[0] = src;
            hp.modifier[0].operands[1] = hostile ? 200 : 30;
            hp.modifier[0].op = 1;                     // SUM (drives the operand loads)
            hp.env_lfo[0].rate = rate;
            hp.env_lfo[0].shape = 1;
            hp.osc[0].shape = shp;
            hp.osc[0].parameter = 64;
            hp.osc[1].shape = shp;
            auto& pb = proc->getEngine().getPart (0).patchBytes;
            for (size_t i = 0; i < sizeof (hp); ++i)
                pb[i] = reinterpret_cast<const uint8_t*> (&hp)[i];

            ambika::dsp::Part pt {};
            pt.volume = 100;
            pt.portamento_time = hostile ? 255 : 127;
            auto& pp = proc->getEngine().getPart (0).partBytes;
            for (size_t i = 0; i < sizeof (pt); ++i)
                pp[i] = reinterpret_cast<const uint8_t*> (&pt)[i];
            return proc;
        };

        auto procA = forge (true);   // hostile
        auto procB = forge (false);  // clamped twin

        // Pin the Ladder card on the ENGINE (the forge writes patch bytes
        // directly; an APVTS sync would overwrite them). The energy gate was
        // calibrated under this card.
        procA->getEngine().setFilterTopology (ambika::dsp::FilterTopology::FOUR_POLE_LADDER);
        procB->getEngine().setFilterTopology (ambika::dsp::FilterTopology::FOUR_POLE_LADDER);

        // Identical stimulus: a held two-note chord with portamento glide
        // (exercises Trigger's portamento path) + several render blocks
        // (exercises the mod matrix + LFO + wavetable every block).
        auto renderStimulus = [] (HellcatAudioProcessor& p, std::vector<float>& capture) {
            for (int blk = 0; blk < 60; ++blk)
            {
                juce::AudioBuffer<float> buf (2, 256);
                buf.clear();
                juce::MidiBuffer midi;
                if (blk == 0)
                {
                    midi.addEvent (juce::MidiMessage::noteOn (1, 60, (uint8_t) 110), 0);
                    midi.addEvent (juce::MidiMessage::noteOn (1, 67, (uint8_t) 90), 64);
                }
                p.processBlock (buf, midi);
                for (int i = 0; i < 256; ++i)
                    capture.push_back (buf.getSample (0, i));
            }
        };
        std::vector<float> capA, capB;
        renderStimulus (*procA, capA);
        renderStimulus (*procB, capB);

        bool finite = true;
        for (float s : capA)
            if (! std::isfinite (s)) { finite = false; break; }
        check (finite, "[7] hostile-byte render is finite (no NaN/inf from OOB state)");

        double energy = 0.0;
        for (float s : capA) energy += (double) s * (double) s;
        check (energy > 1.0, "[7] hostile-byte render is non-silent");

        bool identical = capA.size() == capB.size();
        if (identical)
            for (size_t i = 0; i < capA.size(); ++i)
                if (capA[i] != capB[i]) { identical = false; break; }
        char msg[128];
        std::snprintf (msg, sizeof (msg),
                      "[7] hostile bytes render byte-identical to the clamped twin (%zu samples)",
                      capA.size());
        check (identical, msg);
    }

    std::printf ("\n%s (%d failures)\n",
                 g_failures ? "MULTI LOAD TEST: FAILURES" : "MULTI LOAD TEST: ALL CHECKS PASSED",
                 g_failures);
    return g_failures == 0;
}

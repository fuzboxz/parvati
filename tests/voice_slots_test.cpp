// Voice-slots (Parvati extension) test — verifies the SLOTS MODEL on top of
// the faithful 6-voicecard engine:
//
//   * voiceSlots is the SINGLE SOURCE OF TRUTH: each Part has 1..16 voices
//     from the 96-voice pool (1 voice = digital voice + voicecard); the
//     6-card bitmask is DERIVED (contiguous proportional share, minimum one
//     card per active Part) and keeps only its aux-out routing + .MUL export
//     jobs.
//   * Default: Part 0 materializes 6 voices (the faithful 6-voice Ambika);
//     the other Parts are disabled (0 slots).
//   * Fixed slots: a Part draws its slot count from the pool; every Part can
//     be maxed simultaneously (pool = kNumParts * kMaxVoicesPerPart).
//   * Slots alone enable a Part — the old "no cards = disabled" gate is gone;
//     0 slots (set only by the ctor default / legacy loaders) disables.
//   * MONO fires every allocated VOICE: unison size = the Part's voice count
//     (MONO + 1 voice is true single-voice mono, MONO + 16 is 16-voice
//     unison).
//   * Host engine-state round-trip: voiceSlots + part names survive
//     capture/restore; a legacy v5-sized blob (no v6 tail) materializes its
//     slot counts from the blob bitmasks (popcount).
//
// Harness mirrors polyphony_test: a ParvatiAudioProcessor, edits via the
// engine API, notes on MIDI channel 1, active-voice inspection on the engine.

#include <algorithm>
#include <cstdio>
#include <set>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "ParvatiPreset.h"
#include "PluginProcessor.h"
#include "SynthEngine.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg) { std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg); if (! cond) ++g_failures; }

void renderIdle (ParvatiAudioProcessor& p, int blocks)
{
    for (int i = 0; i < blocks; ++i)
    {
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer midi;
        p.processBlock (buf, midi);
    }
}

void noteEvent (ParvatiAudioProcessor& p, const juce::MidiMessage& m)
{
    juce::AudioBuffer<float> buf (2, 256);
    buf.clear();
    juce::MidiBuffer midi;
    midi.addEvent (m, 0);
    p.processBlock (buf, midi);
}

// Count the engine's currently-active voices, optionally restricted to a Part.
int activeVoices (SynthEngine& engine, int part = -1)
{
    int n = 0;
    for (int i = 0; i < kNumVoices; ++i)
        if (auto* av = engine.getAmbikaVoice (i))
            if (av->isVoiceActive() && (part < 0 || av->getPartIndex() == part))
                ++n;
    return n;
}
}  // namespace

int main()
{
    std::printf ("VOICE SLOTS TEST\n");

    // ---- [a] Default = Part 0 with 6 materialized voices ----
    {
        std::printf ("\n[a] default: part 0 owns 6 voices (faithful hardware)\n");
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        renderIdle (proc, 2);
        SynthEngine& engine = proc.getEngine();
        check (engine.getPartVoiceSlots (0) == 6, "default voice_slots = 6 (materialized from the init bitmask)");
        check ((int) engine.getPart (0).voiceIndices.size() == 6, "part 0 owns six voices (faithful 6-voice Ambika)");
        check (engine.getPartVoiceAllocation (0) == 0x3f, "derived mask: the single active part owns all 6 cards");
        for (int i = 1; i < kNumParts; ++i)
            check (engine.getPart (i).voiceIndices.empty(), "parts 1..5 disabled (0 slots -> 0 voices)");

        // 6 distinct notes sustain on part 0 (hardware parity).
        for (int n = 60; n < 66; ++n)
            noteEvent (proc, juce::MidiMessage::noteOn (1, n, 0.8f));
        renderIdle (proc, 2);
        check (activeVoices (engine, 0) == 6, "6 held notes -> 6 active voices (hardware parity)");
        for (int n = 60; n < 66; ++n)
            noteEvent (proc, juce::MidiMessage::noteOff (1, n, 0.8f));
        renderIdle (proc, 200);
        check (activeVoices (engine) == 0, "all voices freed after release");
    }

    // ---- [b] Fixed slots: polyphony beyond the card count, from the pool ----
    {
        std::printf ("\n[b] fixed slots: part 0 (6 cards) at 16 slots sustains 10 notes\n");
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        renderIdle (proc, 2);
        SynthEngine& engine = proc.getEngine();
        engine.setPartVoiceSlots (0, 16);
        renderIdle (proc, 2);   // service the deferred pool re-partition
        check ((int) engine.getPart (0).voiceIndices.size() == 16, "slots=16 -> 16 pool voices for part 0");

        for (int n = 60; n < 70; ++n)
            noteEvent (proc, juce::MidiMessage::noteOn (1, n, 0.8f));
        renderIdle (proc, 2);
        check (activeVoices (engine, 0) == 10, "10 held notes -> 10 active voices (beyond the 6-card limit)");
        for (int n = 60; n < 70; ++n)
            noteEvent (proc, juce::MidiMessage::noteOff (1, n, 0.8f));
        renderIdle (proc, 200);
    }

    // ---- [c] Every Part maxed simultaneously (pool never steals) ----
    {
        std::printf ("\n[c] all 6 parts at 16 slots, one card each\n");
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        renderIdle (proc, 2);
        SynthEngine& engine = proc.getEngine();
        for (int p = 0; p < kNumParts; ++p)
        {
            engine.setPartVoiceAllocation (p, static_cast<uint8_t> (1u << p));
            engine.setPartMidiChannel (p, p + 1);
            engine.setPartVoiceSlots (p, 16);
        }
        renderIdle (proc, 2);
        for (int p = 0; p < kNumParts; ++p)
            check ((int) engine.getPart (p).voiceIndices.size() == 16, "part owns 16 pool voices (all parts maxed)");

        // 8 notes on part 0 + 8 on part 1 simultaneously.
        for (int n = 60; n < 68; ++n) noteEvent (proc, juce::MidiMessage::noteOn (1, n, 0.8f));
        for (int n = 60; n < 68; ++n) noteEvent (proc, juce::MidiMessage::noteOn (2, n, 0.8f));
        renderIdle (proc, 2);
        check (activeVoices (engine, 0) == 8, "part 0 sustains 8 notes");
        check (activeVoices (engine, 1) == 8, "part 1 sustains 8 notes concurrently");
    }

    // ---- [d] Slots alone enable a Part (the card gate is gone) ----
    {
        std::printf ("\n[d] slots activate a part with no legacy card assignment\n");
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        renderIdle (proc, 2);
        SynthEngine& engine = proc.getEngine();
        engine.setPartVoiceSlots (1, 8);      // part 1 was disabled (0 slots)...
        engine.setPartMidiChannel (1, 2);
        renderIdle (proc, 2);
        check ((int) engine.getPart (1).voiceIndices.size() == 8, "slots=8 alone -> 8 pool voices for part 1");
        check (engine.getPartVoiceAllocation (1) != 0, "derived mask: part 1 holds >= 1 card (min-one rule)");
        check ((int) engine.getPart (0).voiceIndices.size() == 6, "part 0 unaffected (its own 6 slots)");
        // 0 slots via the legacy path disables; the PUBLIC setter clamps 0 -> 1.
        engine.setPartVoiceAllocation (1, 0);
        renderIdle (proc, 2);
        check (engine.getPart (1).voiceIndices.empty(), "legacy zero mask -> 0 slots -> disabled");
        engine.setPartVoiceSlots (1, 0);
        renderIdle (proc, 2);
        check (engine.getPartVoiceSlots (1) == 1, "public setter clamps 0 -> 1 (cannot disable)");
    }

    // ---- [e] Aux routing: a Part's voices spread round-robin over ITS DERIVED cards ----
    {
        std::printf ("\n[e] derived card tagging (round-robin over the derived share)\n");
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        renderIdle (proc, 2);
        SynthEngine& engine = proc.getEngine();
        // Two parts, 8 slots each: the derived share is a contiguous 3+3
        // split (largest-remainder of 6 cards by 8/8).
        engine.setPartVoiceSlots (0, 8);
        engine.setPartVoiceSlots (1, 8);
        renderIdle (proc, 2);
        check (engine.getPartVoiceAllocation (0) == 0b000111 && engine.getPartVoiceAllocation (1) == 0b111000,
               "derived masks: contiguous 3+3 proportional split of the 6 cards");
        for (int p = 0; p < 2; ++p)
        {
            const auto& vi = engine.getPart (p).voiceIndices;
            check (vi.size() == 8, "part owns 8 pool voices");
            std::set<int> cards;
            for (int v : vi)
                if (auto* av = engine.getAmbikaVoice (v))
                    cards.insert (av->getVoiceCard());
            const int firstCard = p == 0 ? 0 : 3;
            bool ownOnly = ! cards.empty();
            for (int c : cards)
                if (c < firstCard || c >= firstCard + 3) ownOnly = false;
            check (ownOnly && cards.size() == 3, "voices tagged round-robin onto the part's own derived cards");
        }
    }

    // ---- [f] MONO unison size = the Part's voice count ----
    {
        std::printf ("\n[f] MONO fires every allocated voice\n");
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        renderIdle (proc, 2);
        SynthEngine& engine = proc.getEngine();

        proc.getApvts().getParameterAsValue ("part_polyphony") = 0.0f;   // MONO
        engine.setPartVoiceSlots (0, 16);
        renderIdle (proc, 4);               // polyphony byte -> allocation service
        noteEvent (proc, juce::MidiMessage::noteOn (1, 60, 0.8f));
        renderIdle (proc, 2);
        check (activeVoices (engine, 0) == 16, "MONO + 16 voices -> 16-voice unison (every allocated voice)");

        engine.setPartVoiceSlots (0, 6);    // the faithful 6-voice unison
        renderIdle (proc, 4);
        noteEvent (proc, juce::MidiMessage::noteOff (1, 60, 0.8f));
        renderIdle (proc, 200);
        noteEvent (proc, juce::MidiMessage::noteOn (1, 62, 0.8f));
        renderIdle (proc, 2);
        check (activeVoices (engine, 0) == 6, "MONO + 6 voices -> 6-voice unison (hardware parity)");

        // MONO + a single voice = true single-voice mono (legacy 1-card mask
        // materializes 1 slot).
        engine.setPartVoiceAllocation (0, 0x01);
        renderIdle (proc, 4);
        noteEvent (proc, juce::MidiMessage::noteOff (1, 62, 0.8f));
        renderIdle (proc, 200);
        noteEvent (proc, juce::MidiMessage::noteOn (1, 64, 0.8f));
        renderIdle (proc, 2);
        check (activeVoices (engine, 0) == 1, "MONO + 1 voice -> exactly 1 sounding voice (true mono)");
    }

    // ---- [g] Host engine-state v6 round-trip: slots + names ----
    {
        std::printf ("\n[g] engine-state capture/restore round-trips slots + names\n");
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        renderIdle (proc, 2);
        SynthEngine& engine = proc.getEngine();
        engine.setPartVoiceAllocation (0, 0x3f);
        engine.setPartVoiceSlots (0, 9);
        engine.setPartName (0, "Lead");
        engine.setPartName (3, "Pad");
        juce::MemoryBlock blob;
        engine.captureState (blob);

        ParvatiAudioProcessor other;
        other.prepareToPlay (48000.0, 256);
        renderIdle (other, 2);
        check (other.getEngine().restoreState (blob.getData(), blob.getSize()), "v6 blob restores");
        check (other.getEngine().getPartVoiceSlots (0) == 9, "voice_slots round-trips");
        check (other.getEngine().getPartName (0) == "Lead", "part name round-trips");
        check (other.getEngine().getPartName (3) == "Pad", "second name round-trips");
        check (other.getEngine().getPartDisplayName (1) == "Part 2", "unnamed part displays \"Part N\"");
        renderIdle (other, 2);
        check ((int) other.getEngine().getPart (0).voiceIndices.size() == 9, "restored slots re-partition the pool");
    }

    // ---- [h] Legacy v5 blob (no v6 tail) restores with AUTO + empty names ----
    {
        std::printf ("\n[h] legacy v5 blob -> AUTO slots, empty names\n");
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        renderIdle (proc, 2);
        juce::MemoryBlock blob;
        proc.getEngine().captureState (blob);

        // Build a legacy v5 blob: strip each part's 2-byte v6 tail (slots byte
        // + empty-name length byte) and the v7-ONLY tuning block (4-byte length
        // prefix + {mode; offsets[12]}) if present, then patch the version byte
        // back to 5. The per-part layout is DISCOVERED from the capture's
        // version header (v8 REMOVED the tuning block, 2026-08-19) — the old
        // hard-coded v7 math overran the capture buffer on a v8 blob (heap OOB
        // read, ASLR-dependent segv). A size sanity check guards the cursor
        // arithmetic before any memcpy.
        const size_t full = blob.getSize();
        constexpr size_t kPartCore = 112 + 84 + 4 + 4 + 78;   // patch+part+routing+fxprefix+fx
        const int capVersion = ((const uint8_t*) blob.getData())[4];
        const size_t v7Tuning = (capVersion == 7) ? (size_t) (4 + 25) : 0;
        check (capVersion >= 6 && capVersion <= 8 && full >= 6 + kNumParts * (kPartCore + 2 + v7Tuning),
               "capture is a known slots-era blob sized for the v5 strip (v8)");
        juce::MemoryBlock v5 (full - (size_t) (kNumParts * (2 + v7Tuning)), true);
        const uint8_t* src = (const uint8_t*) blob.getData();
        uint8_t* dst = (uint8_t*) v5.getData();
        size_t r = 6, w = 6;   // read/write cursors (past the 6-byte header)
        std::memcpy (dst, src, 6);   // header (magic + version + currentPart)
        for (int p = 0; p < kNumParts; ++p)
        {
            std::memcpy (dst + w, src + r, kPartCore);
            r += kPartCore + 2 + v7Tuning;   // skip the v6 tail (+ the v7 tuning block if present)
            w += kPartCore;
        }
        dst[4] = 5;
        ParvatiAudioProcessor other;
        other.prepareToPlay (48000.0, 256);
        renderIdle (other, 2);
        check (other.getEngine().restoreState (v5.getData(), v5.getSize()), "legacy v5 blob restores");
        check (other.getEngine().getPartVoiceSlots (0) == 6, "legacy blob materializes slots from its bitmask (6)");
        check (other.getEngine().getPartName (0).isEmpty(), "legacy blob -> empty name");
    }

    // ---- [i] .parvati multi round-trips slots + names ----
    {
        std::printf ("\n[i] .parvati multi round-trips slots + names\n");
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        renderIdle (proc, 2);
        SynthEngine& engine = proc.getEngine();
        engine.setPartVoiceSlots (0, 12);
        engine.setPartName (0, "Kick");
        const juce::String yaml = parvati::preset::serializeParvatiMulti (proc);
        check (yaml.contains ("voice_slots: 12"), "serialized multi carries voice_slots");
        check (yaml.contains ("name: \"Kick\""), "serialized multi carries the part name");

        ParvatiAudioProcessor other;
        other.prepareToPlay (48000.0, 256);
        renderIdle (other, 2);
        check (parvati::preset::applyParvatiMulti (other, yaml), "multi re-applies");
        check (other.getEngine().getPartVoiceSlots (0) == 12, "voice_slots round-trips through the multi format");
        check (other.getEngine().getPartName (0) == "Kick", "part name round-trips through the multi format");
        renderIdle (other, 2);
        check ((int) other.getEngine().getPart (0).voiceIndices.size() == 12, "applied slots re-partition the pool");
    }

    std::printf ("\nVOICE SLOTS TEST: %s (%d failures)\n",
                 g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}

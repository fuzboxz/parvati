// Voice-slots (Parvati extension) test — verifies the per-part voice-slot
// pool model on top of the faithful 6-voicecard engine:
//
//   * Default (AUTO slots): one voice per allocated card — the pre-extension
//     6-voice hardware behaviour, bit-for-bit.
//   * Fixed slots: a Part draws its slot count from the 96-voice pool; every
//     Part can be maxed simultaneously (pool = kNumParts * kMaxVoicesPerPart).
//   * Card bitmask keeps ownership / aux-out routing: a Part's voices are
//     tagged round-robin across ITS cards; a Part with no cards is disabled
//     regardless of slots.
//   * Per-CARD mono: MONO fires exactly one voice per allocated card, so the
//     unison size (and CPU) is invariant under the slots setting, and
//     MONO + 1 card is true single-voice mono.
//   * Host engine-state round-trip (v6): voiceSlots + part names survive
//     capture/restore; a legacy v5-sized blob (no v6 tail) still restores
//     with AUTO slots + empty names.
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

    // ---- [a] Default = AUTO slots = faithful 6-voice hardware ----
    {
        std::printf ("\n[a] default AUTO: part 0 (all 6 cards) owns 6 voices\n");
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        renderIdle (proc, 2);
        SynthEngine& engine = proc.getEngine();
        check (engine.getPartVoiceSlots (0) == 0, "default voice_slots = 0 (AUTO)");
        check ((int) engine.getPart (0).voiceIndices.size() == 6, "AUTO gives part 0 six voices (card count)");
        for (int i = 1; i < kNumParts; ++i)
            check (engine.getPart (i).voiceIndices.empty(), "AUTO: card-less part disabled (0 voices)");

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

    // ---- [d] Slots never override ownership: no cards -> disabled ----
    {
        std::printf ("\n[d] slots do not bypass the card bitmask\n");
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        renderIdle (proc, 2);
        SynthEngine& engine = proc.getEngine();
        engine.setPartVoiceAllocation (0, 0x3f);
        engine.setPartVoiceSlots (1, 8);      // part 1 has NO cards...
        renderIdle (proc, 2);
        check ((int) engine.getPart (1).voiceIndices.empty(), "card-less part stays disabled despite slots=8");
        check ((int) engine.getPart (0).voiceIndices.size() == 6, "part 0 unaffected (AUTO, 6 cards)");
    }

    // ---- [e] Aux routing: a Part's voices spread round-robin over ITS cards ----
    {
        std::printf ("\n[e] per-part card tagging (round-robin over owned cards)\n");
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        renderIdle (proc, 2);
        SynthEngine& engine = proc.getEngine();
        engine.setPartVoiceAllocation (0, 0b000011);   // cards 0+1
        engine.setPartVoiceSlots (0, 5);
        renderIdle (proc, 2);
        const auto& vi = engine.getPart (0).voiceIndices;
        check (vi.size() == 5, "5 pool voices for the 2-card part");
        std::set<int> cards;
        for (int v : vi)
            if (auto* av = engine.getAmbikaVoice (v))
                cards.insert (av->getVoiceCard());
        check (cards.count (0) == 1 && cards.count (1) == 1, "voices render onto the part's own cards (0 and 1)");
        check (cards.size() == 2, "no voice tagged to a foreign card");
    }

    // ---- [f] Per-CARD mono: unison size = card count, slots-invariant ----
    {
        std::printf ("\n[f] MONO fires one voice per card regardless of slots\n");
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        renderIdle (proc, 2);
        SynthEngine& engine = proc.getEngine();

        proc.getApvts().getParameterAsValue ("part_polyphony") = 0.0f;   // MONO
        engine.setPartVoiceSlots (0, 16);   // 6 cards, 16 slots
        renderIdle (proc, 4);               // polyphony byte -> allocation service
        noteEvent (proc, juce::MidiMessage::noteOn (1, 60, 0.8f));
        renderIdle (proc, 2);
        check (activeVoices (engine, 0) == 6, "MONO + 6 cards + slots=16 -> exactly 6 sounding voices (one per card)");

        engine.setPartVoiceSlots (0, 0);    // back to AUTO
        renderIdle (proc, 4);
        noteEvent (proc, juce::MidiMessage::noteOff (1, 60, 0.8f));
        renderIdle (proc, 200);
        noteEvent (proc, juce::MidiMessage::noteOn (1, 62, 0.8f));
        renderIdle (proc, 2);
        check (activeVoices (engine, 0) == 6, "MONO + 6 cards + AUTO -> same 6-voice unison (capacity-invariant)");

        // MONO + a single card = true single-voice mono.
        engine.setPartVoiceAllocation (0, 0x01);
        renderIdle (proc, 4);
        noteEvent (proc, juce::MidiMessage::noteOff (1, 62, 0.8f));
        renderIdle (proc, 200);
        noteEvent (proc, juce::MidiMessage::noteOn (1, 64, 0.8f));
        renderIdle (proc, 2);
        check (activeVoices (engine, 0) == 1, "MONO + 1 card -> exactly 1 sounding voice (true mono)");
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
        // + empty-name length byte) AND the 29-byte v7 tuning block (4-byte
        // length prefix + {mode; offsets[12]}), then patch the version byte
        // back to 5.
        const size_t full = blob.getSize();
        constexpr size_t kPartV6 = 112 + 84 + 4 + 4 + 78 + 2;   // patch+part+routing+fxprefix+fx+v6 tail
        constexpr size_t kPartV7Extra = 4 + 25;                 // tuning block length prefix + payload
        juce::MemoryBlock v5 (full - (size_t) (kNumParts * (2 + kPartV7Extra)), true);
        const uint8_t* src = (const uint8_t*) blob.getData();
        uint8_t* dst = (uint8_t*) v5.getData();
        size_t r = 6, w = 6;   // read/write cursors (past the 6-byte header)
        std::memcpy (dst, src, 6);   // header (magic + version + currentPart)
        for (int p = 0; p < kNumParts; ++p)
        {
            std::memcpy (dst + w, src + r, kPartV6 - 2);
            r += kPartV6 + kPartV7Extra;   // skip the v6 tail AND the v7 tuning block
            w += kPartV6 - 2;
        }
        dst[4] = 5;
        ParvatiAudioProcessor other;
        other.prepareToPlay (48000.0, 256);
        renderIdle (other, 2);
        check (other.getEngine().restoreState (v5.getData(), v5.getSize()), "legacy v5 blob restores");
        check (other.getEngine().getPartVoiceSlots (0) == 0, "legacy blob -> AUTO slots (faithful hardware)");
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

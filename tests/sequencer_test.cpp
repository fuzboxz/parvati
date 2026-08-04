// Step-sequencer verification for Parvati (Ambika port).
//  (1) NOTE sequence mode: arp_mode=Sequencer + a note sequence -> the engine
//      emits sequenced notes (>= 2 distinct pitches over time, transposed by
//      the held key).
//  (2) MODULATION sequences: SEQ_1 reaches the voices (engine injects the
//      sequence value into every voice's modulation_sources_[MOD_SRC_SEQ_1]
//      each block; value varies as steps advance).
//
// Run: cmake --build build --target parvati_sequencer_test && ./build/parvati_sequencer_test

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <set>
#include <unordered_set>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "dsp/patch.h"
#include "PluginProcessor.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg) { std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg); if (! cond) ++g_failures; }

class FakePlayHead : public juce::AudioPlayHead
{
public:
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm (120.0);
        info.setIsPlaying (true);
        info.setTimeInSamples (static_cast<int64_t> (0));
        return info;
    }
};
}  // namespace

int main()
{
    juce::MessageManager::getInstance();
    juce::ScopedJuceInitialiser_GUI guiInit;

    // ---------- (1) NOTE sequence: distinct pitches ----------
    std::printf ("[1] Note sequence emits distinct pitches\n");
    {
        ParvatiAudioProcessor processor;
        FakePlayHead playHead;
        processor.setPlayHead (&playHead);
        processor.prepareToPlay (48000.0, 256);
        processor.syncAllParamsToEngine();

        auto set = [&] (const char* id, float v) { processor.getApvts().getParameterAsValue (id) = v; };
        set ("arp_mode", 2.0f);        // Sequencer (NOTE) mode
        set ("seq_length_3", 4.0f);    // 4-step note sequence
        set ("seqnote_step0", 188.0f);  // C3  (60 | 0x80 = gated)
        set ("seqnote_step1", 192.0f);  // E3  (64 | 0x80)
        set ("seqnote_step2", 195.0f);  // G3  (67 | 0x80)
        set ("seqnote_step3", 200.0f);  // C4  (72 | 0x80)

        // Hold C3 (48) -> transpose base. note = seqNote + 48 - 60.
        {
            juce::AudioBuffer<float> buf (2, 256); buf.clear();
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 48, (uint8_t) 100), 0);
            processor.processBlock (buf, midi);
        }

        constexpr int kBlock = 256, kNumBlocks = 562;
        // Collect the distinct NOTES the sequence drives onto voices (direct,
        // robust under multitimbral — release tails would smear audio-ZCR).
        std::set<int> seqNotes;
        double peak = 0.0;
        for (int b = 0; b < kNumBlocks; ++b)
        {
            juce::AudioBuffer<float> buf (2, kBlock); buf.clear();
            juce::MidiBuffer empty;
            processor.processBlock (buf, empty);
            for (int i = 0; i < kNumVoices; ++i)
                if (auto* v = processor.getEngine().getAmbikaVoice (i))
                    if (v->getCurrentlyPlayingNote() >= 0)
                        seqNotes.insert (v->getCurrentlyPlayingNote());
            for (int i = 0; i < kBlock; ++i) peak = std::max (peak, (double) std::fabs (buf.getSample (0, i)));
        }
        std::printf ("     distinct sequencer notes on voices: %zu (", seqNotes.size());
        for (int n : seqNotes) std::printf (" %d", n);
        std::printf (")  audio peak: %.4f\n", peak);
        check (seqNotes.size() >= 2, "note sequence produces >= 2 distinct pitches");
        check (peak > 0.001, "note sequence produces audible audio");
    }

    // ---------- (2) SEQ_1 modulation reaches a voice ----------
    std::printf ("\n[2] SEQ_1 modulation reaches the voices\n");
    {
        ParvatiAudioProcessor processor;
        FakePlayHead playHead;
        processor.setPlayHead (&playHead);
        processor.prepareToPlay (48000.0, 256);
        processor.syncAllParamsToEngine();

        auto set = [&] (const char* id, float v) { processor.getApvts().getParameterAsValue (id) = v; };
        set ("arp_mode", 2.0f);        // active (Sequencer) so the clock advances
        set ("seq_length_1", 4.0f);    // 4-step modulation sequence
        set ("seq1_step0", 0.0f);
        set ("seq1_step1", 127.0f);
        set ("seq1_step2", 0.0f);
        set ("seq1_step3", 127.0f);

        std::unordered_set<int> seen;
        constexpr int kBlock = 256, kNumBlocks = 200;
        for (int b = 0; b < kNumBlocks; ++b)
        {
            juce::AudioBuffer<float> buf (2, kBlock); buf.clear();
            juce::MidiBuffer empty;
            processor.processBlock (buf, empty);
            if (auto* v = processor.getEngine().getAmbikaVoice (0))
                seen.insert (static_cast<int> (v->getModulationSource (ambika::dsp::MOD_SRC_SEQ_1)));
        }
        std::printf ("     distinct SEQ_1 values seen at voice 0: %zu (", seen.size());
        for (int x : seen) std::printf (" %d", x);
        std::printf (" )\n");
        check (seen.size() >= 2, "SEQ_1 modulation reaches a voice with >= 2 distinct values");
    }

    std::printf ("\n%s (%d failures)\n",
                 g_failures ? "SEQUENCER TEST: FAILURES" : "SEQUENCER TEST: ALL CHECKS PASSED",
                 g_failures);
    return g_failures ? 1 : 0;
}

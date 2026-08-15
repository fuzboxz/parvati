// Arpeggiator verification: proves the host-tempo-driven arpeggiator generates
// multiple distinct note pitches over time when the transport is playing and
// keys are held, and that note-offs occur between steps.
//
// Run: cmake --build build --target parvati_arp_test && ./build/parvati_arp_test

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <unordered_map>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>  // ScopedJuceInitialiser_GUI

#include "ParameterLayout.h"
#include "PluginProcessor.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

// A minimal AudioPlayHead that reports a fixed BPM + playing state.
class FakePlayHead : public juce::AudioPlayHead
{
public:
    FakePlayHead (double bpm, bool playing) : bpm_ (bpm), playing_ (playing) {}

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm (bpm_);
        info.setIsPlaying (playing_);
        info.setTimeInSamples (static_cast<int64_t> (0));
        return info;
    }

private:
    double bpm_;
    bool playing_;
};
}  // namespace

int main()
{
    juce::MessageManager::getInstance();
    juce::ScopedJuceInitialiser_GUI guiInit;

    ParvatiAudioProcessor processor;

    // Provide a fake play head at 120 BPM, playing.
    FakePlayHead playHead (120.0, true);
    processor.setPlayHead (&playHead);

    processor.prepareToPlay (48000.0, 256);
    processor.syncAllParamsToEngine();

    // Configure the arpeggiator: mode=Arp (index 1), direction=Up, octave=2, resolution=1/16.
    processor.getApvts().getParameterAsValue ("arp_mode") = 1.0f;       // Arp (index 1 of Off/Arp/Sequencer)
    processor.getApvts().getParameter ("arp_direction")->setValueNotifyingHost (0.0f);  // Up
    processor.getApvts().getParameter ("arp_octave")->setValueNotifyingHost (          // 2 octaves
        juce::jmap (2.0f, 1.0f, 4.0f, 0.0f, 1.0f));
    processor.getApvts().getParameter ("arp_resolution")->setValueNotifyingHost (       // 1/16
        juce::jmap (6.0f, 0.0f, 14.0f, 0.0f, 1.0f));
    processor.getApvts().getParameter ("arp_pattern")->setValueNotifyingHost (0.0f);    // pattern 0

    // Feed a MIDI NoteOn (C3 = 48, vel 100).
    {
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 48, (uint8_t) 100), 0);
        processor.processBlock (buf, midi);
    }

    // Render ~3 seconds (at 48kHz, 256-sample blocks => ~562 blocks).
    // At 120 BPM with 1/16 resolution: 24 ticks per step, 1000 samples/tick
    // => 24000 samples per step => ~0.5s/step => ~6 arp steps in 3 seconds.
    // With octave=2 the arp cycles C3..C4 across octaves.
    constexpr int kBlock = 256;
    constexpr int kNumBlocks = 562;

    // Capture mono audio (channel 0) for pitch analysis + energy tracking.
    std::vector<float> allAudio;
    std::vector<double> blockEnergies;
    std::set<int> arpNotes;   // arp-generated notes (direct, tail-immune)
    allAudio.reserve (kNumBlocks * kBlock);

    for (int b = 0; b < kNumBlocks; ++b)
    {
        juce::AudioBuffer<float> buf (2, kBlock);
        buf.clear();
        juce::MidiBuffer empty;
        processor.processBlock (buf, empty);
        arpNotes.insert (static_cast<int> (processor.getEngine().getPart (0).arp.lastNote()));

        double rms = 0.0;
        for (int i = 0; i < kBlock; ++i)
        {
            const float s = buf.getSample (0, i);
            allAudio.push_back (s);
            rms += s * s;
        }
        rms = std::sqrt (rms / kBlock);
        blockEnergies.push_back (rms);
    }

    // ---- analysis: energy dynamics ----
    int activeBlocks = 0;
    for (double e : blockEnergies)
        if (e > 0.001)
            ++activeBlocks;

    std::printf ("[arp_test] active blocks: %d / %d\n", activeBlocks, kNumBlocks);
    check (activeBlocks > 10, "arp produces sustained audio across many blocks");

    double meanEnergy = 0.0;
    for (double e : blockEnergies) meanEnergy += e;
    meanEnergy /= blockEnergies.size();
    double variance = 0.0;
    for (double e : blockEnergies) variance += (e - meanEnergy) * (e - meanEnergy);
    double stddev = std::sqrt (variance / blockEnergies.size());

    std::printf ("[arp_test] energy mean=%.5f stddev=%.5f\n", meanEnergy, stddev);
    check (stddev > 0.001, "arp energy varies over time (note on/off dynamics)");

    // ---- pitch analysis: read the arp's generated notes directly (robust under
    // multitimbral — overlapping release tails would smear an audio-ZCR measure). ----
    std::set<int> distinctNotes;
    for (int n : arpNotes)
        if (n != 0xff)
            distinctNotes.insert (n);
    std::printf ("[arp_test] distinct arp notes generated:");
    for (int n : distinctNotes) std::printf (" %d", n);
    std::printf (" (count=%zu)\n", distinctNotes.size());
    check (distinctNotes.size() >= 2, "arp produces at least 2 distinct pitches (octave cycling)");

    // ---- descriptor count check ----
    const int descCount = static_cast<int> (getPatchParamDescriptors().size());
    std::printf ("[arp_test] descriptor count: %d (expected 260 = 106 + 5 arp + 4 options + 67 sequencer + 78 fx)\n", descCount);
    check (descCount == 260, "descriptor table includes 5 arp params (+4 options + 67 sequencer + 78 fx)");

    // ---- report ----
    std::printf ("\nARP TEST: %s\n", g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}

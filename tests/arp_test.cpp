// Arpeggiator verification: proves the host-tempo-driven arpeggiator generates
// multiple distinct note pitches over time when the transport is playing and
// keys are held, and that note-offs occur between steps.
//
// Run: cmake --build build --target parvati_arp_test && ./build/parvati_arp_test

#include <algorithm>
#include "unified_test_runner.h"
#include "test_utils.h"   // FakePlayHead
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

// Render `blocks` with no MIDI (lets deferred config/mode engages service on
// the audio thread + release tails decay).
void renderIdleBlocks (ParvatiAudioProcessor& p, int blocks)
{
    for (int i = 0; i < blocks; ++i)
    {
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer midi;
        p.processBlock (buf, midi);
    }
}

// A minimal AudioPlayHead that reports a fixed BPM + playing state.
}  // namespace

TEST(arp_test)
{
    juce::MessageManager::getInstance();
    juce::ScopedJuceInitialiser_GUI guiInit;

    ParvatiAudioProcessor processor;

    // Provide a fake play head at 120 BPM, playing.
    FakePlayHead playHead (120.0, true);
    processor.setPlayHead (&playHead);

    processor.prepareToPlay (48000.0, 256);
    processor.syncAllParamsToEngine();

    // Configure the arpeggiator: mode=Arp (index 1), direction=Up, octave=2, resolution=1/4.
    processor.getApvts().getParameterAsValue ("arp_mode") = 1.0f;       // Arp (index 1 of Off/Arp/Sequencer)
    processor.getApvts().getParameter ("arp_direction")->setValueNotifyingHost (0.0f);  // Up
    processor.getApvts().getParameter ("arp_octave")->setValueNotifyingHost (          // 2 octaves
        juce::jmap (2.0f, 1.0f, 4.0f, 0.0f, 1.0f));
    processor.getApvts().getParameter ("arp_resolution")->setValueNotifyingHost (       // 1/4 (index 6 = 24 ticks)
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
    // At 120 BPM with 1/4 resolution: 24 ticks per step, 1000 samples/tick
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

    // ---- stuck-note regression: enabling the arp while a note sounds must not
    // swallow that note's release. The note was triggered through the DIRECT
    // path (arp off), so its key never entered the arp's held-key stack; the
    // note-off must fall through to the direct path (pre-fix it was handed to
    // arp.noteOff unconditionally and the direct voice sustained forever).
    std::printf ("\n[arp_test] stuck-note regression: enable arp while a note sounds\n");
    {
        ParvatiAudioProcessor proc;
        proc.setPlayHead (&playHead);
        proc.prepareToPlay (48000.0, 256);
        proc.syncAllParamsToEngine();

        auto activePart0 = [&]() {
            int n = 0;
            for (int vi : proc.getEngine().getPart (0).voiceIndices)
                if (auto* av = proc.getEngine().getAmbikaVoice (vi))
                    if (av->isVoiceActive()) ++n;
            return n;
        };

        // arp OFF (default): note 60 sounds through the direct path.
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer on;
        on.addEvent (juce::MidiMessage::noteOn (1, 60, (uint8_t) 100), 0);
        proc.processBlock (buf, on);
        renderIdleBlocks (proc, 2);
        const int activeWhileHeld = activePart0();
        std::printf ("     active Part-0 voices with arp off, held: %d\n", activeWhileHeld);
        check (activeWhileHeld >= 1, "direct note sounds with arp off");

        // Enable the arp (mode 1) while the note sounds, flush the mode engage.
        proc.getApvts().getParameterAsValue ("arp_mode") = 1.0f;
        renderIdleBlocks (proc, 2);

        // Release the key: must reach the DIRECT voice (not the held-key stack).
        juce::AudioBuffer<float> buf2 (2, 256);
        buf2.clear();
        juce::MidiBuffer off;
        off.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
        proc.processBlock (buf2, off);
        // Let the release envelope decay (~1.5 s at 48k/256 — the init release
        // tail; a STUCK voice never goes inactive, so the window is generous).
        renderIdleBlocks (proc, 300);
        const int activeAfterRelease = activePart0();
        std::printf ("     active Part-0 voices after release + decay: %d (expect 0)\n", activeAfterRelease);
        check (activeAfterRelease == 0,
               "pre-arp note releases after enabling the arp (no stuck sustain)");
    }

    // ---- chord-direction release regression (bug hunt 2026-08-18, F-eng-2):
    // the firmware kills the note DIRECTLY at key-up in chord trigger mode
    // ("the chord trigger mode doesn't really clean after itself" —
    // ambika_reference/controller/part.cc:341-354). The port dropped that
    // branch: a released chord voice kept ringing (each step only re-triggers
    // HELD keys, no off is ever sent), and on the LAST key-up
    // allNotesOff()'s chord branch looped the already-EMPTY held-key stack —
    // every chord voice stranded until CC123 / voice-steal.
    std::printf ("\n[arp_test] chord-direction release regression (F-eng-2)\n");
    {
        ParvatiAudioProcessor proc;
        proc.setPlayHead (&playHead);
        proc.prepareToPlay (48000.0, 256);
        proc.syncAllParamsToEngine();

        auto activePart0 = [&]() {
            int n = 0;
            for (int vi : proc.getEngine().getPart (0).voiceIndices)
                if (auto* av = proc.getEngine().getAmbikaVoice (vi))
                    if (av->isVoiceActive()) ++n;
            return n;
        };

        // Arp mode, CHORD direction (choice index 5), 1/16 resolution.
        proc.getApvts().getParameterAsValue ("arp_mode") = 1.0f;
        proc.getApvts().getParameter ("arp_direction")->setValueNotifyingHost (
            juce::jmap (5.0f, 0.0f, 5.0f, 0.0f, 1.0f));   // Chord
        proc.getApvts().getParameter ("arp_resolution")->setValueNotifyingHost (
            juce::jmap (10.0f, 0.0f, 14.0f, 0.0f, 1.0f));  // 1/16

        // Hold a 3-note chord long enough for >= 2 arp steps (chord mode
        // triggers every held note each step).
        {
            juce::AudioBuffer<float> buf (2, 256);
            buf.clear();
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (uint8_t) 100), 0);
            midi.addEvent (juce::MidiMessage::noteOn (1, 64, (uint8_t) 100), 0);
            midi.addEvent (juce::MidiMessage::noteOn (1, 67, (uint8_t) 100), 0);
            proc.processBlock (buf, midi);
        }
        renderIdleBlocks (proc, 100);   // ~0.5 s: >= 1 full chord step at 120bpm/1/16
        const int activeWhileHeld = activePart0();
        std::printf ("     active Part-0 voices while chord held: %d\n", activeWhileHeld);
        check (activeWhileHeld >= 1, "chord mode triggers held notes");

        // Release ONE key mid-phrase: that pitch must stop (firmware kills it
        // at key-up). Pre-fix it rang until the next voice-steal.
        {
            juce::AudioBuffer<float> buf (2, 256);
            buf.clear();
            juce::MidiBuffer off;
            off.addEvent (juce::MidiMessage::noteOff (1, 64), 0);
            proc.processBlock (buf, off);
        }
        renderIdleBlocks (proc, 300);   // let the release tail decay
        // Notes 60/67 are still HELD — they legitimately remain active.
        const int activeAfterPartial = activePart0();
        std::printf ("     active after releasing one chord key: %d\n", activeAfterPartial);
        check (activeAfterPartial < activeWhileHeld || activeWhileHeld == 1,
               "releasing one chord key retires its voice (firmware key-up kill)");

        // Release the remaining keys: EVERY voice must retire. Pre-fix,
        // allNotesOff()'s chord branch iterated the now-empty stack and all
        // voices stayed stuck.
        {
            juce::AudioBuffer<float> buf (2, 256);
            buf.clear();
            juce::MidiBuffer off;
            off.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
            off.addEvent (juce::MidiMessage::noteOff (1, 67), 0);
            proc.processBlock (buf, off);
        }
        renderIdleBlocks (proc, 300);
        const int activeAfterAll = activePart0();
        std::printf ("     active after releasing all chord keys: %d (expect 0)\n", activeAfterAll);
        check (activeAfterAll == 0,
               "chord voices all retire on key release (no stranded chord voices)");
    }

    // ---- unclamped loaded arp bytes: a raw PartData mode 5 must not silence
    // the part, and a raw arpOctave 0 must stage >= 1 (the Random direction's
    // octave-wrap loop never terminated with range 0 — an audio-thread hang).
    std::printf ("\n[arp_test] raw PartData arp bytes are clamped at staging\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        proc.syncAllParamsToEngine();
        auto& e = proc.getEngine();

        // Stage a hand-forged PartData: arpMode 5 (out of Off/Arp/Sequencer),
        // direction 4 (Random), octave 0 (hang input), pattern/resolution/seq
        // lengths out of range too. stageArpSeqFromPartBytes is the loader
        // entry point (.MUL loads / host-state restore).
        auto& pb = e.getPart (0).partBytes;
        for (int b = 7; b <= 14; ++b)
            pb[(size_t) b] = 0xff;
        pb[7] = 5;   // arpMode = 5
        pb[8] = 4;   // direction = Random
        pb[9] = 0;   // octave = 0 (hang input)
        e.stageArpSeqFromPartBytes (0);

        // Flush one block: services configDirty (the staged config engages on
        // the audio thread). With the pre-fix raw mode 5, isActive() was true
        // while isEnabled() was false — the part swallowed notes silently; the
        // clamped mode (2 = Sequencer... or 0/1/2) must at least be ACTIVE-legal.
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer empty;
        proc.processBlock (buf, empty);

        const uint8_t mode = e.getPart (0).arp.getMode();
        std::printf ("     staged arp mode after clamp: %d (expect <= 2)\n", (int) mode);
        check (mode <= 2, "out-of-range arp mode clamps into Off/Arp/Sequencer");

        // The hang case: Random + octave 0. After one more staging round-trip
        // the ENGAGED octave must be >= 1 (setOctave receives the staged
        // value); the loop guard in stepArpeggio is the belt-and-braces half.
        // Read back through the pendingConfig consumer path: engage another
        // flush with a held key + running transport so stepArpeggio RUNS.
        proc.setPlayHead (&playHead);
        juce::MidiBuffer on;
        on.addEvent (juce::MidiMessage::noteOn (1, 60, (uint8_t) 100), 0);
        juce::AudioBuffer<float> buf2 (2, 256);
        buf2.clear();
        proc.processBlock (buf2, on);   // held key enters (mode is Sequencer/Arp — active)
        // ~2 s of transport at 120 BPM: many prescaled steps; pre-fix this
        // block loop would spin forever on the range-0 wrap (the test would
        // hang, not fail) — with the clamp + guard it returns.
        for (int i = 0; i < 400; ++i)
        {
            juce::AudioBuffer<float> b (2, 256);
            b.clear();
            juce::MidiBuffer m;
            proc.processBlock (b, m);
        }
        check (true, "Random + octave 0 no longer hangs the audio thread (blocks return)");
    }

    // ---- report ----
    std::printf ("\nARP TEST: %s\n", g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES");
    return g_failures == 0;
}

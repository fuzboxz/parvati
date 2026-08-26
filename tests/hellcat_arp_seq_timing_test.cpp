// Arp/Sequencer timing + note-lifecycle unit tests (direct, no engine).
//
// Proves the two engine fixes:
//   (A) BUG 1 — Arpeggiator::clockTick() now returns bool (true when a prescaled
//       step fired). The engine gates the Sequencer on this so BOTH run at the
//       same prescaled rate (firmware part.cc:590-601 runs ClockSequencer +
//       ClockArpeggiator in the SAME prescaled branch). Previously the Sequencer
//       ran every raw 24-PPQN tick = up to 24x too fast.
//   (B) BUG 2 — Sequencer::allNotesOff() releases a stranded previousNote_, and
//       clockTick's defensive else-branch self-cleans when the note-block guard
//       goes false (key released / length 0 / mode != NOTE).
//
// Run: ./build_unified/hellcat_unified_tests hellcat_arp_seq_timing_test

#include <cstdint>
#include "unified_test_runner.h"
#include "test_utils.h"   // FakePlayHead
#include <cstdio>
#include <set>
#include <vector>

#include "Arpeggiator.h"
#include "Sequencer.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>  // ScopedJuceInitialiser_GUI
#include "PluginProcessor.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}
}  // namespace

// ---------------------------------------------------------------------------
// (A) Arp prescaler gating — clockTick() returns true once per prescaler step.
// ---------------------------------------------------------------------------
static void testPrescalerGating()
{
    std::printf ("(A) Arpeggiator::clockTick fires once per prescaled step\n");
    using hellcat::Arpeggiator;
    using hellcat::kMidiClockTickPerStep;

    // Resolution index 13 -> prescaler 2 (a fast division, so few ticks needed).
    // start() forces clockCounter_ = prescaler_, so the FIRST tick fires, then
    // every prescaler_-th tick after.
    {
        Arpeggiator arp;
        arp.setResolution (13);   // kMidiClockTickPerStep[13] == 2
        arp.start();
        check (kMidiClockTickPerStep[13] == 2, "resolution 13 -> prescaler 2");

        int fires = 0;
        for (int t = 0; t < 20; ++t)
            if (arp.clockTick()) ++fires;
        // 20 ticks at prescaler 2: fires at 0,2,4,...,18 = 10.
        char msg[96];
        std::snprintf (msg, sizeof (msg), "20 ticks / prescaler 2 -> 10 fires (got %d)", fires);
        check (fires == 10, msg);
    }

    // Default resolution (index 6 -> prescaler 24): 48 ticks -> 2 fires.
    // (Pre-fix the Sequencer advanced all 48 ticks here -> 24x too fast.)
    {
        Arpeggiator arp;
        arp.setResolution (6);    // kMidiClockTickPerStep[6] == 24
        arp.start();
        check (kMidiClockTickPerStep[6] == 24, "resolution 6 (default) -> prescaler 24");

        int fires = 0;
        for (int t = 0; t < 48; ++t)
            if (arp.clockTick()) ++fires;
        char msg[96];
        std::snprintf (msg, sizeof (msg), "48 ticks / prescaler 24 -> 2 fires (got %d; was 48 pre-fix)", fires);
        check (fires == 2, msg);
    }
}

// ---------------------------------------------------------------------------
// (B) Sequencer::allNotesOff + the clockTick defensive else-branch.
// ---------------------------------------------------------------------------
static void testNoteLifecycle()
{
    using hellcat::Sequencer;
    std::printf ("\n(B) Sequencer allNotesOff + defensive self-clean\n");

    // --- allNotesOff releases the sounding note (idempotent) ---
    {
        Sequencer seq;
        int onCount = 0, offCount = 0;
        uint8_t lastOff = 0xff;
        seq.setNoteOnCallback  ([&] (int, uint8_t, uint8_t) { ++onCount; });
        seq.setNoteOffCallback ([&] (int, uint8_t n) { ++offCount; lastOff = n; });

        seq.setMode (2);                 // NOTE mode
        seq.setSequenceLength (2, 4);    // 4-step note sequence
        seq.setSequenceDataByte (32, 0x80 | 60);   // step 0: note 60, gate on (note seq starts at offset 32)
        seq.setSequenceDataByte (34, 0x80 | 64);
        seq.setSequenceDataByte (36, 0x80 | 67);
        seq.setSequenceDataByte (38, 0x80 | 72);

        // Drive one step with a held key (heldNote 60 -> transpose = +0, note 60).
        seq.clockTick (60, true);
        check (onCount == 1,  "note step fires one note-on");
        check (offCount == 0, "no note-off on the first step");

        seq.allNotesOff();
        check (offCount == 1,  "allNotesOff fires a note-off (releases the stranded note)");
        check (lastOff == 60,  "allNotesOff released the correct pitch (60)");

        // Idempotent: previousNote_ now 0xff -> a second call fires nothing.
        seq.allNotesOff();
        check (offCount == 1, "allNotesOff is idempotent (no double note-off)");
    }

    // --- defensive else-branch (PATH A): keyHeld -> false releases the note ---
    {
        Sequencer seq;
        int onCount = 0, offCount = 0;
        seq.setNoteOnCallback  ([&] (int, uint8_t, uint8_t) { ++onCount; });
        seq.setNoteOffCallback ([&] (int, uint8_t) { ++offCount; });

        seq.setMode (2);
        seq.setSequenceLength (2, 4);
        seq.setSequenceDataByte (32, 0x80 | 60);   // note step 0 (offset 32)

        seq.clockTick (60, true);    // note-on, previousNote_ = 60
        check (onCount == 1 && offCount == 0, "note-on fired, none off");

        seq.clockTick (60, false);   // keyHeld false -> defensive else-branch
        check (offCount == 1, "clockTick self-cleans (else-branch) when keyHeld goes false");
    }
}

static void testEngineNoteSeqNoStuck()
{
    std::printf ("(C) engine: NOTE seq releases on key-release (no stuck note)\n");

    juce::ScopedJuceInitialiser_GUI guiInit;
    juce::MessageManager::getInstance();

    // Minimal play head at 120 BPM, playing (shared fixture).
    HellcatAudioProcessor proc;
    FakePlayHead playHead (120.0, true);
    proc.setPlayHead (&playHead);
    proc.prepareToPlay (48000.0, 256);
    proc.syncAllParamsToEngine();

    // NOTE/Sequencer mode (arp_mode choice: 0=Off,1=Arp,2=Sequencer).
    proc.getApvts().getParameterAsValue ("arp_mode") = 2.0f;
    proc.getApvts().getParameterAsValue ("seq_length_3") = 4.0f;       // 4-step note seq
    // Program 4 gated notes: C4/E4/G4/C5 (byte = 0x80 | note).
    const int notes[4] = { 60, 64, 67, 72 };
    for (int i = 0; i < 4; ++i)
        proc.getApvts().getParameterAsValue (juce::String ("seqnote_step") + juce::String (i)) = static_cast<float> (0x80 | notes[i]);

    auto peakOf = [] (const juce::AudioBuffer<float>& b) {
        float pk = 0.0f;
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
                pk = juce::jmax (pk, std::fabs (b.getSample (ch, i)));
        return pk;
    };

    // Press key 60 (C4) -> the seq transposes by it; render ~2.2s so the 4-step
    // seq cycles a couple of times at the default 1/4 resolution (0.5s/step).
    float peakDuring = 0.0f;
    std::set<int> distinctSeqNotes;
    {
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, static_cast<uint8_t> (100)), 0);
        proc.processBlock (buf, midi);
        peakDuring = juce::jmax (peakDuring, peakOf (buf));
        distinctSeqNotes.insert (proc.getEngine().getPart (0).seq.debugPreviousNote());
    }
    for (int blk = 0; blk < 400; ++blk)   // ~2.1s
    {
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer empty;
        proc.processBlock (buf, empty);
        peakDuring = juce::jmax (peakDuring, peakOf (buf));
        distinctSeqNotes.insert (proc.getEngine().getPart (0).seq.debugPreviousNote());
    }

    // Release the key; render ~1.5s and measure the LATE peak (past any release tail).
    {
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
        proc.processBlock (buf, midi);
    }
    float peakAfter = 0.0f;
    for (int blk = 0; blk < 250; ++blk)   // ~1.3s after release
    {
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer empty;
        proc.processBlock (buf, empty);
        if (blk > 200)   // last ~0.26s, well past the release tail
            peakAfter = juce::jmax (peakAfter, peakOf (buf));
    }

    char msg[160];
    std::snprintf (msg, sizeof (msg), "seq produced audio while key held (peakDuring=%.4f)", peakDuring);
    check (peakDuring > 0.01f, msg);
    std::snprintf (msg, sizeof (msg), "seq cycled through >=2 distinct notes (got %d) -- not frozen on one", (int) distinctSeqNotes.size());
    check (distinctSeqNotes.size() >= 2, msg);
    std::snprintf (msg, sizeof (msg), "audio silent after key-release (peakAfter=%.4f, must be < 0.002) -- stuck note if high", peakAfter);
    check (peakAfter < 0.002f, msg);
}

TEST(hellcat_arp_seq_timing_test)
{
    std::printf ("=== arp/seq timing + note lifecycle ===\n\n");
    testPrescalerGating();
    testNoteLifecycle();
    testEngineNoteSeqNoStuck();

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures == 0 ? "ALL PASS" : "FAILURES",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}

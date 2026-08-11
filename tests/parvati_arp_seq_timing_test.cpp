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
// Run: cmake --build build --target parvati_arp_seq_timing_test && ./build/parvati_arp_seq_timing_test

#include <cstdint>
#include <cstdio>

#include "Arpeggiator.h"
#include "Sequencer.h"

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
    using parvati::Arpeggiator;
    using parvati::kMidiClockTickPerStep;

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
    using parvati::Sequencer;
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

int main()
{
    std::printf ("=== arp/seq timing + note lifecycle ===\n\n");
    testPrescalerGating();
    testNoteLifecycle();

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures == 0 ? "ALL PASS" : "FAILURES",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}

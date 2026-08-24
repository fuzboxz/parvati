// NoteStack unit test (Source/NoteStack.h — faithful port of the Ambika
// controller's note_stack.h).
//
// Pins the linked-list + sorted-array invariants the arpeggiator relies on:
//   - sorted_note(i) ascending by pitch
//   - played_note(i) insertion order (0 = oldest played; the last index is
//     the most recent — the doc'd "LIFO order, reversed")
//   - most_recent_note == the LIFO head
//   - saturation at capacity evicts the LEAST-recently-played note (the
//     next_ptr==0 tail), never inflating size_
//   - re-noteOn of a held note is a dedup: size unchanged, the note moves to
//     most-recent with the NEW velocity
//   - contains / noteOff keep both orderings consistent
//   - clear() fully resets (including the pool[0] dummy sentinel)
//
// This class caused a past hosted SIGBUS (see the NoteStack.h header comment)
// when default-init desynced the pool from the sorted array — hence the
// fresh-construction checks at the top.
//
// Run: ./build_unified/parvati_unified_tests note_stack_test

#include <cstdint>
#include "unified_test_runner.h"
#include <cstdio>

#include "NoteStack.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}
}  // namespace

TEST(note_stack_test)
{
    std::printf ("=== Parvati NoteStack unit (ordering / saturation / dedup) ===\n");

    // ---- Fresh construction is usable without init() (the ctor clears) ----
    std::printf ("\n[1] default construction is clear\n");
    {
        parvati::NoteStack<12> stack;
        check (stack.size() == 0, "fresh stack is empty");
        check (! stack.contains (60), "fresh stack contains nothing");

        // First noteOn must succeed without corrupting state (the old
        // zero-init pool made the free-slot search fail -> dummy-sentinel
        // write + size inflation; the hosted SIGBUS).
        stack.noteOn (60, 100);
        check (stack.size() == 1, "first noteOn after default ctor lands (size 1)");
        check (stack.most_recent_note().note == 60 && stack.most_recent_note().velocity == 100,
               "first noteOn is most-recent with its velocity");
    }

    // ---- Orderings with a 3-note chord ----
    std::printf ("\n[2] sorted (pitch) + played (insertion) orderings\n");
    {
        parvati::NoteStack<12> stack;
        stack.noteOn (60, 100);   // played 1st
        stack.noteOn (67, 80);    // played 2nd
        stack.noteOn (64, 90);    // played 3rd (most recent)

        check (stack.size() == 3, "three noteOn -> size 3");

        // sorted_note: ascending pitch regardless of press order.
        check (stack.sorted_note (0).note == 60, "sorted_note(0) = 60");
        check (stack.sorted_note (1).note == 64, "sorted_note(1) = 64");
        check (stack.sorted_note (2).note == 67, "sorted_note(2) = 67");

        // played_note: 0 = OLDEST played, last = most recent.
        check (stack.played_note (0).note == 60, "played_note(0) = 60 (oldest)");
        check (stack.played_note (1).note == 67, "played_note(1) = 67");
        check (stack.played_note (2).note == 64, "played_note(2) = 64 (most recent)");

        // most_recent = LIFO head.
        check (stack.most_recent_note().note == 64, "most_recent_note = 64 (last pressed)");

        check (stack.contains (60) && stack.contains (64) && stack.contains (67),
               "contains all held notes");
        check (! stack.contains (61), "contains an unheld note is false");
    }

    // ---- noteOff removes from BOTH orderings ----
    std::printf ("\n[3] noteOff keeps both orderings consistent\n");
    {
        parvati::NoteStack<12> stack;
        stack.noteOn (60, 100);
        stack.noteOn (64, 90);
        stack.noteOn (67, 80);

        stack.noteOff (64);
        check (stack.size() == 2, "noteOff(64) -> size 2");
        check (! stack.contains (64), "noteOff removes contains()");
        check (stack.sorted_note (0).note == 60 && stack.sorted_note (1).note == 67,
               "sorted order closes the gap (60, 67)");
        check (stack.played_note (0).note == 60 && stack.played_note (1).note == 67,
               "played order closes the gap (60, 67)");
        check (stack.most_recent_note().note == 67, "most-recent survives the middle removal");
    }

    // ---- Re-noteOn dedup: no duplicate, moves to most-recent ----
    std::printf ("\n[4] re-noteOn dedups (no size inflation)\n");
    {
        parvati::NoteStack<12> stack;
        stack.noteOn (60, 100);
        stack.noteOn (64, 90);
        stack.noteOn (67, 80);

        stack.noteOn (64, 50);   // re-press a HELD note
        check (stack.size() == 3, "re-noteOn keeps size 3 (no duplicate)");
        check (stack.most_recent_note().note == 64, "re-pressed note becomes most-recent");
        check (stack.most_recent_note().velocity == 50, "re-press updates the velocity");
        check (stack.sorted_note (1).note == 64, "sorted position unchanged by re-press");
        check (stack.sorted_note (0).note == 60 && stack.sorted_note (2).note == 67,
               "sorted neighbours unchanged by re-press");
    }

    // ---- Saturation: capacity 12 evicts the least-recently-played ----
    std::printf ("\n[5] saturation evicts least-recently-played at capacity\n");
    {
        parvati::NoteStack<12> stack;
        for (int n = 40; n <= 51; ++n)   // 12 distinct notes; 40 = oldest
            stack.noteOn (static_cast<uint8_t> (n), 100);
        check (stack.size() == 12, "12 noteOn -> size 12 (at capacity)");

        stack.noteOn (static_cast<uint8_t> (52), 100);   // 13th distinct
        check (stack.size() == 12, "13th noteOn keeps size 12 (saturated)");
        check (! stack.contains (40), "oldest note (40) evicted");
        check (stack.contains (52), "new note (52) admitted");
        check (stack.contains (41) && stack.contains (51),
               "all non-oldest notes survive the eviction");
        check (stack.most_recent_note().note == 52, "the 13th note is most-recent");

        // Re-pressing an old note refreshes recency: 41 must NOT be the one
        // evicted by the next saturating press — 42 is the new tail.
        stack.noteOn (41, 100);
        stack.noteOn (53, 100);
        check (stack.size() == 12, "second saturating press keeps size 12");
        check (! stack.contains (42), "eviction follows recency order (42 now oldest)");
        check (stack.contains (41), "re-pressed 41 survives");
    }

    // ---- clear() fully resets ----
    std::printf ("\n[6] clear() resets everything\n");
    {
        parvati::NoteStack<12> stack;
        stack.noteOn (60, 100);
        stack.noteOn (64, 90);
        stack.clear();
        check (stack.size() == 0, "clear -> size 0");
        check (! stack.contains (60) && ! stack.contains (64), "clear -> contains nothing");
        stack.noteOn (67, 70);
        check (stack.size() == 1 && stack.most_recent_note().note == 67,
               "stack is reusable after clear");
    }

    // ---- init() is an alias of clear() (firmware entry point) ----
    std::printf ("\n[7] init() == clear()\n");
    {
        parvati::NoteStack<12> stack;
        stack.noteOn (60, 100);
        stack.init();
        check (stack.size() == 0, "init -> size 0");
    }

    std::printf ("\n=== %s (%d failure%s) ===\n",
                 g_failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}

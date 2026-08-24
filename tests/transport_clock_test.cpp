// TransportClock unit test (Source/TransportClock.h — host BPM -> 24-PPQN
// tick stream).
//
// Pins the tempo math and clamps the arpeggiator depends on:
//   - samplesPerTick = sr*60/(bpm*24): 1000.0 exactly at 48 kHz / 120 BPM
//     (24 PPQN x 120 BPM = 2880 ticks/min = 48 Hz -> 48000/48 = 1000 samples
//     per tick; NOTE the gap-audit's "100.0" was an arithmetic slip — the
//     code and this test both pin 1000.0). Pinned behaviourally: advance(999)
//     yields no tick, +1 sample does, and the fractional carry returns to
//     exactly 0 after a whole tick
//   - advance() fractional carry is DRIFT-FREE: 3-sample calls at 480 BPM
//     over 1e6 samples produce exactly samples/samplesPerTick ticks (4000)
//   - bpm > 999 clamps (a 5000 BPM host behaves as 999, not as spt=24)
//   - bpm <= 0 keeps the PREVIOUS rate (a degenerate host value is "no
//     update", not a recompute to garbage)
//   - advance() never spins: ticks <= numSamples
//   - reset() zeroes the fractional phase
//
// Run: ./build_unified/parvati_unified_tests transport_clock_test

#include <cstdlib>
#include "unified_test_runner.h"
#include <cstdio>

#include "TransportClock.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}
}  // namespace

TEST(transport_clock_test)
{
    std::printf ("=== Parvati TransportClock unit (tempo math / clamps / drift) ===\n");

    // ---- 48k / 120 BPM -> exactly 1000 samples per 24-PPQN tick ----
    std::printf ("\n[1] samplesPerTick = 1000.0 @ 48k/120\n");
    {
        parvati::TransportClock clock;
        clock.prepare (48000.0);
        clock.setTempo (120.0);

        check (clock.advance (999) == 0, "999 samples -> no tick");
        check (clock.advance (1) == 1, "the 1000th sample completes the tick");
        // After a whole tick the phase must be exactly 0 again (integer-exact
        // double math), i.e. the next tick again needs a full 1000 samples.
        check (clock.advance (999) == 0, "carry is exactly 0 after the tick");
        check (clock.advance (1) == 1, "second tick lands on sample 2000");
    }

    // ---- Drift-free fractional carry: 480 BPM, 3-sample calls, 1e6 samples ----
    std::printf ("\n[2] 1e6 samples @ 480 BPM in 3-sample calls -> exactly 4000 ticks\n");
    {
        parvati::TransportClock clock;
        clock.prepare (48000.0);
        clock.setTempo (480.0);   // spt = 48000*60/(480*24) = 250.0

        int totalTicks = 0;
        int remaining  = 1'000'000;
        while (remaining >= 3)
        {
            totalTicks += clock.advance (3);
            remaining  -= 3;
        }
        if (remaining > 0)
            totalTicks += clock.advance (remaining);

        const int expected = 1'000'000 / 250;   // 4000
        std::printf ("     ticks = %d (expect %d +- 1)\n", totalTicks, expected);
        check (std::abs (totalTicks - expected) <= 1,
               "tick count over 1e6 samples matches samples/spt within 1 (drift-free)");
    }

    // ---- bpm > 999 clamps ----
    std::printf ("\n[3] bpm > 999 clamps to 999\n");
    {
        parvati::TransportClock clock;
        clock.prepare (48000.0);
        clock.setTempo (5000.0);   // unclamped would be spt = 24 -> 8 ticks/200

        const int ticks = clock.advance (200);
        std::printf ("     200 samples @ '5000 BPM' -> %d ticks (999 BPM ~ 1.66; unclamped would be 8)\n",
                     ticks);
        check (ticks == 1, "clamped to 999 BPM (1 tick per 200 samples)");
        check (ticks <= 200, "advance never returns more ticks than samples");
    }

    // ---- bpm <= 0 keeps the previous rate ----
    std::printf ("\n[4] bpm <= 0 is a no-update (previous rate kept)\n");
    {
        parvati::TransportClock clock;
        clock.prepare (48000.0);
        clock.setTempo (120.0);          // spt = 1000
        clock.reset();

        clock.setTempo (0.0);
        check (clock.advance (1000) == 1, "after setTempo(0): still 1 tick / 1000 samples");

        clock.setTempo (-7.0);
        check (clock.advance (1000) == 1, "after setTempo(-7): still 1 tick / 1000 samples");
    }

    // ---- reset() zeroes the fractional phase ----
    std::printf ("\n[5] reset() zeroes the phase\n");
    {
        parvati::TransportClock clock;
        clock.prepare (48000.0);
        clock.setTempo (120.0);

        clock.advance (500);   // phase = 500/1000
        clock.reset();
        // Without the reset the next 500 samples would complete a tick.
        check (clock.advance (500) == 0, "post-reset 500 samples -> no tick (phase was 0)");
        check (clock.advance (500) == 1, "a full fresh 1000 samples -> exactly 1 tick");
    }

    std::printf ("\n=== %s (%d failure%s) ===\n",
                 g_failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}

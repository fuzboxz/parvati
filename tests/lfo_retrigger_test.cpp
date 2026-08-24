// LFO retrigger-on-note verification for Parvati (Ambika port).
// Mirrors firmware Part::RetriggerLfos: an LFO whose retrigger_mode is
// LFO_SYNC_MODE_SLAVE resets its phase to 0 on a fresh note-on. Drives an
// ambika::dsp::Voice directly and reads MOD_SRC_LFO_1 each block.

#include "dsp/voice.h"
#include "unified_test_runner.h"
#include "dsp/constants.h"
#include "dsp/patch.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

using namespace ambika::dsp;

static int g_failures = 0;

// This file keeps its own CHECK macro: it wins over the runner copy.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"

#define CHECK(cond, msg)                                            \
    do {                                                            \
        if (cond) { std::printf ("  ok  : %s\n", msg); }            \
        else { std::printf ("  FAIL: %s\n", msg); ++g_failures; }   \
    } while (0)
#pragma clang diagnostic pop

// env_lfo[0] is at Patch byte offset 24:
//   attack24, decay25, sustain26, release27, shape28, rate29, padding30, retrigger_mode31
static constexpr int kShape   = 28;
static constexpr int kRate    = 29;
static constexpr int kRetrig  = 31;

// Run a fresh voice, returning MOD_SRC_LFO_1 after: trigger note1 -> warmup
// (so the rate-105 increment is installed) -> `mid` more renders -> an optional
// second note-on (the retrigger under test) -> one more render.
static uint8_t runScenario (uint8_t retriggerMode, int mid, bool doRetrigger)
{
    Voice v;
    v.Init();
    v.setTempo (120.0);
    v.set_patch_data (kShape,  LFO_WAVEFORM_TRIANGLE);
    v.set_patch_data (kRate,   105);                 // fast free-running LFO
    v.set_patch_data (kRetrig, retriggerMode);

    v.Trigger (60 << 7, 200, 0);

    for (int b = 0; b < 4; ++b)   // warmup: install the rate-105 increment
        v.ProcessBlock();
    for (int b = 0; b < mid; ++b) // advance to mid-cycle
        v.ProcessBlock();

    if (doRetrigger)
        v.Trigger (64 << 7, 200, 0);   // second note -> retriggers SLAVE LFOs

    v.ProcessBlock();
    return v.modulation_source (MOD_SRC_LFO_1);
}

TEST(lfo_retrigger_test)
{
    constexpr int mid = 40;  // well past the triangle midpoint

    // Phase-0 reference: a SLAVE LFO that is reset (mid=0 + retrigger) starts
    // from phase ~0 -> value in the ~255 region (triangle starts at the top).
    const uint8_t vFresh = runScenario (LFO_SYNC_MODE_SLAVE, 0, true);
    std::printf ("phase-0 (fresh retrigger) value = %u\n", vFresh);

    std::printf ("\n[1] SLAVE mode retriggers LFO phase to 0\n");
    {
        const uint8_t vR = runScenario (LFO_SYNC_MODE_SLAVE, mid, true);   // retriggered
        const uint8_t vC = runScenario (LFO_SYNC_MODE_SLAVE, mid, false);  // not retriggered
        std::printf ("     SLAVE retriggered=%u  continued=%u  fresh=%u\n", vR, vC, vFresh);

        char m[110];
        std::snprintf (m, sizeof (m),
                       "SLAVE retrigger jumps back near phase-0 (|vR-fresh|<=10, was %d)",
                       std::abs ((int) vR - (int) vFresh));
        CHECK (std::abs ((int) vR - (int) vFresh) <= 10, m);

        std::snprintf (m, sizeof (m),
                       "SLAVE without retrigger stays mid-cycle (|vC-fresh|>50, was %d)",
                       std::abs ((int) vC - (int) vFresh));
        CHECK (std::abs ((int) vC - (int) vFresh) > 50, m);

        std::snprintf (m, sizeof (m),
                       "retrigger changes the value vs no-retrigger (vR=%u != vC=%u)", vR, vC);
        CHECK (vR != vC, m);
    }

    std::printf ("\n[2] FREE mode does NOT retrigger (no-op)\n");
    {
        const uint8_t vR = runScenario (LFO_SYNC_MODE_FREE, mid, true);    // "retriggered"
        const uint8_t vC = runScenario (LFO_SYNC_MODE_FREE, mid, false);   // continued
        std::printf ("     FREE retriggered=%u  continued=%u\n", vR, vC);
        char m[110];
        std::snprintf (m, sizeof (m),
                       "FREE retrigger is a no-op (vR == vC, %u vs %u)", vR, vC);
        CHECK (vR == vC, m);
    }

    std::printf ("\n%s (%d failures)\n",
                 g_failures ? "LFO RETRIGGER TEST: FAILURES" : "LFO RETRIGGER TEST: ALL CHECKS PASSED",
                 g_failures);
    return g_failures == 0;
}

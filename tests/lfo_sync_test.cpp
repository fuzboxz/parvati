// Tempo-synced LFO verification for Hellcat (Ambika port).
// Drives ambika::dsp::Voice directly, reads MOD_SRC_LFO_1 each block, and
// confirms that synced rates (env_lfo[i].rate < kNumSyncedLfoRates) lock to the
// host BPM at the expected frequency derived from midi_clock_tick_per_step[],
// while free-running rates (>= 15) are tempo-independent.

#include "dsp/voice.h"
#include "unified_test_runner.h"
#include "dsp/constants.h"
#include "dsp/patch.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using ambika::dsp::Voice;
using ambika::dsp::kAudioBlockSize;
using ambika::dsp::kInternalSampleRate;
using ambika::dsp::midi_clock_tick_per_step;
using namespace ambika::dsp;  // MOD_SRC_*, LFO_WAVEFORM_* enum constants

static int g_failures = 0;

// This file keeps its own CHECK macro: it wins over the runner copy.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"

#define CHECK(cond, msg)                                            \
    do {                                                            \
        if (cond) { printf("  ok  : %s\n", msg); }                  \
        else { printf("  FAIL: %s\n", msg); ++g_failures; }         \
    } while (0)
#pragma clang diagnostic pop

// env_lfo[0] lives at Patch byte offset 24: {attack,decay,sustain,release,
// shape, rate, padding, retrigger}. So shape=28, rate=29.
static constexpr int kEnvLfo0Shape = 28;
static constexpr int kEnvLfo0Rate  = 29;

static double median(std::vector<int> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v.size() % 2 ? (double) v[v.size() / 2]
                        : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2.0;
}

// Measure LFO_1 frequency (Hz): record its value each block, find the median
// period (in blocks) between upward crossings of the mid-point (128), convert
// to Hz using the internal sample rate + block size.
static double measureLfoFreq(uint8_t shape, uint8_t rate, double bpm, int numBlocks)
{
    Voice voice;
    voice.Init();
    voice.setTempo(bpm);
    voice.set_patch_data(kEnvLfo0Shape, shape);
    voice.set_patch_data(kEnvLfo0Rate, rate);
    voice.Trigger(60 << 7, 200, 0);

    std::vector<int> cycleBlocks;
    int prev = 128, lastUpCross = -1;
    for (int b = 0; b < numBlocks; ++b)
    {
        voice.ProcessBlock();
        const int v = voice.modulation_source(MOD_SRC_LFO_1);
        if (prev < 128 && v >= 128)
        {
            if (lastUpCross >= 0) cycleBlocks.push_back(b - lastUpCross);
            lastUpCross = b;
        }
        prev = v;
    }

    const double cyc = median(cycleBlocks);
    if (cyc <= 0.0) return 0.0;
    return kInternalSampleRate / (cyc * (double) kAudioBlockSize);
}

TEST(lfo_sync_test)
{
    const double bpm = 120.0;
    const double ticksPerSec = 24.0 * bpm / 60.0;  // 48.0

    struct Case { uint8_t rate; const char* name; };
    const Case synced[] = { {6,  "rate 6  (24 ticks -> 1/16)"},
                            {9,  "rate 9  (8 ticks)"},
                            {12, "rate 12 (3 ticks)"} };

    printf("[1] Tempo-synced LFO rates @ %.0f BPM (triangle)\n", bpm);
    for (const auto& c : synced)
    {
        const double expected = ticksPerSec / midi_clock_tick_per_step[c.rate];
        const double measured = measureLfoFreq(LFO_WAVEFORM_TRIANGLE, c.rate, bpm, 4000);
        const double errPct = expected > 0.0 ? std::abs(measured - expected) / expected * 100.0 : 999.0;
        char msg[160];
        std::snprintf(msg, sizeof(msg), "%s: expected %.2f Hz, measured %.2f Hz (%.2f%%)",
                      c.name, expected, measured, errPct);
        printf("   %s\n", msg);
        CHECK(errPct < 2.0, msg);
    }

    printf("\n[2] Free-running rate (105, ~11.6 Hz) is tempo-independent\n");
    {
        const double f60  = measureLfoFreq(LFO_WAVEFORM_TRIANGLE, 105,  60.0, 4000);
        const double f240 = measureLfoFreq(LFO_WAVEFORM_TRIANGLE, 105, 240.0, 4000);
        const double rel  = std::abs(f60 - f240) / std::max(1e-9, f60) * 100.0;
        char msg[160];
        std::snprintf(msg, sizeof(msg), "free-running @60=%.2f Hz vs @240=%.2f Hz (%.2f%% apart)",
                      f60, f240, rel);
        printf("   %s\n", msg);
        CHECK(rel < 2.0, msg);
    }

    printf("\n%s (%d failures)\n",
           g_failures ? "LFO SYNC TEST: FAILURES" : "LFO SYNC TEST: ALL CHECKS PASSED",
           g_failures);
    return g_failures == 0;
}

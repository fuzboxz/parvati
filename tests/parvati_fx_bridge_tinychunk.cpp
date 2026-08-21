// Regression: HostRateBridge must NOT zero its output when a 1-sample block is
// passed (m==0). Root cause: renderPartFx's drift-corrected ~980 Hz sub-chunking
// periodically emits a 1-sample sub-chunk (the `if (sub<=0) sub=1` guard). With
// the phase carry >= 1, hostToInternal(1) produced m=0 internal samples, and
// internalToHost then ZEROED that host sample -> a full-amplitude dropout (the
// audible "crackle" in every Clouds FX). The fix: hold the last processed
// internal sample (prevTail_) instead of zeroing.
//
// This test drives the bridge with 1-sample blocks (the worst case) and with a
// drift-like [49,49,49,1] pattern, and FAILS if any output sample is zeroed or
// the output has an unbounded discontinuity. It FAILS if the m=0 hold fix is
// reverted (zero-dropout returns).
//
// Build: linked as parvati_fx_bridge_tinychunk_test (see CMakeLists).

#include <algorithm>
#include "unified_test_runner.h"
#include <cmath>
#include <cstdio>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "dsp/fx/HostRateBridge.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

// Render a 220 Hz tone through the bridge identity round-trip using the given
// per-call block-size PATTERN (repeated). Returns the L output.
std::vector<float> renderPattern (double sr, const std::vector<int>& pattern, int totalSamples)
{
    std::vector<float> L (static_cast<size_t> (totalSamples)), R (static_cast<size_t> (totalSamples));
    for (int i = 0; i < totalSamples; ++i)
        L[(size_t) i] = R[(size_t) i] = static_cast<float> (0.3 * std::sin (2.0 * 3.14159265 * 220.0 * i / sr));
    HostRateBridge b;
    b.prepare (sr, 256);
    size_t pIdx = 0;
    int off = 0;
    while (off < totalSamples)
    {
        const int chunk = pattern[pIdx % pattern.size()];
        ++pIdx;
        const int n = std::min (chunk, totalSamples - off);
        b.hostToInternal (L.data() + off, R.data() + off, n);
        b.internalToHost (L.data() + off, R.data() + off, n);
        off += n;
    }
    return L;
}
}  // namespace

TEST(parvati_fx_bridge_tinychunk)
{
    std::printf ("=== HostRateBridge tiny-chunk (m==0) dropout regression ===\n");
    constexpr double sr = 48000.0;
    constexpr int total = 48000;   // 1 s

    // ---- 1: worst case -- every call is a 1-sample block ----
    {
        const auto out = renderPattern (sr, { 1 }, total);
        int zeros = 0;
        for (int i = 2000; i < total - 1; ++i)   // skip warmup
            if (out[(size_t) i] == 0.0f) ++zeros;
        // worst sample-to-sample delta in steady state (the pre-fix dropout made
        // this ~= the full signal amplitude 0.3, alternating signal/zero).
        double worst = 0.0;
        for (int i = 2001; i < total - 1; ++i)
            worst = std::max (worst, std::fabs (static_cast<double> (out[(size_t) i] - out[(size_t) i - 1])));
        char msg[160];
        std::snprintf (msg, sizeof (msg), "1-sample blocks: zero-output samples=%d (must be 0); worstDelta=%.4f (must be < 0.05)", zeros, worst);
        std::printf ("  %s\n", msg);
        check (zeros == 0 && worst < 0.05, msg);
    }

    // ---- 2: drift-like pattern including a 1-sample sub-chunk (mirrors
    //       renderPartFx's cadence: ~49, ~49, ~49, then a 1-sample alignment) ----
    {
        const auto out = renderPattern (sr, { 49, 49, 49, 1 }, total);
        int zeros = 0;
        double worst = 0.0;
        for (int i = 2001; i < total - 1; ++i)
        {
            if (out[(size_t) i] == 0.0f) ++zeros;
            worst = std::max (worst, std::fabs (static_cast<double> (out[(size_t) i] - out[(size_t) i - 1])));
        }
        char msg[160];
        std::snprintf (msg, sizeof (msg), "drift pattern {49,49,49,1}: zeros=%d (must be 0); worstDelta=%.4f (must be < 0.02)", zeros, worst);
        std::printf ("  %s\n", msg);
        check (zeros == 0 && worst < 0.02, msg);
    }

    std::printf ("\n%s\n", g_failures == 0 ? "All HostRateBridge tiny-chunk checks PASSED." : "FAILED.");
    return g_failures == 0;
}

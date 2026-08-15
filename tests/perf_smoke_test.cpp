// perf_smoke_test.cpp — CI-portable headless performance regression gate.
//
// Catches the timer/message-thread class of UI regression without a real
// window: constructs the processor + full editor headlessly, then pumps the
// macOS main CFRunLoop in 20 ms slices for 10 s so every editor Timer fires
// at its real cadence, while measuring process CPU time (getrusage
// user+sys) and message-thread congestion (a 30 Hz probe Timer whose
// inter-callback gaps stretch when the thread is busy).
//
// This would have caught the PatchPage PartRow combo async-onChange
// ping-pong (an infinite message storm = instant CPU budget blowout +
// pegged probe gaps) without the manual `sample` session it took to find.
//
// THE PUMP: JUCE 9 has no unguarded synchronous pump — runDispatchLoopUntil
// is JUCE_MODAL_LOOPS_PERMITTED-gated (compiled OUT of the Parvati lib this
// test links), and runDispatchLoop is [NSApp run], which in a console
// binary does not service the JUCE timer queue (measured: a 30 Hz probe
// Timer fires 0 times inside it). The JUCE MessageQueue on macOS IS a
// CFRunLoopSource on the main run loop (juce_MessageQueue_mac.h), so the
// correct headless pump is to run that loop directly. Slice WALL time is
// not a useful metric (a slice that handles a source returns early, an
// idle slice runs out its timeout), hence the budgets below.
//
// Budgets, tuned with headroom over the measured value on the reference
// machine (Apple Silicon dev box, Debug build; three runs measured CPU
// 0.256-0.271 s, gap p99 43.7-44.0 ms, gap max 44.1-44.4 ms — the ~44 ms
// canary period is the CFRunLoop slice granularity + timer delivery, not
// congestion):
//   * total CPU time for the 10 s window: ~0.27 s measured -> 0.8 s budget
//   * 99th-percentile probe gap: ~44 ms measured -> 55 ms budget
//   * max single probe gap: ~45 ms measured -> 120 ms budget
// The historical regressions blow these out by 1-2 orders of magnitude
// (the combo ping-pong was ~10 s of CPU in a 10 s window; a 30 Hz repaint
// churn more than doubles the CPU), so the headroom is generous without
// being permissive. If a legitimate feature raises the floor, re-measure
// and update BOTH the constant and this comment (a budget that was never
// re-derived is a budget nobody trusts).
//
// What it cannot see: peer-driven repaint cost (no window = no paint) —
// that is what tools/profile_ui.sh covers as a LOCAL/manual gate (the
// CGEvent drag helper needs Accessibility permission, so it cannot run
// unattended on CI runners). See CONTRIBUTING.md.
//
// Built by default. Run: ./build/parvati_perf_smoke_test

#if ! __APPLE__
 #error "perf_smoke_test pumps the macOS main CFRunLoop directly (see the pump note above)"
#endif

#include <CoreFoundation/CoreFoundation.h>
#include <sys/resource.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"

namespace
{
// ---- budgets (re-measure + update together; see file header) ----
constexpr int    kRunSeconds          = 10;
constexpr double kCpuTimeBudgetSecs   = 0.8;   // 10 s window, ~3x measured
constexpr double kGapP99BudgetMs      = 55.0;  // canary period ~44 ms + headroom
constexpr double kGapMaxBudgetMs      = 120.0; // no single stalled second

int g_failures = 0;
void check (bool cond, const char* msg) { std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg); if (! cond) ++g_failures; }

double cpuTimeSecs()
{
    rusage ru {};
    getrusage (RUSAGE_SELF, &ru);
    return (double) ru.ru_utime.tv_sec + (double) ru.ru_utime.tv_usec * 1e-6
         + (double) ru.ru_stime.tv_sec + (double) ru.ru_stime.tv_usec * 1e-6;
}

// A 30 Hz canary on the message thread: its inter-callback gaps are ~33 ms
// while the thread is healthy and stretch when dispatch work (runaway
// timers, message storms) congests the queue. This is the same signal a
// user perceives as a laggy knob or a frozen repaint.
struct CongestionProbe : public juce::Timer
{
    std::vector<double> gapsMs;
    double lastCallbackMs = 0.0;

    void timerCallback() override
    {
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        if (lastCallbackMs > 0.0)
            gapsMs.push_back (nowMs - lastCallbackMs);
        lastCallbackMs = nowMs;
    }
};
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;   // Timer/MessageManager plumbing

    // Construction cost is NOT the regression surface (layout caches etc.);
    // the pump window starts after the editor exists. One-shot async setup
    // (combo rebuilds, ...) lands inside the window — acceptable: it is
    // exactly the kind of cost a regression budget should contain anyway.
    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);
    juce::AudioProcessorEditor* editor = proc.createEditor();
    check (editor != nullptr, "editor constructs headlessly");
    if (editor == nullptr)
        return 1;

    CongestionProbe probe;
    probe.startTimerHz (30);

    const auto endTimeMs = juce::Time::getMillisecondCounter() + (unsigned) (kRunSeconds * 1000);
    const double cpu0 = cpuTimeSecs();
    const auto   wall0 = juce::Time::getHighResolutionTicks();

    while (juce::Time::getMillisecondCounter() < endTimeMs)
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.020, false);

    const double cpuUsed = cpuTimeSecs() - cpu0;
    const double wallUsed = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - wall0);
    probe.stopTimer();

    auto& gaps = probe.gapsMs;
    std::sort (gaps.begin(), gaps.end());
    const double gapP99 = gaps.empty() ? 1e9 : gaps[static_cast<size_t> ((double) (gaps.size() - 1) * 0.99)];
    const double gapMax = gaps.empty() ? 1e9 : gaps.back();
    const int    nGaps  = (int) gaps.size();

    std::printf ("[perf smoke] %.1f s pumped run loop: cpu %.3f s (budget %.1f), probe gaps n=%d p99/max %.1f/%.1f ms (budgets %.0f/%.0f)\n",
                 wallUsed, cpuUsed, kCpuTimeBudgetSecs, nGaps, gapP99, gapMax, kGapP99BudgetMs, kGapMaxBudgetMs);

    check (cpuUsed <= kCpuTimeBudgetSecs, "total CPU time within budget");
    check (nGaps >= 30, "probe fired at a plausible rate (timers actually ran)");
    check (gapP99 <= kGapP99BudgetMs, "99th-percentile message-thread gap within budget");
    check (gapMax <= kGapMaxBudgetMs, "worst message-thread gap within budget");

    delete editor;
    std::printf ("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}

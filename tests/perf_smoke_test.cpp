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
// is JUCE_MODAL_LOOPS_PERMITTED-gated (compiled OUT of the Hellcat lib this
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
// re-derived is a budget nobody trusts). Loaded CI hosts without a code
// regression can scale every budget through HELLCAT_TEST_PERF_BUDGET_MULT
// (for example 2 doubles them; see budgetMultiplier below).
//
// What it cannot see: peer-driven repaint cost (no window = no paint) —
// that is what tools/profile_ui.sh covers as a LOCAL/manual gate (the
// CGEvent drag helper needs Accessibility permission, so it cannot run
// unattended on CI runners). See CONTRIBUTING.md.
//
// Run: ./build_unified/hellcat_unified_tests perf_smoke_test

// Non-Apple hosts cannot run this test: the pump drives the macOS
// CFRunLoop directly and the CPU budget reads getrusage. The single TEST()
// below branches on __APPLE__: the stub prints a skip note and returns
// true, so the unified target builds on Linux and Windows. The file keeps
// exactly one textual TEST() registration (build_policy_test counts them).
// (macOS behavior stays identical.)
#include "unified_test_runner.h"

#if ! __APPLE__

#include <cstdio>

#else

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
#include "ui/SettingsPanel.h"   // F-ios-perf-1 combo-count gate
#include "ui/ThemeManager.h"    // SettingsPanel ctor dependency

namespace
{
// ---- budgets (re-measure + update together; see file header) ----
constexpr int    kRunSeconds          = 10;
constexpr double kCpuTimeBudgetSecs   = 0.8;   // 10 s window, ~3x measured
constexpr double kGapP99BudgetMs      = 55.0;  // canary period ~44 ms + headroom
constexpr double kGapMaxBudgetMs      = 120.0; // no single stalled second

// Loaded CI hosts can exceed the reference wall-clock budgets without a
// code regression. HELLCAT_TEST_PERF_BUDGET_MULT scales every budget; 2
// doubles them. Values of zero or less fall back to 1.0.
double budgetMultiplier()
{
    const char* e = std::getenv ("HELLCAT_TEST_PERF_BUDGET_MULT");
    if (e == nullptr || e[0] == '\0')
        return 1.0;
    const double m = std::atof (e);
    return m > 0.0 ? m : 1.0;
}

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

// ---- F-ios-perf-1 (iOS hunt 2026-08-19) portable render-cost pins ----
// The absolute ms figures are machine-dependent; the portable contracts are
// (a) the RATIO: 2x oversampling must stay < 2.5x the 1x cost per block at
//     the 96-voice worst case (measured ~2.0x on Apple Silicon; a regression
//     that adds per-voice work independent of the OS factor collapses the
//     ratio toward 1, one that multiplies OS cost explodes it), and
// (b) the sanity ceiling: 1x at 96 voices must render below 0.5x realtime
//     on any CI-class machine (measured 0.13x on the reference box; the
//     generous margin only fails if per-voice cost regresses catastrophically).
struct WorstCaseRenderer
{
    explicit WorstCaseRenderer (HellcatAudioProcessor& p) : proc (p)
    {
        // 96-voice worst case: every part at 16 slots (6 x 16 = 96).
        for (int part = 0; part < 6; ++part)
            proc.getEngine().setPartVoiceSlots (part, 16);
        // Dense held chord per part (part p listens on channel p+1 by init).
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer midi;
        for (int part = 0; part < 6; ++part)
            for (int n = 0; n < 4; ++n)
                midi.addEvent (juce::MidiMessage::noteOn (part + 1, 48 + n * 5, (uint8_t) 110), n * 8);
        proc.processBlock (buf, midi);
    }
    // Mean CPU seconds per audio second at the given oversampling factor.
    double measure (int factor, int blocks)
    {
        proc.setOversamplingFactor (factor);
        juce::AudioBuffer<float> buf (2, 256);
        // Warm-up: the staged factor installs over the first block(s).
        for (int i = 0; i < 8; ++i) { buf.clear(); juce::MidiBuffer m; proc.processBlock (buf, m); }
        const double cpu0 = cpuTimeSecs();
        const auto t0 = juce::Time::getHighResolutionTicks();
        for (int i = 0; i < blocks; ++i) { buf.clear(); juce::MidiBuffer m; proc.processBlock (buf, m); }
        const double cpu = cpuTimeSecs() - cpu0;
        const double audioSecs = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
        (void) audioSecs;
        const double audioContent = (double) blocks * 256.0 / 48000.0;
        return cpu / audioContent;   // CPU-seconds per audio-second
    }
    HellcatAudioProcessor& proc;
};

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

TEST(perf_smoke_test)
{
#if ! __APPLE__
    std::printf ("  SKIPPED: perf_smoke_test is macOS-only (CFRunLoop pump)\n");
    return true;
#else
    juce::ScopedJuceInitialiser_GUI gui;   // Timer/MessageManager plumbing

    // Construction cost is NOT the regression surface (layout caches etc.);
    // the pump window starts after the editor exists. One-shot async setup
    // (combo rebuilds, ...) lands inside the window — acceptable: it is
    // exactly the kind of cost a regression budget should contain anyway.
    HellcatAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);
    juce::AudioProcessorEditor* editor = proc.createEditor();
    check (editor != nullptr, "editor constructs headlessly");
    if (editor == nullptr)
        return false;

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

    // Apply the host-load knob AFTER the measurement: the budgets scale, the
    // printed raw numbers stay comparable across hosts.
    const double mult    = budgetMultiplier();
    const double cpuBud  = kCpuTimeBudgetSecs * mult;
    const double p99Bud  = kGapP99BudgetMs * mult;
    const double maxBud  = kGapMaxBudgetMs * mult;

    auto& gaps = probe.gapsMs;
    std::sort (gaps.begin(), gaps.end());
    const double gapP99 = gaps.empty() ? 1e9 : gaps[static_cast<size_t> ((double) (gaps.size() - 1) * 0.99)];
    const double gapMax = gaps.empty() ? 1e9 : gaps.back();
    const int    nGaps  = (int) gaps.size();

    std::printf ("[perf smoke] %.1f s pumped run loop: cpu %.3f s (budget %.1f), probe gaps n=%d p99/max %.1f/%.1f ms (budgets %.0f/%.0f)\n",
                 wallUsed, cpuUsed, cpuBud, nGaps, gapP99, gapMax, p99Bud, maxBud);

    check (cpuUsed <= cpuBud, "total CPU time within budget");
    check (nGaps >= 30, "probe fired at a plausible rate (timers actually ran)");
    check (gapP99 <= p99Bud, "99th-percentile message-thread gap within budget");
    check (gapMax <= maxBud, "worst message-thread gap within budget");

    // ------------------------------------------------------------------
    // [F-ios-perf-1] 96-voice worst-case render-cost pins (portable ratios;
    // see WorstCaseRenderer). A state restore never widens scope here — the
    // factors are set directly through the public setter.
    // ------------------------------------------------------------------
    std::printf ("\n[F-ios-perf-1] 96-voice render-cost pins\n");
    {
        WorstCaseRenderer worst (proc);
        const double ratio1x = worst.measure (1, 400);
        const double ratio2x = worst.measure (2, 400);
        std::printf ("     96-voice cpu-per-audio-second: 1x %.3f, 2x %.3f (ratio %.2f)\n",
                     ratio1x, ratio2x, ratio1x > 0.0 ? ratio2x / ratio1x : -1.0);
        // The ABSOLUTE realtime budget is meaningless under sanitizers (ASan/
        // TSan instrument every access, 2-20x slowdown) — only the portable
        // RELATIVE pin (2x < 2.5 * 1x, same build) is asserted there.
      #if ! (defined(__has_feature) && __has_feature(address_sanitizer)) \
       && ! (defined(__has_feature) && __has_feature(thread_sanitizer)) \
       && ! defined(__SANITIZE_ADDRESS__) && ! defined(__SANITIZE_THREAD__)
        check (ratio1x < 0.5, "1x at 96 voices renders below 0.5x realtime (sanity)");
      #else
        std::printf ("     (sanitizer build: absolute-time sanity skipped)\n");
      #endif
        check (ratio1x > 0.0 && ratio2x < 2.5 * ratio1x,
               "2x cost stays under 2.5x the 1x cost (per-voice OS scaling)");
        // Restore the default before the editor is destroyed.
        proc.setOversamplingFactor (2);
    }

    // ------------------------------------------------------------------
    // [F-ios-perf-1] Settings Filter-Quality combo: iOS offers only 1x/2x
    // (audio-overrun gate); desktop keeps all four.
    // ------------------------------------------------------------------
    std::printf ("\n[F-ios-perf-1] oversampling combo item count gate\n");
    {
        ThemeManager themeManager;
        SettingsPanel panel (proc, themeManager, {}, {}, {}, {}, {});
        const juce::ComboBox* osCombo = nullptr;
        for (int i = 0; i < panel.getNumChildComponents(); ++i)
            if (auto* cb = dynamic_cast<const juce::ComboBox*> (panel.getChildComponent (i)))
                if (cb->getNumItems() > 0 && cb->getItemText (0).contains ("Standard"))
                    osCombo = cb;
        check (osCombo != nullptr, "Filter-Quality combo found in the settings panel");
        if (osCombo != nullptr)
        {
            const int n = osCombo->getNumItems();
            char msg[96];
#if JUCE_IOS
            std::snprintf (msg, sizeof (msg), "iOS combo lists exactly 2 items (got %d)", n);
            check (n == 2, msg);
#else
            std::snprintf (msg, sizeof (msg), "desktop combo lists all 4 items (got %d)", n);
            check (n == 4, msg);
#endif
        }
    }

    // ------------------------------------------------------------------
    // [F-ios-perf-1] state-restore clamp: a state SAVED at 8x/4x must restore
    // as 2x on iOS (audio-overrun gate); on desktop the persisted factor must
    // round-trip UNCHANGED (the clamp must never leak onto desktop).
    // ------------------------------------------------------------------
    std::printf ("\n[F-ios-perf-1] oversampling state-restore clamp\n");
    {
        HellcatAudioProcessor saver;
        saver.prepareToPlay (48000.0, 256);
        saver.setOversamplingFactor (8);
        juce::MemoryBlock blob;
        saver.getStateInformation (blob);
        HellcatAudioProcessor restored;
        restored.prepareToPlay (48000.0, 256);
        restored.setStateInformation (blob.getData(), (int) blob.getSize());
#if JUCE_IOS
        check (restored.getUiOversampling() == 2,
               "iOS: an 8x state restores clamped to 2x (dropout gate)");
#else
        check (restored.getUiOversampling() == 8,
               "desktop: an 8x state restores 8x (clamp stays iOS-only)");
#endif
    }

    // ------------------------------------------------------------------
    // [F-ios-perf-2] thermal decision function: the entire advisory policy
    // (pure mapping, no device needed).
    // ------------------------------------------------------------------
    std::printf ("\n[F-ios-perf-2] thermal decision mapping\n");
    {
        using P = HellcatAudioProcessor;
        check (P::thermalActionForLevel (P::ThermalLevel::Nominal)  == P::ThermalAction::None,
               "Nominal -> None");
        check (P::thermalActionForLevel (P::ThermalLevel::Fair)     == P::ThermalAction::None,
               "Fair -> None");
        check (P::thermalActionForLevel (P::ThermalLevel::Serious)  == P::ThermalAction::Hint,
               "Serious -> Hint");
        check (P::thermalActionForLevel (P::ThermalLevel::Critical) == P::ThermalAction::StrongHint,
               "Critical -> StrongHint");
        check (proc.getThermalHint() == (int) P::ThermalAction::None,
               "thermal hint starts at None (desktop never writes it)");
    }

    delete editor;
    std::printf ("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
#endif  // __APPLE__ (TEST body branch)
}

#endif  // __APPLE__ (file: helpers and includes)

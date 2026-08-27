// modbar_strip_theme_test — the live strip paints in EVERY theme (Y2K too).
//
// The CentralModBar pills carry a live sparkline (the history strip) that
// shows each modulation source's state. A theme must never hide it: the Y2K
// restyle routes every indicator through ONE accent, and the strip colour
// resolves through clusterAccent -> accentPrimary there — visible in theory,
// but nothing pinned it. This test feeds a REAL moving telemetry frame
// through the bar's own tick path, renders the bar offscreen, and counts
// STRONGLY-COLOURED pixels inside the LFO 1 pill's strip rect — once per
// built-in theme. A theme that paints the strip invisible (alpha, fill
// order, a chrome cover) or not at all fails here.
//
// macOS-only: the tick pump drives the CFRunLoop (the perf_smoke_test
// pattern; JUCE 9 gates runDispatchLoopUntil out of this lib).

#include "unified_test_runner.h"

#if ! __APPLE__

#include <cstdio>

#else

#include <CoreFoundation/CoreFoundation.h>

#include <cstdio>
#include <vector>

#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "dsp/patch.h"                  // MOD_SRC_LFO_1 / MOD_SRC_PITCH_BEND
#include "ui/CentralModBar.h"
#include "ui/ModTelemetryTypes.h"
#include "ui/ThemeManager.h"

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "test_utils.h"                 // renderBlocks / setParam

namespace
{
int g_failures = 0;
void check (bool cond, const juce::String& msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg.toRawUTF8());
    if (! cond) ++g_failures;
}

// Pumps the macOS main run loop so juce::Timer callbacks (the bar's 60 Hz
// telemetry fallback) really fire. See perf_smoke_test.cpp for why this is
// the correct headless pump for this lib.
void pumpMs (int ms)
{
    const auto until = juce::Time::getMillisecondCounter() + (uint32_t) ms;
    while (juce::Time::getMillisecondCounter() < until)
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false);
}

// Collects every descendant (depth-first).
void collectAll (juce::Component* c, std::vector<juce::Component*>& out)
{
    if (c == nullptr) return;
    out.push_back (c);
    for (int i = 0; i < c->getNumChildComponents(); ++i)
        collectAll (c->getChildComponent (i), out);
}

// Finds the pill whose tooltip matches @p name, returns its index (-1 when
// absent) — the discovery idiom of modbar_pill_paint_split_test.
int pillIndexOf (CentralModBar& bar, const juce::String& tooltip)
{
    std::vector<juce::Component*> all;
    collectAll (&bar, all);
    for (auto* c : all)
        if (auto* tc = dynamic_cast<juce::SettableTooltipClient*> (c))
            if (tc->getTooltip() == tooltip)
            {
                // Correlate the component with a bar pill index.
                for (int i = 0; i < 512; ++i)
                    if (bar.pillComponentForTest (i) == c)
                        return i;
            }
    return -1;
}

// The strip child's bounds in BAR coordinates (accumulated through the whole
// parent chain — the pills sit inside the bar's scrolled Viewport).
juce::Rectangle<int> stripRectInBar (CentralModBar& bar, int idx)
{
    auto* strip = bar.pillStripChildForTest (idx);
    if (strip == nullptr)
        return {};
    juce::Point<int> chain {};
    for (auto* c = strip; c != nullptr && c != &bar; c = c->getParentComponent())
        chain += c->getPosition();
    return strip->getBounds().withPosition (chain);
}

// Counts pixels in @p rect that are STRONGLY coloured (saturation above
// 0.25 and clearly brighter than the pill's dark data-cell fill): the
// sparkline stroke + its band. A painted-but-invisible strip returns ~0.
int colouredPixels (const juce::Image& img, const juce::Rectangle<int>& rect)
{
    int n = 0;
    for (int y = rect.getY(); y < rect.getBottom(); ++y)
        for (int x = rect.getX(); x < rect.getRight(); ++x)
        {
            const auto p = img.getPixelAt (x, y);
            const float sat = p.getSaturation();
            const float lum = p.getPerceivedBrightness();
            if (sat > 0.25f && lum > 0.15f)
                ++n;
        }
    return n;
}
}  // namespace

TEST(modbar_strip_theme_test)
{
    juce::ScopedJuceInitialiser_GUI gui;

    ThemeManager themes;
    CentralModBar bar (themes);
    bar.setBounds (0, 0, 900, CentralModBar::kBarHeight);
    bar.setVisible (true);

    const int lfoIdx = pillIndexOf (bar, "LFO 1");
    check(lfoIdx >= 0, "the LFO 1 pill is discoverable");
    if (lfoIdx < 0)
        return g_failures == 0;

    // A REAL moving frame: a square wave on LFO 1 (full swing, bipolar) and
    // a resting-mid Pitch Bend, history full, epoch valid, head advancing so
    // the ring reads "audio flowing". Same epoch every fetch => valid frames.
    int head = 0;
    const auto fetch = [&] (hellcat::ModTelemetrySnapshot& s)
    {
        s = {};
        s.epoch = 7;
        s.part  = 0;
        s.historyCount = hellcat::ModTelemetrySnapshot::kHistoryLen;
        head = (head + 12) % hellcat::ModTelemetrySnapshot::kHistoryLen;
        s.historyHead = head;
        for (int i = 0; i < hellcat::ModTelemetrySnapshot::kHistoryLen; ++i)
        {
            const uint8_t v = ((i / 24) % 2 == 0) ? 10 : 245;   // square, slow
            s.history[(size_t) ambika::dsp::MOD_SRC_LFO_1 * hellcat::ModTelemetrySnapshot::kHistoryLen
                      + (size_t) i] = v;
        }
        s.sources[(size_t) ambika::dsp::MOD_SRC_LFO_1] = 128;
        return true;
    };

    bar.setTelemetryProvider (fetch);
    bar.setTelemetryRateHz (60);

    // Let the 60 Hz fallback Timer fetch + repaint a few times.
    pumpMs (300);
    check(bar.telemetryGeneration() > 0,
          "the telemetry tick ran and the strip data changed at least once");

    // THE point: render in EVERY built-in theme and require the sparkline to
    // paint visible pixels. selectByName drives the real selection path; the
    // bar re-resolves its accents through applyThemeColors (the workspace
    // theme-switch hook).
    const auto names = themes.getThemeNames();
    check(names.size() >= 5, "the built-in theme list is populated");
    for (const auto& name : names)
    {
        check(themes.selectByName (name), "theme '" + name + "' selects");
        bar.applyThemeColors();
        pumpMs (120);   // a fresh tick under the new theme

        juce::Image img (juce::Image::ARGB, bar.getWidth(), bar.getHeight(), true);
        {
            juce::Graphics g (img);
            bar.paintEntireComponent (g, false);
        }
        const auto rect = stripRectInBar (bar, lfoIdx);
        const int px = colouredPixels (img, rect);
        juce::String extra;
        if (px < 30)
            extra << " (rect " << rect.toString() << ", strip paints "
                  << bar.pillStripPaintCountForTest (lfoIdx) << ")";
        check(px >= 30,
              "theme '" + name + "': the LFO 1 sparkline paints visible pixels ("
                  + juce::String (px) + ")" + extra);
    }

    std::printf ("\nMODBAR STRIP THEME TEST: %s (%d failure%s)\n",
                 g_failures == 0 ? "PASS" : "FAIL", g_failures,
                 g_failures == 1 ? "" : "s");
    return g_failures == 0;
}

// ---- [2] The REAL editor + REAL engine, per theme ----
// The bar-level section [1] pins the painting in isolation. This section
// drives the exact app path: processor + editor, live audio (telemetry
// flowing), the editor's own theme-switch seam, and the bar found INSIDE the
// live tree. The strip must animate visibly in every theme here too.
namespace {
CentralModBar* findModBar (juce::Component* c)
{
    if (auto* bar = dynamic_cast<CentralModBar*> (c)) return bar;
    for (int i = 0; i < c->getNumChildComponents(); ++i)
        if (auto* r = findModBar (c->getChildComponent (i))) return r;
    return nullptr;
}

// A 30 Hz probe: proves the message loop delivers Timer callbacks while the
// audio pump runs (the perf_smoke_test idiom).
struct ProbeTimer : private juce::Timer
{
    std::atomic<int> ticks { 0 };
    ProbeTimer() { startTimerHz (30); }
    ~ProbeTimer() override { stopTimer(); }
    void timerCallback() override { ++ticks; }
};
}  // namespace

TEST(modbar_strip_editor_live_test)
{
    juce::ScopedJuceInitialiser_GUI gui;

    HellcatAudioProcessor processor;
    processor.prepareToPlay (48000.0, 256);
    // A fast free-running LFO 1 (env1_lfo_rate ~ 15 Hz at byte 100): the
    // strip swings hard inside the pump window.
    setParam (processor, "env1_lfo_rate", 100);
    processor.syncAllParamsToEngine();

    auto* ed = processor.createEditor();
    check(ed != nullptr, "the editor is created");
    if (ed == nullptr) return false;
    auto* editor = dynamic_cast<HellcatEditor*> (ed);
    auto* bar = findModBar (ed);
    check(bar != nullptr, "a CentralModBar lives in the editor tree");
    if (bar == nullptr || editor == nullptr) return false;

    // LAUNCH STATE (t=0): constructed editor, NO audio, NO pump. The strips
    // start with no history (stripCount_ == 0) — the rest line must paint in
    // the FIRST render, before telemetry has ever flowed (the app-launch
    // look, incl. a state-restored patch).
    {
        const int idx0 = [&]
        {
            for (int i = 0; i < 512; ++i)
            {
                auto* c = bar->pillComponentForTest (i);
                if (c == nullptr) break;
                if (auto* tc = dynamic_cast<juce::SettableTooltipClient*> (c))
                    if (tc->getTooltip() == "LFO 1") return i;
            }
            return -1;
        }();
        check(idx0 >= 0, "the LFO 1 pill is discoverable at t=0");
        if (idx0 >= 0)
        {
            juce::Image img0 (juce::Image::ARGB, ed->getWidth(), ed->getHeight(), true);
            {
                juce::Graphics g (img0);
                ed->paintEntireComponent (g, false);
            }
            auto* strip0 = bar->pillStripChildForTest (idx0);
            juce::Point<int> chain0 {};
            for (auto* c = strip0; c != nullptr && c != ed; c = c->getParentComponent())
                chain0 += c->getPosition();
            const auto r0 = strip0->getBounds().withPosition (chain0);
            int px0 = 0;
            for (int y = r0.getY(); y < r0.getBottom(); ++y)
                for (int x = r0.getX(); x < r0.getRight(); ++x)
                {
                    const auto p = img0.getPixelAt (x, y);
                    if (p.getSaturation() > 0.25f && p.getPerceivedBrightness() > 0.15f)
                        ++px0;
                }
            check(px0 >= 30,
                  "at launch (no telemetry yet) the LFO 1 strip paints its rest line ("
                      + juce::String (px0) + " px)");
        }
    }

    // A held note: the engine renders, telemetry appends, sources move.
    FakePlayHead transport (120.0, true);
    processor.setPlayHead (&transport);
    juce::AudioBuffer<float> audio (2, 256);
    juce::MidiBuffer noteOn;
    juce::MidiBuffer none;
    noteOn.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
    for (int b = 0; b < 30; ++b)
        processor.processBlock (audio, b == 0 ? noteOn : none);

    const int lfoIdx = [&]
    {
        for (int i = 0; i < 512; ++i)
        {
            auto* c = bar->pillComponentForTest (i);
            if (c == nullptr) break;
            if (auto* tc = dynamic_cast<juce::SettableTooltipClient*> (c))
                if (tc->getTooltip() == "LFO 1") return i;
        }
        return -1;
    }();
    check(lfoIdx >= 0, "the live editor bar exposes the LFO 1 pill");
    if (lfoIdx < 0) return false;

    // One warm-up pump + audio so the hub/timers run before diagnosing.
    ProbeTimer probe;
    for (int slice = 0; slice < 10; ++slice)
    {
        for (int b = 0; b < 6; ++b)
            processor.processBlock (audio, none);
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false);
    }
    // A valid engine frame before the theme sweep: the strip data source is
    // alive (history flowing) independent of any theme.
    {
        hellcat::ModTelemetrySnapshot frame;
        check(processor.getEngine().readUiTelemetry (frame) && frame.historyCount > 0,
              "the engine telemetry frame is valid and carries history");
    }

    for (const auto& name : { "Carbon", "Y2K" })
    {
        check(editor->switchThemeSynchronousForTest (name),
              "theme '" + juce::String (name) + "' switches live");
        // Keep audio + the message loop running together: render in slices
        // so the hub/bar timers fire between blocks (a 15 Hz LFO needs
        // several fetches to build a visible band).
        for (int slice = 0; slice < 24; ++slice)
        {
            for (int b = 0; b < 6; ++b)
                processor.processBlock (audio, none);
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false);
        }
        // PUMP LIVENESS: the pixel check is only meaningful if Timer
        // callbacks were delivered during the pump (returnAfterSourceHandled
        // must be FALSE — a true-flavoured slice starves the JUCE queue and
        // the test would pass vacuously with every strip dead).
        check(probe.ticks.load() > 0,
              juce::String ("theme '") + name + "': the pump delivered timer callbacks ("
                  + juce::String (probe.ticks.load()) + ")");
        check(bar->telemetryGeneration() > 0,
              juce::String ("theme '") + name + "': the bar's telemetry tick produced data");

        juce::Image img (juce::Image::ARGB, ed->getWidth(), ed->getHeight(), true);
        {
            juce::Graphics g (img);
            ed->paintEntireComponent (g, false);
        }
        // The strip rect in EDITOR coordinates (chain-accumulate to the editor).
        auto* strip = bar->pillStripChildForTest (lfoIdx);
        juce::Point<int> chain {};
        for (auto* c = strip; c != nullptr && c != ed; c = c->getParentComponent())
            chain += c->getPosition();
        const auto rect = strip->getBounds().withPosition (chain);
        int px = 0;
        for (int y = rect.getY(); y < rect.getBottom(); ++y)
            for (int x = rect.getX(); x < rect.getRight(); ++x)
            {
                const auto p = img.getPixelAt (x, y);
                if (p.getSaturation() > 0.25f && p.getPerceivedBrightness() > 0.15f)
                    ++px;
            }
        check(px >= 30,
              juce::String ("theme '") + name + "': the LIVE editor's LFO 1 strip "
              "paints visible pixels (" + juce::String (px) + ", rect "
              + rect.toString() + ")");
    }

    processor.setPlayHead (nullptr);
    delete ed;
    std::printf ("\nMODBAR STRIP EDITOR LIVE TEST: %s (%d failure%s)\n",
                 g_failures == 0 ? "PASS" : "FAIL", g_failures,
                 g_failures == 1 ? "" : "s");
    return g_failures == 0;
}

// ---- [3] THE LAUNCH SCENARIO ------------------------------------------------
// The user report: right after app launch (a state-restored session) the
// strips/envelopes do not react to notes until a preset is loaded. This
// drives exactly that sequence: editor + setStateInformation, audio, one
// note, pump — then asserts the LFO 1 strip CARRIED LIVE MOTION (not just
// the rest line: the env generators must append non-flat history).
TEST(modbar_strip_launch_test)
{
    juce::ScopedJuceInitialiser_GUI gui;

    HellcatAudioProcessor saver;
    saver.prepareToPlay (48000.0, 256);
    setParam (saver, "env1_lfo_rate", 100);   // ~15 Hz LFO 1
    saver.syncAllParamsToEngine();
    juce::MemoryBlock state;
    saver.getStateInformation (state);

    HellcatAudioProcessor processor;   // the "launched app": fresh, then restored
    processor.prepareToPlay (48000.0, 256);
    processor.setStateInformation (state.getData(), (int) state.getSize());

    auto* ed = processor.createEditor();
    check(ed != nullptr, "launch: the editor is created");
    if (ed == nullptr) return false;
    auto* bar = findModBar (ed);
    check(bar != nullptr, "launch: a CentralModBar lives in the tree");
    if (bar == nullptr) { delete ed; return false; }

    // Wait for the restore's resetUiTelemetry to be serviced: render some
    // blocks + pump so the audio thread wipes and the hub sees a valid frame.
    juce::AudioBuffer<float> audio (2, 256);
    juce::MidiBuffer none;
    {
        FakePlayHead transport (120.0, true);
        processor.setPlayHead (&transport);
        for (int slice = 0; slice < 12; ++slice)
        {
            for (int b = 0; b < 8; ++b)
                processor.processBlock (audio, none);
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false);
        }
    }
    {
        hellcat::ModTelemetrySnapshot frame;
        const bool valid = processor.getEngine().readUiTelemetry (frame);
        std::printf ("  launch: frame valid=%d count=%d\n", (int) valid, frame.historyCount);
        check(valid, "launch: the telemetry frame is valid after restore + audio");
    }

    // PLAY A NOTE (no preset load anywhere) with LFO 1 armed from the start.
    {
        juce::MidiBuffer noteOn;
        noteOn.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
        processor.processBlock (audio, noteOn);
    }
    for (int slice = 0; slice < 24; ++slice)
    {
        for (int b = 0; b < 6; ++b)
            processor.processBlock (audio, none);
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false);
    }

    // The strip must have MOVED: the telemetry generation advanced beyond
    // the rest line (an append-driven change), and the LFO 1 history carries
    // a non-flat window (a 15 Hz square swings 10..245).
    {
        hellcat::ModTelemetrySnapshot frame;
        const bool valid = processor.getEngine().readUiTelemetry (frame);
        check(valid && frame.historyCount > 16,
              "launch: after a NOTE the history is flowing (count "
                  + juce::String (valid ? frame.historyCount : -1) + ")");
        if (valid)
        {
            int lo = 255, hi = 0;
            const uint8_t* hist = frame.history
                + (size_t) ambika::dsp::MOD_SRC_LFO_1 * hellcat::ModTelemetrySnapshot::kHistoryLen;
            for (int i = 0; i < frame.historyCount; ++i)
            {
                lo = juce::jmin (lo, (int) hist[(size_t) i]);
                hi = juce::jmax (hi, (int) hist[(size_t) i]);
            }
            check(hi - lo > 100,
                  "launch: the LFO 1 strip SWINGS with the note (min " + juce::String (lo)
                      + ", max " + juce::String (hi) + ")");
        }
    }

    processor.setPlayHead (nullptr);
    delete ed;
    std::printf ("\nMODBAR STRIP LAUNCH TEST: %s (%d failure%s)\n",
                 g_failures == 0 ? "PASS" : "FAIL", g_failures,
                 g_failures == 1 ? "" : "s");
    return g_failures == 0;
}

// ---- [4] PURE FRESH LAUNCH: NO state restore, NO preset load ---------------
// The user report: at APP LAUNCH the strips/envelopes do NOT react to a
// note until a preset is loaded. A preset load re-points telemetry via
// resetUiTelemetry + setUiTelemetryPart. A PURE launch must ALSO track: the
// editor ctor wires the provider + part, and first blocks must push live
// history WITHOUT any restore/load illusion serving as a re-point.
TEST(modbar_strip_fresh_launch_test)
{
    juce::ScopedJuceInitialiser_GUI gui;

    HellcatAudioProcessor processor;   // pure fresh app: nothing restored, nothing loaded
    processor.prepareToPlay (48000.0, 256);
    auto* ed = processor.createEditor();
    check(ed != nullptr, "fresh-launch: the editor is created");
    if (ed == nullptr) return false;
    auto* bar = findModBar (ed);
    check(bar != nullptr, "fresh-launch: a CentralModBar lives in the tree");
    if (bar == nullptr) { delete ed; return false; }

    juce::AudioBuffer<float> audio (2, 256);
    juce::MidiBuffer none;
    {
        FakePlayHead transport (120.0, true);
        processor.setPlayHead (&transport);
        // Warm the audio thread so the editor's setUiTelemetryPart lands and
        // the frame services this part (no restore to do the re-pointing).
        for (int slice = 0; slice < 12; ++slice)
        {
            for (int b = 0; b < 8; ++b)
                processor.processBlock (audio, none);
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false);
        }
    }
    {
        hellcat::ModTelemetrySnapshot frame;
        const bool valid = processor.getEngine().readUiTelemetry (frame);
        std::printf ("  fresh-launch: frame valid=%d count=%d\n", (int) valid, frame.historyCount);
        check(valid, "fresh-launch: the telemetry frame is valid on a PURE launch");
    }

    // Play a note on the fresh-launch instance.
    {
        juce::MidiBuffer noteOn;
        noteOn.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
        processor.processBlock (audio, noteOn);
    }
    // Probe DURING the attack: the env observables (written every block while
    // a voice is active) must show a triggered envelope. The user's complaint
    // is precisely these amp / mod env markers not responding at launch.
    {
        hellcat::ModTelemetrySnapshot frame;
        bool sawVoice = false;
        float peakLevel = 0.0f;
        for (int probe = 0; probe < 8; ++probe)
        {
            for (int b = 0; b < 3; ++b)
                processor.processBlock (audio, none);
            if (processor.getEngine().readUiTelemetry (frame))
            {
                sawVoice = sawVoice || frame.voiceActive;
                for (int e = 0; e < 3; ++e)
                    peakLevel = juce::jmax (peakLevel, frame.envLevel[(size_t) e]);
            }
        }
        check(sawVoice, "fresh-launch: a voice is ACTIVE with the note (env markers live)");
        check(peakLevel > 0.05f,
              "fresh-launch: an ENVELOPE triggered with the note (peak level "
              + juce::String (peakLevel) + ")");
    }
    for (int slice = 0; slice < 24; ++slice)
    {
        for (int b = 0; b < 6; ++b)
            processor.processBlock (audio, none);
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false);
    }
    {
        hellcat::ModTelemetrySnapshot frame;
        const bool valid = processor.getEngine().readUiTelemetry (frame);
        check(valid && frame.historyCount > 16,
              "fresh-launch: after a NOTE history flows WITHOUT any load (count "
                  + juce::String (valid ? frame.historyCount : -1) + ")");
        if (valid)
        {
            int lo = 255, hi = 0;
            const uint8_t* hist = frame.history
                + (size_t) ambika::dsp::MOD_SRC_LFO_1 * hellcat::ModTelemetrySnapshot::kHistoryLen;
            for (int i = 0; i < frame.historyCount; ++i)
            {
                lo = juce::jmin (lo, (int) hist[(size_t) i]);
                hi = juce::jmax (hi, (int) hist[(size_t) i]);
            }
            check(hi - lo > 100,
                  "fresh-launch: the LFO 1 strip SWINGS on a PURE launch (min "
                      + juce::String (lo) + ", max " + juce::String (hi) + ")");
        }
    }

    // The USER-VISIBLE surface: the painted strip must show MOTION, not just
    // the engine frame. Drive the UI tick (the same provider / strip paint
    // the running app uses) and count LFO 1 pixels — a live square wave fills
    // far more than the ~52 px rest line.
    ed->setVisible (true);
    bar->runTelemetryTickForTest();
    {
        const int idx0 = [&]
        {
            for (int i = 0; i < 512; ++i)
            {
                auto* c = bar->pillComponentForTest (i);
                if (c == nullptr) break;
                if (auto* tc = dynamic_cast<juce::SettableTooltipClient*> (c))
                    if (tc->getTooltip() == "LFO 1") return i;
            }
            return -1;
        }();
        check(idx0 >= 0, "fresh-launch: the LFO 1 pill is resolvable after tick");
        if (idx0 >= 0)
        {
            juce::Image img0 (juce::Image::ARGB, ed->getWidth(), ed->getHeight(), true);
            {
                juce::Graphics g (img0);
                ed->paintEntireComponent (g, false);
            }
            auto* strip0 = bar->pillStripChildForTest (idx0);
            juce::Point<int> chain0 {};
            for (auto* c = strip0; c != nullptr && c != ed; c = c->getParentComponent())
                chain0 += c->getPosition();
            const auto r0 = strip0->getBounds().withPosition (chain0);
            int px0 = 0;
            for (int y = r0.getY(); y < r0.getBottom(); ++y)
                for (int x = r0.getX(); x < r0.getRight(); ++x)
                {
                    const auto p = img0.getPixelAt (x, y);
                    if (p.getSaturation() > 0.25f && p.getPerceivedBrightness() > 0.15f)
                        ++px0;
                }
            check(px0 >= 80,
                  "fresh-launch: the PAINTED LFO 1 strip shows motion ("
                  + juce::String (px0) + " px, rest line is ~52)");
        }
    }

    processor.setPlayHead (nullptr);
    delete ed;
    std::printf ("\nMODBAR STRIP FRESH LAUNCH TEST: %s (%d failure%s)\n",
                 g_failures == 0 ? "PASS" : "FAIL", g_failures,
                 g_failures == 1 ? "" : "s");
    return g_failures == 0;
}

// ---- [5] WAKE-UP FROM A FULL FLAT WINDOW (the launch/stale-home freeze) ----
// The reported bug: at launch the mod pills show NO feedback on a note until
// a preset is switched. Root cause found: the strip FLAT-SKIP fast path reads
// delta = historyCount - stripRawCount_, but historyCount is PINNED at
// kWindow once the ring is FULL — so a flat-at-rest strip (LFO/ENV at zero
// before the first note) sees delta==0 every tick and NEVER recomputes. It
// freezes on its rest line until a preset-switch wipe re-arms it. This test
// fills the ring to the brim with rest zeros (arming the fast path), then
// plays a note and demands the painted strip wakes up to live motion.
TEST(modbar_strip_wake_from_full_flat_test)
{
    juce::ScopedJuceInitialiser_GUI gui;

    HellcatAudioProcessor processor;   // fresh app
    processor.prepareToPlay (48000.0, 256);
    auto* ed = processor.createEditor();
    check(ed != nullptr, "wake-test: the editor is created");
    if (ed == nullptr) return false;
    auto* bar = findModBar (ed);
    check(bar != nullptr, "wake-test: a CentralModBar lives in the tree");
    if (bar == nullptr) { delete ed; return false; }

    juce::AudioBuffer<float> audio (2, 256);
    juce::MidiBuffer none;
    {
        FakePlayHead transport (120.0, true);
        processor.setPlayHead (&transport);
        // Warm up a FULL, STABLE window of rest zeros so the strip's FLAT-SKIP
        // fast path is armed (stripFlat_=true, count pinned -> delta==0). This
        // is the app's state at launch: history filled, sources idle at rest.
        int reachedFull = -1;
        int flatSlices = 0;
        for (int slice = 0; slice < 320 && flatSlices < 30; ++slice)
        {
            for (int b = 0; b < 5; ++b)
                processor.processBlock (audio, none);
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false);
            hellcat::ModTelemetrySnapshot probe;
            if (processor.getEngine().readUiTelemetry (probe))
            {
                if (probe.historyCount == reachedFull)
                    ++flatSlices;          // count stable across probes = idle-full
                else
                {
                    flatSlices = 0;
                    reachedFull = probe.historyCount;
                }
            }
        }
        check(reachedFull > 32,
              "wake-test: the window went idle-full (count " + juce::String (reachedFull)
                  + ", stable " + juce::String (flatSlices) + " probes)");
        if (reachedFull <= 32) { processor.setPlayHead (nullptr); delete ed; return static_cast<bool> (g_failures == 0); }
    }

    // PLAY A NOTE (no preset load anywhere).
    {
        juce::MidiBuffer noteOn;
        noteOn.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
        processor.processBlock (audio, noteOn);
    }
    for (int slice = 0; slice < 30; ++slice)
    {
        for (int b = 0; b < 5; ++b)
            processor.processBlock (audio, none);
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false);
    }

    // The PAINTED LFO 1 strip must have woken up: a live square wave fills
    // far more than the ~52 px rest line. Before the fix this stayed frozen
    // (the flat-skip fast path returned false every tick once the ring was
    // full) — the exact reported failure.
    ed->setVisible (true);
    bar->runTelemetryTickForTest();
    {
        const int idx0 = [&]
        {
            for (int i = 0; i < 512; ++i)
            {
                auto* c = bar->pillComponentForTest (i);
                if (c == nullptr) break;
                if (auto* tc = dynamic_cast<juce::SettableTooltipClient*> (c))
                    if (tc->getTooltip() == "LFO 1") return i;
            }
            return -1;
        }();
        check(idx0 >= 0, "wake-test: the LFO 1 pill is resolvable");
        if (idx0 >= 0)
        {
            juce::Image img0 (juce::Image::ARGB, ed->getWidth(), ed->getHeight(), true);
            {
                juce::Graphics g (img0);
                ed->paintEntireComponent (g, false);
            }
            auto* strip0 = bar->pillStripChildForTest (idx0);
            juce::Point<int> chain0 {};
            for (auto* c = strip0; c != nullptr && c != ed; c = c->getParentComponent())
                chain0 += c->getPosition();
            const auto r0 = strip0->getBounds().withPosition (chain0);
            int px0 = 0;
            for (int y = r0.getY(); y < r0.getBottom(); ++y)
                for (int x = r0.getX(); x < r0.getRight(); ++x)
                {
                    const auto p = img0.getPixelAt (x, y);
                    if (p.getSaturation() > 0.25f && p.getPerceivedBrightness() > 0.15f)
                        ++px0;
                }
            check(px0 >= 80,
                  "wake-test: the PAINTED LFO 1 strip woke from the full-flat freeze ("
                  + juce::String (px0) + " px, rest line is ~52)");
        }
    }

    processor.setPlayHead (nullptr);
    delete ed;
    std::printf ("\nMODBAR STRIP WAKE FROM FULL-FLAT TEST: %s (%d failure%s)\n",
                 g_failures == 0 ? "PASS" : "FAIL", g_failures,
                 g_failures == 1 ? "" : "s");
    return g_failures == 0;
}

#endif  // __APPLE__

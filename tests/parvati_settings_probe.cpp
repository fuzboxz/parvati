// Settings drawer visual probe (2026-08-22, third-attempt fix): opens the
// REAL drawer on a desktop-attached editor, dumps the content-tree bounds
// (SidePanel -> Viewport -> SettingsPanel -> every row child), and (with
// PARVATI_TEST_SHOTS=1, mirroring PARVATI_TEST_HOLD) saves PNG snapshots to
// the temp directory (settings_shots/) for inspection. Scenarios: default
// 1280x634, short 1280x500, after theme switch, after close+reopen.
#include <cstdio>
#include "unified_test_runner.h"
#include <typeinfo>
#include <memory>
#include <string>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

#include "PluginEditor.h"
#include "test_utils.h"              // displayAvailable (headless-host skip)
#include "PluginProcessor.h"
#include "ui/SettingsPanel.h"

namespace
{
void pump (double sec)
{
#ifdef __APPLE__
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    while (juce::Time::getMillisecondCounterHiRes() - t0 < sec * 1000.0)
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.040, false);
#else
    juce::Thread::sleep ((int) (sec * 1000));
#endif
}

void dumpTree (juce::Component* c, int depth, juce::String& out)
{
    if (c == nullptr) return;
    juce::String pad;
    for (int i = 0; i < depth; ++i) pad += "  ";
    out += pad + c->getName() + " [" + typeid (*c).name () + "] "
         + "bounds=" + c->getBounds().toString()
         + (c->isVisible() ? "" : " HIDDEN") + "\n";
    for (auto* ch : c->getChildren())
        dumpTree (ch, depth + 1, out);
}

void snapshot (juce::Component& c, const juce::String& name)
{
    // Opt-in only: writing screenshots on every suite run pollutes the
    // machine and makes this probe's footprint machine-state dependent.
    if (juce::SystemStats::getEnvironmentVariable ("PARVATI_TEST_SHOTS", "0") != "1")
        return;
    // Route through juce::File::tempDirectory: the path follows the
    // per-lane TMPDIR that tools/run_tests_parallel.sh sets, and Windows
    // resolves a real temp tree instead of the current drive root.
    const juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("settings_shots");
    dir.createDirectory();
    auto img = c.createComponentSnapshot (c.getLocalBounds());
    juce::File f (dir.getChildFile (name + ".png"));
    juce::FileOutputStream os (f);
    juce::PNGImageFormat().writeImageToStream (img, os);
}
} // namespace

TEST(parvati_settings_probe)
{
    setEnvVar ("PARVATI_HEADLESS", "1");
    // The probe drives a real on-desktop window (native title bar + peer);
    // a headless host cannot create one. macOS always reports a display.
    if (! displayAvailable())
    {
        std::printf ("  SKIP: no display server (headless host)\n");
        return true;
    }
    juce::ScopedJuceInitialiser_GUI gui;

    const bool interactive = juce::SystemStats::getEnvironmentVariable ("PARVATI_TEST_HOLD", "0") == "1";

    auto proc = std::make_unique<ParvatiAudioProcessor>();
    proc->prepareToPlay (48000.0, 256);
    auto* ed = dynamic_cast<ParvatiEditor*> (proc->createEditor());
    if (ed == nullptr) { std::printf ("FAIL: no editor\n"); return false; }

    auto win = std::make_unique<juce::DocumentWindow> ("SettingsProbe",
        juce::Colours::black, juce::DocumentWindow::allButtons);
    win->setUsingNativeTitleBar (true);
    win->setContentNonOwned (ed, false);
    win->centreWithSize (1280, 634);
    win->addToDesktop (juce::ComponentPeer::windowAppearsOnTaskbar);
    win->setVisible (true);
    pump (0.30);

    for (int scenario = 0; scenario < 4; ++scenario)
    {
        const char* label = "default634";
        if (scenario == 1) { label = "short500";   ed->setSize (1280, 500); pump (0.20); }
        if (scenario == 2) { label = "short-theme"; proc->setUiTheme ("Immutable"); proc->syncAllParamsToEngine(); pump (0.25); }
        if (scenario == 3) { label = "reopen500";  ed->openSettingsForTest(); ed->openSettingsForTest(); pump (0.20); }
        // (re)open the drawer for every scenario through the REAL path
        ed->openSettingsForTest();

        // OPEN-LATENCY REGRESSION (2026-08-22): the panel must be sized
        // SYNCHRONOUSLY at the open call — before the slide animation's proxy
        // snapshot — or the drawer slides in blank and the content pops at the
        // end (the reported first-open latency). No pumping before this check.
        if (auto* content = ed->settingsContentForTest())
            if (auto* vp = dynamic_cast<juce::Viewport*> (content))
                if (auto* panel = vp->getViewedComponent())
                {
                    char m[96];
                    std::snprintf (m, sizeof (m),
                                   "panel sized at open call, pre-animation (%dx%d)",
                                   panel->getWidth(), panel->getHeight());
                    const bool ok = panel->getWidth() > 0 && panel->getHeight() > 0;
                    if (! ok) { win->setVisible (false); delete ed; return false; }
                    // (checked silently — printed only via the census below)
                }

        pump (0.60);   // let the slide animation + tracker settle

        juce::String dump;
        dump += juce::String ("=== scenario: ") + label + " (editor "
              + juce::String (ed->getWidth()) + "x" + juce::String (ed->getHeight()) + ") ===\n";
        dumpTree (ed, 0, dump);
        // also the panel's own children summary (visible-row census)
        if (auto* content = ed->settingsContentForTest())
        {
            int vis = 0, hid = 0;
            juce::Array<juce::Component*> nodes { content };
            for (int i = 0; i < nodes.size(); ++i)
                for (auto* ch : nodes.getUnchecked (i)->getChildren()) nodes.add (ch);
            for (auto* c : nodes)
                if (c->getName().isNotEmpty())
                { if (c->isVisible() && c->getWidth() > 0) ++vis; else ++hid; }
            dump += juce::String ("content census: visible=") + juce::String (vis)
                  + " hidden/zero=" + juce::String (hid) + "\n";
            // 2026-08-22: the REGRESSION GATE. This probe used to return true
            // unconditionally — while the drawer rendered BLANK (every row
            // 0×0; the tracker's getViewWidth() gate was circularly starved
            // by the 0×0 panel it was supposed to size). Two invariants now
            // FAIL the probe: (1) the VIEWED PANEL itself must be sized
            // (non-zero bounds) once the drawer shows, and (2) no NAMED
            // child may sit at 0×0 (a collapsed/mis-laid-out row). Most rows
            // are unnamed Labels/ComboBoxes, so vis counts only the named
            // buttons/toggles (>= 5 when laid out).
            const bool panelSized = [&]
            {
                if (auto* vp = dynamic_cast<juce::Viewport*> (content))
                    if (auto* panel = vp->getViewedComponent())
                        return panel->getWidth() > 0 && panel->getHeight() > 0;
                return false;
            }();
            if (! panelSized || hid != 0 || vis < 5)
            {
                dump += "FAIL: settings drawer content not displayed (panelSized="
                      + juce::String (panelSized ? 1 : 0) + " visible=" + juce::String (vis)
                      + " collapsed=" + juce::String (hid) + ") — panel sizing broken\n";
                std::printf ("%s\n", dump.toRawUTF8 ());
                win->setVisible (false);
                delete ed;
                return false;
            }
        }
        std::printf ("%s\n", dump.toRawUTF8 ());
        snapshot (*ed, label);
        // SCROLL GATE (2026-08-22 "language row unreachable" regression): the
        // drawer's content is taller than the view at this window size, so
        // (a) the vertical scrollbar must be VISIBLE after settling (JUCE's
        // auto-bar early-return path can leave it hidden; the editor's 30 Hz
        // tick re-asserts it — wheel scrolling also works unconditionally via
        // allowScrollingWithoutScrollbar), and (b) scrolling to the bottom
        // must bring the LAST row fully into view.
        if (auto* content = ed->settingsContentForTest())
            if (auto* vp = dynamic_cast<juce::Viewport*> (content))
                if (auto* panel = vp->getViewedComponent())
                {
                    const int over = panel->getHeight() - vp->getViewHeight();
                    vp->setViewPosition (juce::Point<int> (0, 1 << 20));   // scroll to bottom
                    pump (0.10);   // one editor tick: the 30 Hz bar re-assert settles
                    const int maxY = vp->getViewPositionY();
                    const bool barOk = vp->getVerticalScrollBar().isVisible();
                    std::printf ("[scroll] panelH=%d viewH=%d overflow=%d vbarVisible=%d maxScrollY=%d\n",
                                 panel->getHeight(), vp->getViewHeight(), over, (int) barOk, maxY);
                    juce::Component* last = nullptr;
                    for (auto* c : panel->getChildren())
                        if (c->getBottom() > 0 && (last == nullptr || c->getBottom() > last->getBottom()))
                            last = c;
                    const bool reachable = last != nullptr
                        && (last->getBottom() - vp->getViewPositionY()) <= vp->getViewHeight() + 1;
                    if (! (over > 0 && maxY == over && barOk && reachable))
                    {
                        std::printf ("FAIL: drawer scroll broken (over=%d maxY=%d bar=%d reachable=%d lastBottom=%d)\n",
                                     over, maxY, (int) barOk, (int) reachable,
                                     last != nullptr ? last->getBottom() : -1);
                        win->setVisible (false);
                        delete ed;
                        return false;
                    }
                }
    }

    if (interactive)
    {
        std::printf ("holding 10 s for manual look...\n");
        pump (10.0);
    }
    win->setVisible (false);
    delete ed;
    std::printf ("PROBE DONE (PNG shots in /tmp/settings_shots only when PARVATI_TEST_SHOTS=1)\n");
    return true;
}

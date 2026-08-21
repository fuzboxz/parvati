// Settings drawer visual probe (2026-08-22, third-attempt fix): opens the
// REAL drawer on a desktop-attached editor, dumps the content-tree bounds
// (SidePanel -> Viewport -> SettingsPanel -> every row child), and saves PNG
// snapshots to /tmp/settings_shots/ for inspection. Scenarios: default
// 1280x634, short 1280x500, after theme switch, after close+reopen.
#include <cstdio>
#include "unified_test_runner.h"
#include <typeinfo>
#include <filesystem>
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
    std::filesystem::create_directories ("/tmp/settings_shots");
    auto img = c.createComponentSnapshot (c.getLocalBounds());
    juce::File f ("/tmp/settings_shots/" + name + ".png");
    juce::FileOutputStream os (f);
    juce::PNGImageFormat().writeImageToStream (img, os);
}
} // namespace

TEST(parvati_settings_probe)
{
    ::setenv ("PARVATI_HEADLESS", "1", 1);
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
        if (scenario == 2) { label = "short-theme"; proc->setUiTheme ("Paper"); proc->syncAllParamsToEngine(); pump (0.25); }
        if (scenario == 3) { label = "reopen500";  ed->openSettingsForTest(); ed->openSettingsForTest(); pump (0.20); }
        // (re)open the drawer for every scenario through the REAL path
        ed->openSettingsForTest();
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
        }
        std::printf ("%s\n", dump.toRawUTF8 ());
        snapshot (*ed, label);
    }

    if (interactive)
    {
        std::printf ("holding 10 s for manual look...\n");
        pump (10.0);
    }
    win->setVisible (false);
    delete ed;
    std::printf ("PROBE DONE (shots in /tmp/settings_shots)\n");
    return true;
}

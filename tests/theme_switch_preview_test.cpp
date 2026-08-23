// theme_switch_preview_test — waveform previews must keep updating after a
// theme switch (the reported bug: "after changing the theme the preview no
// longer updates for waveform preview").
//
// Drives the REAL theme path (ParvatiEditor::selectThemeForTest ->
// ThemeManager::selectByName -> change broadcast -> applyAllColoursFromTheme)
// and pins, for the osc waveform preview AND the LFO waveform preview:
//   * the 30 Hz poll timer is still running after the switch,
//   * a shape change still bumps previewGeneration (the preview reacts).
#include <cstdio>

#include "test_utils.h"
#include "unified_test_runner.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "ui/EnvelopeDisplay.h"
#include "dsp/patch.h"   // MOD_SRC_LFO_1
#include "ui/OscPreviewDisplay.h"

namespace
{
void pump (double sec)
{
#ifdef __APPLE__
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    while (juce::Time::getMillisecondCounterHiRes() - t0 < sec * 1000.0)
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.020, false);
#else
    juce::Thread::sleep ((int) (sec * 1000.0));
#endif
}
} // namespace

TEST(theme_switch_preview_test)
{
    juce::ScopedJuceInitialiser_GUI gui;

    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);
    proc.syncAllParamsToEngine();
    auto* ed = dynamic_cast<ParvatiEditor*> (proc.createEditor());
    CHECK(ed != nullptr, "editor created");
    if (ed == nullptr)
        return false;
    // The preview polls need a real peer; a headless host cannot create one.
    // macOS always reports a display, so the guard stays dormant there.
    if (! displayAvailable())
    {
        std::printf ("  SKIP: no display server (headless host)\n");
        return true;
    }
    ed->addToDesktop (juce::ComponentPeer::windowAppearsOnTaskbar);
    ed->setVisible (true);
    pump (0.3);

    // Locate the osc + LFO waveform previews through the generated pages.
    OscPreviewDisplay* osc1 = nullptr;
    EnvelopeDisplay* lfo1 = nullptr;
    for (auto* page : ed->allGeneratedPages())
    {
        if (osc1 == nullptr)
            if (auto* c = dynamic_cast<OscPreviewDisplay*> (page->getGroupInlinePreviewForTest ("Osc 1")))
                osc1 = c;
        if (lfo1 == nullptr)
            if (auto* c = dynamic_cast<EnvelopeDisplay*> (page->getGroupDecorationForTest ("LFO 1")))
                lfo1 = c;
    }
    CHECK(osc1 != nullptr, "Osc 1 waveform preview found");
    CHECK(lfo1 != nullptr, "LFO 1 waveform preview found");
    if (osc1 == nullptr || lfo1 == nullptr)
    { delete ed; return false; }

    // REAL FLOW: the LFO preview lives on the LFO page (swapped through the
    // workspace Viewport on demand) — open it as the active generator page
    // exactly as clicking the LFO 1 pill does, so its displays are on screen.
    if (auto* ws = ed->getSynthWorkspaceForTest())
        ws->setActiveGenerator (ambika::dsp::MOD_SRC_LFO_1);
    pump (0.4);

    // Baseline: both polls running and both previews react to a shape change.
    setChoice (proc, "osc1_shape", 2);   // square
    setChoice (proc, "env1_lfo_shape", 3);   // ramp
    pump (0.4);
    const int oscGen0 = osc1->previewGeneration();
    const int lfoGen0 = lfo1->previewGeneration();
    CHECK(oscGen0 > 0, "osc preview reacts to shape change BEFORE the theme switch");
    CHECK(lfoGen0 > 0, "LFO preview reacts to shape change BEFORE the theme switch");

    // ---- THE REAL THEME SWITCH — with the SETTINGS DRAWER OPEN, exactly as
    // the user drives it (theme combo lives in the drawer), then closed ----
    ed->openSettingsForTest();
    pump (0.5);
    CHECK(ed->selectThemeForTest ("Paper"), "theme switched to Paper (drawer open)");
    pump (0.5);
    ed->openSettingsForTest();   // toggle closed (the gear button path)
    pump (0.5);

    char m[128];
    std::snprintf (m, sizeof (m), "osc preview poll still running after theme switch (running=%d)",
                   (int) osc1->isPollRunningForTest());
    CHECK(osc1->isPollRunningForTest(), m);
    std::snprintf (m, sizeof (m), "LFO preview poll still running after theme switch (running=%d)",
                   (int) lfo1->isPollRunningForTest());
    CHECK(lfo1->isPollRunningForTest(), m);

    // Shape changes must STILL land after the switch.
    const int oscGen1 = osc1->previewGeneration();
    const int lfoGen1 = lfo1->previewGeneration();
    setChoice (proc, "osc1_shape", 4);       // sine
    setChoice (proc, "env1_lfo_shape", 1);   // square
    pump (0.4);
    std::snprintf (m, sizeof (m), "osc preview reacts after theme switch (gen %d -> %d)",
                   oscGen1, osc1->previewGeneration());
    CHECK(osc1->previewGeneration() > oscGen1, m);
    std::snprintf (m, sizeof (m), "LFO preview reacts after theme switch (gen %d -> %d)",
                   lfoGen1, lfo1->previewGeneration());
    CHECK(lfo1->previewGeneration() > lfoGen1, m);

    // And back to the default theme — same contract.
    ed->selectThemeForTest ("Carbon");
    pump (0.3);
    const int oscGen2 = osc1->previewGeneration();
    setChoice (proc, "osc1_shape", 1);   // saw
    pump (0.4);
    std::snprintf (m, sizeof (m), "osc preview reacts after switching back (gen %d -> %d)",
                   oscGen2, osc1->previewGeneration());
    CHECK(osc1->previewGeneration() > oscGen2, m);

    ed->removeFromDesktop();
    delete ed;
    return true;
}

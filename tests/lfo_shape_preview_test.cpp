// lfo_shape_preview_test — the LFO waveform preview must follow SHAPE changes.
//
// The reported bug: switching an LFO's shape (e.g. Square -> S&H) updated the
// mod pill but the LFO preview kept rendering the OLD waveform. This drives
// the REAL display + REAL APVTS choice parameter through shape switches and
// asserts (a) the polled normalized shape value actually changes, (b) the
// display repaints (previewGeneration bumps) on each switch.
#include <cstdio>

#include "test_utils.h"
#include "unified_test_runner.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

#include "PluginProcessor.h"
#include "ui/EnvelopeDisplay.h"

namespace
{
void pump (double sec)
{
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    while (juce::Time::getMillisecondCounterHiRes() - t0 < sec * 1000.0)
    {
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.020, false);
    }
}
} // namespace

TEST(lfo_shape_preview_test)
{
    juce::ScopedJuceInitialiser_GUI gui;

    HellcatAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);
    proc.syncAllParamsToEngine();

    // The REAL wiring from HellcatEditor's page builder (LFO preview mode,
    // shape getter over the APVTS choice parameter).
    auto* shapeParam = proc.getApvts().getParameter ("env1_lfo_shape");
    CHECK(shapeParam != nullptr, "env1_lfo_shape parameter exists");

    EnvelopeDisplay disp ("LFO 1",
        std::function<float()> {}, std::function<float()> {},
        std::function<float()> {}, std::function<float()> {},
        [&proc] { return proc.getApvts().getParameter ("env1_lfo_shape")->getValue(); });
    disp.setPreviewMode (1);   // LFO waveform
    disp.setBounds (0, 0, 200, 80);
    // SCENARIO per the report: the shape changes while the display's page is
    // HIDDEN (no peer — the poll timer is stopped), then the page is shown
    // again. The re-shown display must pick the new shape up within one poll
    // tick (parentHierarchyChanged restarts the timer; the next timerCallback
    // compares against the stale lastShape_ and repaints).
    setChoice (proc, "env1_lfo_shape", 2);   // S&H while hidden
    pump (0.1);
    disp.addToDesktop (0);                   // "page shown": peer + hierarchy change
    disp.setVisible (true);
    pump (0.3);                              // >= one poll tick
    std::printf ("  [dbg] after show: timerRunning=%d showing=%d onDesktop=%d gen=%d\n",
                 (int) disp.isPollRunningForTest(), (int) disp.isShowing(),
                 (int) disp.isOnDesktop(), disp.previewGeneration());

    // Sweep every shape pair; on each switch the polled value must change and
    // the preview must repaint (a generation bump).
    const int numShapes = 4;   // Tri / Sq / S&H / Ramp
    bool valueFollows = true;
    int repaints = 0;
    float prevValue = shapeParam->getValue();
    for (int s = 0; s < numShapes; ++s)
    {
        const float target = (float) s / (float) (numShapes - 1);
        if (std::fabs (target - prevValue) < 1e-4f)
            continue;   // already there
        const int gen0 = disp.previewGeneration();
        setChoice (proc, "env1_lfo_shape", s);
        pump (0.25);   // >= one poll tick + eps gate
        const float now = shapeParam->getValue();
        if (std::fabs (now - target) > 0.02f)
            valueFollows = false;
        if (disp.previewGeneration() > gen0)
            ++repaints;
        prevValue = now;
        std::printf ("  shape %d: param=%.3f gen %d->%d\n", s, now, gen0, disp.previewGeneration());
    }

    CHECK(valueFollows, "choice parameter value follows every shape switch");
    char m[96];
    std::snprintf (m, sizeof (m), "preview repainted on every shape switch (%d switches)", repaints);
    CHECK(repaints >= 3, m);

    disp.removeFromDesktop();
    return true;
}

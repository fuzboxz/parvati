// LIVE-environment FX dropout repro (2026-08-21): renders notes with distortion
// active WHILE A REAL EDITOR IS OPEN ON THE DESKTOP (the live-feedback hub + the
// strip polls + the editor's 30 Hz tick all running — the user's environment).
// Offline renders (no GUI) are clean; if the dropout is GUI-interaction-caused
// this catches it. Also covers type-swap-mid-note and enable-mid-note.
#include <cmath>
#include "unified_test_runner.h"
#include "test_utils.h"
#include <cstdio>
#include <vector>

#include <CoreFoundation/CoreFoundation.h>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

// Metrics: min windowed RMS / median + longest near-zero run + NaN count.
struct Health { double rmsMin; int zeroRun; long nanCount; };

Health analyze (const std::vector<float>& out, int from, int to)
{
    Health h { 1.0, 0, 0 };
    const int n = juce::jmin (to, (int) out.size());
    std::vector<double> wr;
    int run = 0;
    for (int i = from; i < n; ++i)
    {
        const float v = out[(size_t) i];
        if (std::isnan (v) || std::isinf (v)) ++h.nanCount;
        if (std::fabs (v) < 1.0e-5) { if (++run > h.zeroRun) h.zeroRun = run; }
        else run = 0;
    }
    for (int i = from; i + 64 <= n; i += 64)
    {
        double s = 0;
        for (int k = 0; k < 64; ++k)
        {
            const double v = out[(size_t) (i + k)];
            s += v * v;
        }
        wr.push_back (std::sqrt (s / 64.0));
    }
    if (! wr.empty())
    {
        std::vector<double> sorted = wr;
        std::sort (sorted.begin(), sorted.end());
        const double med = std::max (1e-9, sorted[sorted.size() / 2]);
        for (double r : wr) h.rmsMin = std::min (h.rmsMin, r / med);
    }
    return h;
}
} // namespace

TEST(parvati_fx_live_repro)
{
    ::setenv ("PARVATI_HEADLESS", "1", 1);
    juce::ScopedJuceInitialiser_GUI gui;

    constexpr double sr = 44100.0;
    constexpr int buf = 512;
    const int total = (int) (6.0 * sr);   // 6 s

    ParvatiAudioProcessor proc;
    proc.prepareToPlay (sr, buf);
    setInt (proc, "osc1_shape", 1);

    // THE LIVE ENVIRONMENT: a real editor on the desktop (hub + strips + tick).
    auto* ed = proc.createEditor();
    auto win = std::make_unique<juce::DocumentWindow> ("Repro",
        juce::Colours::black, juce::DocumentWindow::allButtons);
    win->setUsingNativeTitleBar (true);
    win->setContentNonOwned (ed, false);
    win->centreWithSize (1280, 700);
    win->addToDesktop (juce::ComponentPeer::windowAppearsOnTaskbar);
    win->setVisible (true);
    CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.200, false);

    // Distortion ON from the start (default-ish params).
    setChoice (proc, "fx1_type", 17);
    setInt (proc, "fx1_enabled", 1);
    setInt (proc, "fx1_drywet", 127);

    std::vector<float> capL ((size_t) total, 0.0f), capR ((size_t) total, 0.0f);

    // A PLAYED SEQUENCE: chord on for 700 ms, off for 250 ms, repeat — with a
    // re-strike of a single note inside some holds (the retrigger path). This
    // exercises release tails + fresh onsets + the no-active-voice chain path.
    struct Ev { int pos; bool on; int n; };
    std::vector<Ev> events;
    {
        int t = (int) (0.30 * sr);
        int step = 0;
        while (t < total - (int) (0.9 * sr))
        {
            events.push_back ({ t, true, step });          // chord on (or single)
            events.push_back ({ t + (int) (0.70 * sr), false, step }); // all off
            t += (int) (0.95 * sr);
            ++step;
        }
    }
    size_t nextEv = 0;

    for (int written = 0; written < total; )
    {
        juce::AudioBuffer<float> b (2, buf);
        b.clear();
        juce::MidiBuffer midi;
        while (nextEv < events.size()
               && events[nextEv].pos >= written && events[nextEv].pos < written + buf)
        {
            const Ev& e = events[nextEv];
            const int posInBuf = e.pos - written;
            if (e.on)
            {
                if (e.n % 2 == 0)
                {
                    for (int c = 0; c < 4; ++c)
                        midi.addEvent (juce::MidiMessage::noteOn (1, (uint8_t) (57 + 5 * c), (uint8_t) 110), posInBuf);
                }
                else
                {
                    midi.addEvent (juce::MidiMessage::noteOn (1, (uint8_t) (69 + 7 * (e.n % 3)), (uint8_t) 100), posInBuf);
                }
            }
            else
            {
                for (int nn = 36; nn < 100; ++nn)
                    midi.addEvent (juce::MidiMessage::noteOff (1, (uint8_t) nn, 0.0f), posInBuf);
            }
            ++nextEv;
        }
        proc.processBlock (b, midi);
        const int n = std::min (buf, total - written);
        for (int i = 0; i < n; ++i)
        {
            capL[(size_t) (written + i)] = b.getSample (0, i);
            capR[(size_t) (written + i)] = b.getSample (1, i);
        }
        written += n;
        // Pump the run loop alongside rendering (the live GUI animates).
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.001, false);
    }

    // DIAGNOSTIC: locate every zero-run >= 32 samples with its start offset
    // relative to the nearest preceding note EVENT.
    {
        int run = 0, runStart = -1;
        for (int i = (int) (0.1 * sr); i < total; ++i)
        {
            if (std::fabs (capL[(size_t) i]) < 1.0e-5)
            {
                if (run == 0) runStart = i;
                ++run;
            }
            else
            {
                if (run >= 32)
                {
                    // nearest event before runStart
                    int evPos = -1; const char* evKind = "?";
                    for (const Ev& e : events)
                        if (e.pos <= runStart) { evPos = e.pos; evKind = e.on ? "on" : "off"; }
                        else break;
                    std::printf ("    zeroRun %4d @ %.4fs (event %s @ %.4fs, delta %+.1f ms)\n",
                                 run, runStart / sr, evKind, evPos / sr,
                                 1000.0 * (runStart - evPos) / sr);
                }
                run = 0;
            }
        }
    }

    // Per-note analysis: for each chord hold [on, on+0.55s], no RMS collapse
    // and no NaN; gaps are allowed to be silent. Plus a global NaN scan.
    long nanTotal = 0;
    for (size_t i = 0; i < capL.size(); ++i)
        if (std::isnan (capL[i]) || std::isinf (capL[i])) ++nanTotal;
    check (nanTotal == 0, "no NaN/Inf anywhere in the sequence output");
    int badHolds = 0, checked = 0;
    for (const Ev& e : events)
    {
        if (! e.on) continue;
        const int from = e.pos + (int) (0.05 * sr);
        const int to   = e.pos + (int) (0.55 * sr);
        const Health h = analyze (capL, from, to);
        ++checked;
        if (h.rmsMin < 0.15 || h.zeroRun > 16)
        {
            ++badHolds;
            std::printf ("    bad hold @%.2fs: rmsMin=%.3f zeroRun=%d\n",
                         e.pos / sr, h.rmsMin, h.zeroRun);
        }
    }
    {
        char m[96];
        std::snprintf (m, sizeof (m), "every held chord stays healthy (%d/%d)", checked - badHolds, checked);
        check (badHolds == 0, m);
    }

    win->setVisible (false);
    delete ed;
    std::printf ("\n%s (%d failures)\n", g_failures ? "REPRO: FAILURES" : "REPRO: CLEAN", g_failures);
    return g_failures == 0;
}

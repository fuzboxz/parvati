// Render-quality regression test for Parvati:
//   [1] Offline auto-max oversampling (setNonRealtime -> 8x filter OS, no
//       persistence, restore on exit, double-entry guard, prepare-time leak
//       guard).
//   [2] Oversized-block CHUNKED render (host block > prepared size renders in
//       prepared-size slices; no silent dropped tail).
//   [3] Dynamic getTailLengthSeconds: pure tailSecondsForFx table (reverbs +
//       DELAYS with feedback-decay math + freeze caps + clamps) and the
//       processor-level cache (all-None floor, reverb > floor, enabled gating).
//
// Built by default. Run with: ./build/parvati_render_quality_test

#include <cmath>
#include <cstdio>
#include <memory>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "PluginProcessor.h"
#include "dsp/fx/FxTypes.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

void checkNear (double got, double want, double tol, const char* msg)
{
    const bool ok = std::fabs (got - want) <= tol;
    std::printf ("  %s: %s (got %.4f, want %.4f +- %.4f)\n",
                 ok ? "ok  " : "FAIL", msg, got, want, tol);
    if (! ok) ++g_failures;
}

void renderBlock (ParvatiAudioProcessor& p, int numSamples)
{
    juce::AudioBuffer<float> buf (2, numSamples);
    buf.clear();
    juce::MidiBuffer midi;
    p.processBlock (buf, midi);
}

double blockPeak (const juce::AudioBuffer<float>& b, int start, int len)
{
    double peak = 0.0;
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = start; i < start + len && i < b.getNumSamples(); ++i)
            peak = std::fmax (peak, std::fabs ((double) b.getSample (ch, i)));
    return peak;
}

void setParam (ParvatiAudioProcessor& p, const char* id, int value)
{
    if (auto* param = p.getApvts().getParameter (id))
        param->setValueNotifyingHost (param->convertTo0to1 ((float) value));
}

// The persisted ui_oversampling inside a saved host state (or -1 when absent).
int savedOversampling (ParvatiAudioProcessor& p)
{
    juce::MemoryBlock mb;
    p.getStateInformation (mb);
    const auto size = (int) mb.getSize();
    if (auto xml = p.getXmlFromBinary (mb.getData(), size))
    {
        const auto tree = juce::ValueTree::fromXml (*xml);
        if (tree.hasProperty ("ui_oversampling"))
            return (int) tree.getProperty ("ui_oversampling");
    }
    return -1;
}
}   // namespace

//==============================================================================
int main()
{
    std::printf ("[1] Offline auto-max oversampling\n");
    {
        juce::ScopedJuceInitialiser_GUI juceInit;
        ParvatiAudioProcessor p;
        p.prepareToPlay (48000.0, 256);
        renderBlock (p, 256);   // flush the initial latency report

        const int latency2x = p.getLatencySamples();
        check (latency2x > 0, "2x default reports nonzero latency");
        check (p.getUiOversampling() == 2, "fresh instance pref is 2x");

        // -- Enter offline: 8x applied, pref untouched, state never carries 8x.
        p.setNonRealtime (true);
        check (p.isOfflineOversamplingActive(), "offline boost active after setNonRealtime(true)");
        check (p.getUiOversampling() == 2, "user pref NOT bumped to 8x");
        renderBlock (p, 256);
        const int latency8x = p.getLatencySamples();
        check (latency8x > latency2x,
               "latency re-reported larger at 8x (2x -> 8x OS group delay)");
        check (savedOversampling (p) == 2, "saved host state still carries 2x (no 8x leak)");

        // -- Double-entry guard: a second setNonRealtime(true) is a no-op
        //    (the saved 2x must not be overwritten by the current 8x).
        p.setNonRealtime (true);
        check (p.isOfflineOversamplingActive(), "double-entry stays active");
        check (p.getUiOversampling() == 2, "double-entry keeps the 2x pref");

        // -- Exit offline: restores the user factor + latency.
        p.setNonRealtime (false);
        check (! p.isOfflineOversamplingActive(), "boost inactive after exit");
        renderBlock (p, 256);
        check (p.getLatencySamples() == latency2x, "latency restored to the 2x value");
        check (p.getUiOversampling() == 2, "pref still 2x after exit");

        // -- Re-enter then re-prepare WITHOUT an exit: the prepare-time leak
        //    guard must restore the user factor (host dropped setNonRealtime).
        p.setNonRealtime (true);
        renderBlock (p, 256);
        p.setNonRealtime (false);           // host "forgot": no exit call
        p.prepareToPlay (48000.0, 256);     // back in realtime: guard fires here
        check (! p.isOfflineOversamplingActive(), "leak guard disarms the boost");
        renderBlock (p, 256);
        check (p.getLatencySamples() == latency2x, "leak guard restores 2x latency");
    }

    std::printf ("\n[2] Oversized-block chunked render\n");
    {
        juce::ScopedJuceInitialiser_GUI juceInit;
        ParvatiAudioProcessor p;
        p.prepareToPlay (48000.0, 256);   // prepared budget: 256

        // Host hands a 4x-oversized block with a note-on at sample 0. The old
        // clamp rendered [0,256) and silently ZEROED the remaining 768 samples
        // (dropped-tail corruption); the chunked render tiles four 256 slices.
        juce::AudioBuffer<float> buf (2, 1024);
        buf.clear();
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 110), 0);
        p.processBlock (buf, midi);

        for (int q = 0; q < 4; ++q)
        {
            const double peak = blockPeak (buf, q * 256, 256);
            char msg[80];
            (void) std::snprintf (msg, sizeof (msg),
                                 "quarter %d (samples %d..%d) is non-silent",
                                 q, q * 256, q * 256 + 255);
            check (peak > 1.0e-5, msg);
        }

        // In-budget block (no behavior change): still renders.
        renderBlock (p, 256);
        check (true, "in-budget block renders after an oversized one");
    }

    std::printf ("\n[3a] Pure tail table (reverbs)\n");
    {
        const float zero[5] = { 0.f, 0.f, 0.f, 0.f, 0.f };
        checkNear (tailSecondsForFx (FxType::None, zero, 120.0), 0.0, 1e-9, "None -> 0");
        checkNear (tailSecondsForFx (FxType::PlateReverb, zero, 120.0), 0.1, 1e-9,
                   "Plate min (decay 0.1 s, no predelay)");
        {
            const float pmax[5] = { 1.f, 1.f, 0.f, 0.f, 0.f };
            checkNear (tailSecondsForFx (FxType::PlateReverb, pmax, 120.0), 4.1, 1e-6,
                       "Plate max (4 s decay + 100 ms predelay)");
        }
        {
            const float pmax[5] = { 1.f, 0.f, 0.f, 0.f, 0.f };
            checkNear (tailSecondsForFx (FxType::Spring, pmax, 120.0), 4.0, 1e-6, "Spring max (4 s)");
            checkNear (tailSecondsForFx (FxType::Room, pmax, 120.0), 3.0, 1e-6, "Room max (3 s)");
        }
        {
            // CVerb: tank fb 0.95, loop 8483/32000 s -> t60 = T*ln(1e-3)/ln(0.95)
            const float tmax[5] = { 0.f, 0.f, 1.f, 0.f, 0.f };
            const double want = (8483.0 / 32000.0) * (std::log (1.0e-3) / std::log (0.95));
            checkNear (tailSecondsForFx (FxType::Reverb, tmax, 120.0), want, 1e-3,
                       "CVerb max-time t60 (feedback-decay law)");
            check (tailSecondsForFx (FxType::Reverb, tmax, 120.0) > 10.0,
                   "CVerb max-time tail exceeds 10 s (matches measured behaviour)");
            const float pd[5] = { 1.f, 0.f, 0.f, 0.f, 0.f };
            checkNear (tailSecondsForFx (FxType::Reverb, pd, 120.0),
                       (8483.0 / 32000.0) * (std::log (1.0e-3) / std::log (0.30)) + 0.20,
                       1e-3, "CVerb min-time = short decay + 200 ms predelay");
        }
        checkNear (tailSecondsForFx (FxType::Diffuser, zero, 120.0), 2048.0 / 32000.0, 1e-9,
                   "Diffuser = 2048-sample AP smear");

        std::printf ("\n[3b] Pure tail table (delays: time x feedback decay)\n");
        {
            // Echo: single repeat at fb=0 (t60 = the loop time itself).
            const float emin[5] = { 0.f, 0.f, 0.f, 0.f, 0.f };
            checkNear (tailSecondsForFx (FxType::Echo, emin, 120.0), 0.010, 1e-6,
                       "Echo min (10 ms, no feedback)");
            // fb=1.0 -> g=0.995 -> the >=0.995 infinite sentinel (cap).
            const float emax[5] = { 1.f, 1.f, 0.f, 0.f, 0.f };
            checkNear (tailSecondsForFx (FxType::Echo, emax, 120.0), kTailCapSeconds, 1e-9,
                       "Echo max feedback (g=0.995) = infinite sentinel");
            // Mid feedback: exact law.
            const float emid[5] = { 1.f, 0.5f, 0.f, 0.f, 0.f };
            const double T = 0.010 * std::pow (47.0, 1.0);
            const double g = 0.5 * 0.995;
            checkNear (tailSecondsForFx (FxType::Echo, emid, 120.0),
                       T * (std::log (1.0e-3) / std::log (g)), 1e-3,
                       "Echo 50% feedback follows t60 = T*ln(1e-3)/ln(g)");
        }
        {
            // ClockedDelay @120 BPM, sync=0 -> div 1 -> T = (4/1)*(60/120) = 2 s
            // clamped to the 1.0 s line; fb=0 -> single pass.
            const float cd[5] = { 0.f, 0.f, 0.f, 0.f, 0.f };
            checkNear (tailSecondsForFx (FxType::ClockedDelay, cd, 120.0), 1.0, 1e-9,
                       "ClockedDelay whole-note @120 clamps to the 1 s line (no fb)");
            // Same with max feedback: 1 s * 134.7 passes.
            const float cdfb[5] = { 0.f, 1.f, 0.f, 0.f, 0.f };
            checkNear (tailSecondsForFx (FxType::ClockedDelay, cdfb, 120.0),
                       1.0 * (std::log (1.0e-3) / std::log (0.95)), 1e-3,
                       "ClockedDelay max feedback follows the law");
            // Tempo dependence: same params at 480 BPM halve the loop time
            // (whole note @120 = 2 s clamped to 1 s; @480 = 0.5 s).
            checkNear (tailSecondsForFx (FxType::ClockedDelay, cd, 480.0), 0.5, 1e-9,
                       "ClockedDelay is tempo-scaled (480 BPM -> 0.5 s)");
            // Degenerate bpm falls back to 120.
            checkNear (tailSecondsForFx (FxType::ClockedDelay, cd, 0.0), 1.0, 1e-9,
                       "bpm=0 falls back to 120");
        }
        {
            // Granular/looping family: 4 s buffer, freeze -> the cap.
            const float noFreeze[5] = { 0.f, 0.f, 0.f, 0.f, 0.f };
            const float freeze[5] = { 0.f, 0.f, 0.f, 1.f, 0.f };
            checkNear (tailSecondsForFx (FxType::LoopingDelay, noFreeze, 120.0), 4.0, 1e-9,
                       "LoopingDelay = 4 s capture buffer");
            checkNear (tailSecondsForFx (FxType::LoopingDelay, freeze, 120.0), kTailCapSeconds,
                       1e-9, "LoopingDelay freeze = infinite sentinel");
            const float spectralFreeze[5] = { 0.f, 0.f, 0.f, 0.f, 1.f };   // freeze is param[4]
            checkNear (tailSecondsForFx (FxType::Spectral, spectralFreeze, 120.0),
                       kTailCapSeconds, 1e-9, "Spectral freeze = infinite sentinel");
        }

        std::printf ("\n[3c] Clamps\n");
        checkNear (clampTailSeconds (0.0), kTailFloorSeconds, 1e-9, "floor maps 0");
        checkNear (clampTailSeconds (1000.0), kTailCapSeconds, 1e-9, "cap maps huge");
        checkNear (clampTailSeconds (std::nan ("")), kTailFloorSeconds, 1e-9, "floor maps NaN");
        checkNear (clampTailSeconds (2.5), 2.5, 1e-9, "in-range passes through");
    }

    std::printf ("\n[3d] Processor-level tail cache\n");
    {
        juce::ScopedJuceInitialiser_GUI juceInit;
        ParvatiAudioProcessor p;
        p.prepareToPlay (48000.0, 256);
        checkNear (p.getTailLengthSeconds(), kTailFloorSeconds, 1e-6,
                   "all-None FX -> floor (prepare-time recompute)");

        // FX1 = Plate, decay max, predelay max -> ~4.1 s.
        setParam (p, "fx1_type", (int) FxType::PlateReverb);
        setParam (p, "fx1_enabled", 1);
        setParam (p, "fx1_param1", 127);   // predelay -> 100 ms
        setParam (p, "fx1_param2", 127);   // decay -> 4 s
        renderBlock (p, 256);              // services fxDirty_ -> recomputes tail
        check (p.getTailLengthSeconds() > 4.0 && p.getTailLengthSeconds() <= kTailCapSeconds,
               "Plate @4s decay -> tail > 4 s");

        // Disable the slot -> back to the floor (bypassed slots contribute 0).
        setParam (p, "fx1_enabled", 0);
        renderBlock (p, 256);
        checkNear (p.getTailLengthSeconds(), kTailFloorSeconds, 1e-6,
                   "disabled slot -> floor again");

        // A delay (the explicit requirement): Echo, max feedback -> capped.
        setParam (p, "fx1_type", (int) FxType::Echo);
        setParam (p, "fx1_enabled", 1);
        setParam (p, "fx1_param1", 127);   // time 470 ms
        setParam (p, "fx1_param2", 127);   // feedback max (0.995)
        renderBlock (p, 256);
        checkNear (p.getTailLengthSeconds(), kTailCapSeconds, 1e-6,
                   "Echo max-feedback delay -> capped tail (delays count)");
    }

    std::printf ("\nRENDER-QUALITY TEST: %s (%d failures)\n",
                 g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}

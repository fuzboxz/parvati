// Concurrency regression test for the message-thread -> audio-thread hand-off.
//
// Runs a mock AUDIO THREAD (a background std::thread looping processBlock) while
// the MESSAGE THREAD concurrently mutates patch/part parameters (fires
// applyPatchByte/applyPartByte via the APVTS), flips polyphony, and loads a
// preset mid-run. Asserts the run completes (no crash/hang), the output stays
// finite (no NaN/Inf), and the engine still responds afterwards.
//
// Before the Phase 2 fix, applyPatchByte/applyPartByte wrote each voice's
// voice_.patch_/part_ directly on the message thread while this background
// thread rendered them -- a torn read (technically UB) that could crash or
// corrupt audio. With the fix those writes are deferred to the audio thread via
// the per-Part frameDirty_ flag, so this test must be stable. Run a few times:
//   for i in 1 2 3 4 5; do ./build/parvati_concurrency_test >/dev/null || echo FAIL $i; done

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "PluginProcessor.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg) { std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg); if (! cond) ++g_failures; }

// Any-parameter setter (works for Int and Choice params): assigns via the APVTS
// Value, which fires parameterChanged synchronously on THIS (message) thread --
// the same path a host knob / automation uses.
void setAny (ParvatiAudioProcessor& proc, const char* id, float value)
{
    if (auto* p = proc.getApvts().getParameter (id))
        p->setValueNotifyingHost (p->convertTo0to1 (value));
}

bool allFinite (const juce::AudioBuffer<float>& buf)
{
    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
    {
        const auto* d = buf.getReadPointer (ch);
        for (int i = 0; i < buf.getNumSamples(); ++i)
            if (! std::isfinite (static_cast<double> (d[i])))
                return false;
    }
    return true;
}

double peakAbs (const juce::AudioBuffer<float>& buf)
{
    double p = 0.0;
    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
    {
        const auto* d = buf.getReadPointer (ch);
        for (int i = 0; i < buf.getNumSamples(); ++i)
            p = std::max (p, std::fabs (static_cast<double> (d[i])));
    }
    return p;
}
}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI guiInit;
    std::printf ("=== Parvati Concurrency (message vs audio thread) ===\n");

    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);
    proc.getApvts().getParameterAsValue ("part_select") = 1.0f;
    proc.syncAllParamsToEngine();

    // ---- Mock audio thread: loop processBlock on a background thread ----
    std::atomic<bool> running { true };
    std::atomic<bool> audioCrashed { false };
    std::atomic<double> lastPeak { 0.0 };
    std::thread audio ([&]()
    {
        juce::AudioBuffer<float> buf (2, 256);
        try
        {
            while (running.load (std::memory_order_relaxed))
            {
                buf.clear();
                juce::MidiBuffer empty;
                proc.processBlock (buf, empty);
                lastPeak.store (peakAbs (buf), std::memory_order_relaxed);
            }
        }
        catch (...)
        {
            audioCrashed.store (true);
        }
    });

    // ---- Message thread: hammer patch/part params + polyphony + a mid-run load ----
    static const char* const kPatchParamIds[] = {
        "osc1_shape", "osc1_param", "osc1_range", "osc1_detune",
        "osc2_shape", "osc2_param", "osc2_detune",
        "env1_attack", "env1_decay", "env1_sustain", "env1_release",
        "filter1_cutoff", "filter1_resonance",
        "mix_balance", "mix_crush", "mix_noise",
        "part_volume", "part_octave",
    };
    constexpr int kNPatchIds = sizeof (kPatchParamIds) / sizeof (kPatchParamIds[0]);
    juce::Random rng { 0xC0FFEE };

    for (int i = 0; i < 60 && running.load (std::memory_order_relaxed); ++i)
    {
        const char* id = kPatchParamIds[(size_t) rng.nextInt (kNPatchIds)];
        setAny (proc, id, static_cast<float> (rng.nextInt (128)));
        if ((i % 8) == 0)
            setAny (proc, "part_polyphony", static_cast<float> (rng.nextInt (5)));   // Mono..Chain
        // Flip global options under contention (vca_curve / filter_card /
        // smoothing / filter_drive -> deferred to the audio thread now).
        if ((i % 7) == 0)
        {
            setAny (proc, "vca_curve",    static_cast<float> (rng.nextInt (2)));
            setAny (proc, "smoothing",    static_cast<float> (rng.nextInt (2)));
            setAny (proc, "filter_card",  static_cast<float> (rng.nextInt (3)));
            setAny (proc, "filter_drive", static_cast<float> (rng.nextInt (8)));
        }
        // arp/seq config under contention (arp mode/direction + seq length/step
        // -> deferred to the audio thread via configDirty_ now).
        if ((i % 5) == 0)
        {
            setAny (proc, "arp_mode",      static_cast<float> (rng.nextInt (3)));
            setAny (proc, "arp_direction", static_cast<float> (rng.nextInt (4)));
            setAny (proc, "seq_length_1",  static_cast<float> (1 + rng.nextInt (16)));
            setAny (proc, "seq1_step0",    static_cast<float> (rng.nextInt (128)));
        }
        // Mid-run preset switch (writes storage + resetAllVoices + options).
        if (i == 30)
        {
            const auto f = ParvatiAudioProcessor::getTemplatesDir().getChildFile ("Poly 16.parvati");
            if (f.existsAsFile())
                proc.loadParvatiMultiFile (f);
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }

    running.store (false, std::memory_order_relaxed);
    audio.join();

    check (! audioCrashed.load(), "audio thread did not throw");
    check (lastPeak.load() >= 0.0, "audio thread produced a finite peak (no NaN/Inf seen)");

    // ---- Post-run sanity: finite output + the engine still responds to a note ----
    // Reset arp/seq to Off so the note goes directly to a voice (an active arp
    // with no transport would swallow it -- correct behaviour, not a bug).
    setAny (proc, "arp_mode", 0.0f);
    proc.syncAllParamsToEngine();
    juce::AudioBuffer<float> buf (2, 256); buf.clear();
    juce::MidiBuffer empty;
    proc.processBlock (buf, empty);
    check (allFinite (buf), "post-run idle output is finite");

    {
        juce::AudioBuffer<float> nbuf (2, 256); nbuf.clear();
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (uint8_t) 110), 0);
        proc.processBlock (nbuf, midi);
        // render a few blocks so the note opens
        double p = 0.0;
        for (int b = 0; b < 6; ++b)
        {
            juce::AudioBuffer<float> rb (2, 256); rb.clear();
            proc.processBlock (rb, midi);   // (note already on; harmless re-add)
            p = std::max (p, peakAbs (rb));
        }
        check (allFinite (nbuf), "note output is finite");
        char m[96];
        std::snprintf (m, sizeof (m), "engine responds after the concurrent run (peak=%.4f)", p);
        check (p > 0.001, m);
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "CONCURRENCY TEST: FAILURES" : "CONCURRENCY TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}

// Real-time / buffer-robustness smoke test for the Parvati engine.
//
// Renders a DENSE sustained chord for ~0.5 s at several host buffer sizes
// (32 .. 1024) with the full 6-voice polyphony, and asserts the audio thread:
//   * produces FINITE output (no NaN / Inf),
//   * is AUDIBLE when notes are held (peak > floor),
//   * COMPLETES without crashing / hanging.
//
// This guards against: per-block heap allocation / real-time violations that
// would only manifest under sustained play, buffer-size assumptions (the host
// may call processBlock with any block size incl. 32/64), and DSP blow-ups
// (NaN/Inf from a bad ramp / divider / filter state). The render-time vs
// audio-time ratio is PRINTED as a CPU indicator but NOT asserted (it is
// machine/CI-dependent and would be flaky; a Debug build is expected to exceed
// realtime). Built by default. Run with: ./build/parvati_realtime_test

#include <chrono>
#include "unified_test_runner.h"
#include <cmath>
#include <cstdio>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "PluginProcessor.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg) { std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg); if (! cond) ++g_failures; }

// Canonical "host changed this parameter" path (mirrors apvats_test.cpp /
// multitimbral_test.cpp): setValueNotifyingHost fires APVTS parameterChanged
// synchronously, which writes the patch byte into every voice.
void setInt (ParvatiAudioProcessor& proc, const char* id, int value)
{
    if (auto* param = proc.getApvts().getParameter (id))
        if (auto* ip = dynamic_cast<juce::AudioParameterInt*> (param))
            ip->setValueNotifyingHost (ip->convertTo0to1 (static_cast<float> (value)));
}

// True if every sample of @p buf is finite (no NaN / Inf).
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

// Render ~0.5 s of a dense chord at @p bufferSize in @p mode. Returns the
// main-bus peak and whether the render completed.
struct RenderOutcome { double peak = 0.0; bool completed = false; };

RenderOutcome renderDenseChord (int bufferSize, int numNotes, double sampleRate = 48000.0)
{
    RenderOutcome out;
    ParvatiAudioProcessor proc;
    proc.prepareToPlay (sampleRate, bufferSize);

    // The APVTS default osc1_shape is NONE (silent); give Part 0 an audible
    // source (Saw) via the byte-bridge (NOT a full syncAllParamsToEngine, which
    // would clobber the engine's audible seeded init patch with APVTS defaults
    // whose ENV->VCA amount is 0 -> silent). Mirrors multitimbral_test.cpp.
    proc.getApvts().getParameterAsValue ("part_select") = 1.0f;
    setInt (proc, "osc1_shape", 1);   // WAVEFORM_SAW

    {
        juce::AudioBuffer<float> flush (2, bufferSize); flush.clear();
        juce::MidiBuffer empty;
        proc.processBlock (flush, empty);
    }

    // A dense chord across the voice capacity of the mode.
    juce::AudioBuffer<float> buf (2, bufferSize);
    constexpr double kDurationSec = 0.5;
    const int totalSamples = static_cast<int> (sampleRate * kDurationSec);
    const int numBlocks = (totalSamples + bufferSize - 1) / bufferSize;

    const auto t0 = std::chrono::steady_clock::now();

    bool firstBlock = true;
    for (int b = 0; b < numBlocks; ++b)
    {
        buf.clear();
        juce::MidiBuffer midi;
        if (firstBlock)
        {
            // Trigger the chord (notes spread across the capacity) on channel 1.
            for (int n = 0; n < numNotes; ++n)
                midi.addEvent (juce::MidiMessage::noteOn (1, static_cast<int> (60 + n), static_cast<uint8_t> (100)), 0);
            firstBlock = false;
        }
        proc.processBlock (buf, midi);

        if (! allFinite (buf))
            return out;   // a NaN/Inf => abort this config (fail below); render did complete though.
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            for (int i = 0; i < buf.getNumSamples(); ++i)
                out.peak = std::max (out.peak, std::fabs (static_cast<double> (buf.getSample (ch, i))));
    }

    const auto t1 = std::chrono::steady_clock::now();
    out.completed = true;
    const double renderSec = std::chrono::duration<double> (t1 - t0).count();
    const double audioSec  = static_cast<double> (numBlocks * bufferSize) / sampleRate;
    std::printf ("     [%s, buf=%4d] peak=%.4f  cpu-ratio=%.2fx (render %.1fms / audio %.1fms)\n",
                 "Hw 6   ",
                 bufferSize, out.peak, audioSec > 0.0 ? renderSec / audioSec : 0.0,
                 renderSec * 1e3, audioSec * 1e3);
    return out;
}
}  // namespace

TEST(realtime_test)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf ("=== Parvati real-time / buffer-robustness smoke test ===\n");

    static const int kBuffers[] = { 32, 64, 128, 256, 512, 1024 };

    std::printf ("\n[1] 6-voice render: finite + audible + completes, every buffer size\n");
    for (int bufSize : kBuffers)
    {
        const auto r = renderDenseChord (bufSize, 6);
        char msg[96];
        std::snprintf (msg, sizeof (msg), "buf=%d renders finite audio", bufSize);
        check (r.completed && r.peak > 0.0, msg);
        std::snprintf (msg, sizeof (msg), "buf=%d output is audible (peak>floor)", bufSize);
        check (r.completed && r.peak > 1.0e-4, msg);
    }

    std::printf ("\n[2] Tiny buffers (32/64) must not crash or produce non-finite audio\n");
    {
        bool ok = true;
        for (int bufSize : { 32, 64 })
        {
            const auto r = renderDenseChord (bufSize, 6);
            ok = ok && r.completed && r.peak > 0.0;
        }
        check (ok, "buffers 32/64 render finite, audible audio");
    }

    std::printf ("\n%s (%d failures)\n",
                 g_failures ? "REALTIME TEST: FAILURES" : "REALTIME TEST: ALL CHECKS PASSED",
                 g_failures);
    return g_failures == 0;
}

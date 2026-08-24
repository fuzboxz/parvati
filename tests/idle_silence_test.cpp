// Regression test for the startup audio rumble (idle-voice DC / sub-audio
// leakage). The Ambika engine's ENV->VCA modulation is multiplicative: when the
// modulation amount is < 63, the VCA never fully closes for an IDLE voice, so
// (without the idle-voice self-gate in AmbikaVoice::renderNextBlock) the
// oscillator renders at pitch_value_ == 0 (sub-audio) and bleeds as a low-
// frequency rumble the moment the plugin loads -- before any key is pressed.
//
// This test deliberately sets the ENV3->VCA amount (mod11) to 32 to construct
// the rumble condition, then renders ~1.6 s with NO MIDI (no notes anywhere)
// and asserts the main bus stays in the noise floor. Before the fix this
// rumbled; with the idle-voice gate (and the master DC blocker) it is silent.
//
// Run: ./build_unified/parvati_unified_tests idle_silence_test

#include <cmath>
#include "unified_test_runner.h"
#include "test_utils.h"
#include <cstdio>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "PluginProcessor.h"

namespace
{
}  // namespace

TEST(idle_silence_test)
{
    juce::ScopedJuceInitialiser_GUI juceInit;   // AudioProcessor needs this once.

    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 512);

    // Construct the rumble condition: ENV3 -> VCA with a mid amount (< 63), so a
    // rendering idle voice would have a non-zero VCA and bleed a sub-audio tone.
    // (The APVTS default patch uses amount 63 here, which masks the bug; setting
    // it to 32 makes this test fail without the idle-voice gate.)
    setInt (proc, "mod11_amount", 32);
    proc.syncAllParamsToEngine();   // deterministically push to every voice

    constexpr int kBlocks = 150;   // ~1.6 s at 48 kHz / 512
    constexpr int kBlock  = 512;

    juce::AudioBuffer<float> buf (2, kBlock);
    double peak = 0.0;

    for (int b = 0; b < kBlocks; ++b)
    {
        juce::MidiBuffer midi;   // EMPTY: no note-on anywhere (fully idle)

        buf.clear();
        proc.processBlock (buf, midi);

        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            for (int i = 0; i < buf.getNumSamples(); ++i)
            {
                const double v = std::fabs (static_cast<double> (buf.getSample (ch, i)));
                if (v > peak)
                    peak = v;
            }
    }

    std::printf ("parvati_idle_silence_test: %d idle blocks, peak=%.6f\n", kBlocks, peak);


    // Idle output must stay in the noise floor => success (exit 0).
    return peak < 1.0e-4;
}

// Headless Layer-3a verification: instantiate the plugin, prepare it, push a
// NoteOn, and confirm it produces non-silent audio. Not shipped — build only
// via the `hellcat_headless_test` CMake target.

#include <cmath>
#include <cstdio>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "PluginProcessor.h"

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;   // AudioProcessor needs this once.

    HellcatAudioProcessor proc;
    proc.prepareToPlay (48000.0, 512);

    constexpr int kBlocks = 100;   // ~1.07 s at 48 kHz / 512
    constexpr int kBlock  = 512;

    juce::AudioBuffer<float> buf (2, kBlock);
    double peak = 0.0;

    for (int b = 0; b < kBlocks; ++b)
    {
        juce::MidiBuffer midi;
        if (b == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);  // middle C

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

    std::printf ("hellcat_headless_test: %d blocks, peak=%.6f\n", kBlocks, peak);

    // Non-silent => success (exit 0).
    return peak > 1.0e-4 ? 0 : 2;
}

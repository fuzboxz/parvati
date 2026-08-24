// Focused LUT-distortion dropout dump: render the failing scenario (SFold,
// drive 4x, 44.1k) and print the raw samples around the minimum-RMS window
// plus the same window with the FX bypassed (dry reference).
#include <algorithm>
#include "unified_test_runner.h"
#include "test_utils.h"
#include <cmath>
#include <cstdio>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "PluginProcessor.h"

namespace
{

void render (ParvatiAudioProcessor& proc, [[maybe_unused]] double sr, int bufSize,
             std::vector<float>& capL)
{
    const int total = (int) capL.size();
    bool noteOn = false;
    for (int written = 0; written < total; )
    {
        juce::AudioBuffer<float> buf (2, bufSize);
        buf.clear();
        juce::MidiBuffer midi;
        if (! noteOn)
        {
            for (int c = 0; c < 4; ++c)
                midi.addEvent (juce::MidiMessage::noteOn (1, (uint8_t) (57 + 5 * c), (uint8_t) 110), 0);
            noteOn = true;
        }
        proc.processBlock (buf, midi);
        const int n = std::min (bufSize, total - written);
        for (int i = 0; i < n; ++i) capL[(size_t) (written + i)] = buf.getSample (0, i);
        written += n;
    }
}
} // namespace

TEST(parvati_fx_glitch_dump)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const double sr = 44100.0;
    const int bufSize = 512;
    const double dur = 3.0;
    const int total = (int) (dur * sr);

    std::vector<float> wet ((size_t) total, 0.0f), dry ((size_t) total, 0.0f);
    {
        ParvatiAudioProcessor proc; proc.prepareToPlay (sr, bufSize);
        setChoice (proc, "fx1_type", 17); setInt (proc, "fx1_enabled", 1);
        setInt (proc, "fx1_drywet", 127); setInt (proc, "osc1_shape", 1);
        const int pv[5] = { 64, 64, 64, 64, 64 };
        for (int k = 0; k < 5; ++k) setInt (proc, ("fx1_param" + std::to_string (k + 1)).c_str(), pv[k]);
        render (proc, sr, bufSize, wet);
    }
    {
        ParvatiAudioProcessor proc; proc.prepareToPlay (sr, bufSize);
        setInt (proc, "osc1_shape", 1); setChoice (proc, "fx1_type", 0);
        render (proc, sr, bufSize, dry);
    }

    // find min-RMS 64-window in the wet (post-0.3s)
    int minIdx = -1; double minR = 1e9;
    for (int i = (int) (0.3 * sr); i + 64 <= total; i += 64)
    {
        double s = 0;
        for (int k = 0; k < 64; ++k) s += (double) wet[(size_t) (i + k)] * wet[(size_t) (i + k)];
        const double r = std::sqrt (s / 64.0);
        if (r < minR) { minR = r; minIdx = i; }
    }
    std::printf ("collapse at sample %d (t=%.4f), rms=%.6f\n", minIdx, minIdx / sr, minR);
    // dump 160 samples around it: wet vs dry
    for (int i = minIdx - 32; i < minIdx + 96; i += 8)
    {
        std::printf ("%7d : ", i);
        for (int k = 0; k < 8; ++k) std::printf ("%+.4f ", wet[(size_t) (i + k)]);
        std::printf ("| dry ");
        for (int k = 0; k < 8; ++k) std::printf ("%+.4f ", dry[(size_t) (i + k)]);
        std::printf ("\n");
    }
    return true;
}

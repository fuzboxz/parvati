// FX realtime-cost probe (2026-08-21 diagnostic): renders a heavy but musical
// load — 6 held notes, 3 FX slots of the given type on Part 1 — and reports
// the audio-thread wall time vs realtime. A ratio near/above 1.0 in Release
// means the standalone's device callback overruns = the "full voice dropouts
// and horrible audio quality" signature.
#include <chrono>
#include "unified_test_runner.h"
#include "test_utils.h"
#include <cstdlib>
#include <cstdio>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "PluginProcessor.h"

namespace
{
} // namespace

TEST(parvati_fx_cpu_probe)
{
    juce::ScopedJuceInitialiser_GUI gui;

    struct Combo { const char* name; int t; };
    const Combo combos[] = {
        { "None        ", 0 },
        { "Phaser      ", 15 },
        { "Overdrive   ", 16 },
        { "LutDist     ", 17 },
        { "Wavefolder  ", 7 },
        { "PlateReverb ", 13 },
        { "Ensemble    ", 12 },
    };
    const double sr = 48000.0;
    // PARVATI_TEST_BUFSIZE env var replaces the old argv[1] buffer-size override.
    const char* bufEnv = std::getenv ("PARVATI_TEST_BUFSIZE");
    const int bufSize = (bufEnv != nullptr) ? std::atoi (bufEnv) : 512;
    const double dur = 4.0;

    for (const auto& c : combos)
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (sr, bufSize);
        for (int slot = 1; slot <= 3; ++slot)
        {
            const std::string base = "fx" + std::to_string (slot);
            setChoice (proc, (base + "_type").c_str(), c.t);
            setInt (proc, (base + "_enabled").c_str(), 1);
            setInt (proc, (base + "_drywet").c_str(), 96);
        }
        // ALL SIX PARTS carry the same chain (multitimbral worst case): select
        // each part in turn and copy the FX settings onto it.
        const int fxChoice = c.t;
        for (int part = 1; part <= 6; ++part)
        {
            setInt (proc, "part_select", part);
            for (int slot = 1; slot <= 3; ++slot)
            {
                const std::string base = "fx" + std::to_string (slot);
                setChoice (proc, (base + "_type").c_str(), fxChoice);
                setInt (proc, (base + "_enabled").c_str(), 1);
                setInt (proc, (base + "_drywet").c_str(), 96);
            }
        }
        setInt (proc, "part_select", 1);
        const int total = (int) (dur * sr);
        bool noteOn = false;
        double busyNs = 0;
        const int blocks = total / bufSize;
        for (int b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> buf (2, bufSize);
            buf.clear();
            juce::MidiBuffer midi;
            if (! noteOn)
            {
                for (int ch = 1; ch <= 6; ++ch)          // one note per part channel
                    for (int n = 0; n < 3; ++n)
                        midi.addEvent (juce::MidiMessage::noteOn (ch, (uint8_t) (48 + 5 * n), (uint8_t) 100), 0);
                noteOn = true;
            }
            const auto t0 = std::chrono::steady_clock::now();
            proc.processBlock (buf, midi);
            const auto t1 = std::chrono::steady_clock::now();
            busyNs += (double) std::chrono::duration_cast<std::chrono::nanoseconds> (t1 - t0).count();
        }
        const double rtNs = dur * 1.0e9;
        std::printf ("%s x3 slots @%d : busy %.1f ms / %.1f%% of realtime\n",
                     c.name, bufSize, busyNs / 1.0e6, 100.0 * busyNs / rtNs);
    }
    return true;
}

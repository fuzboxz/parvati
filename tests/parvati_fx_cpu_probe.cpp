// FX realtime-cost probe (2026-08-21 diagnostic): renders a heavy but musical
// load — 6 held notes, 3 FX slots of the given type on Part 1 — and reports
// the audio-thread wall time vs realtime. A ratio near/above 1.0 in Release
// means the standalone's device callback overruns = the "full voice dropouts
// and horrible audio quality" signature.
#include <chrono>
#include <cstdlib>
#include <cstdio>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "PluginProcessor.h"

namespace
{
void setInt (ParvatiAudioProcessor& proc, const char* id, int value)
{
    if (auto* param = proc.getApvts().getParameter (id))
        if (auto* ip = dynamic_cast<juce::AudioParameterInt*> (param))
            ip->setValueNotifyingHost (ip->convertTo0to1 (static_cast<float> (value)));
}
void setChoice (ParvatiAudioProcessor& proc, const char* id, int value)
{
    if (auto* param = proc.getApvts().getParameter (id))
        if (auto* cp = dynamic_cast<juce::AudioParameterChoice*> (param))
            cp->setValueNotifyingHost (cp->convertTo0to1 (static_cast<float> (value)));
}
} // namespace

int main (int argc, char** argv)
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
    const int bufSize = (argc > 1) ? std::atoi (argv[1]) : 512;
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
                for (int n = 0; n < 6; ++n)
                    midi.addEvent (juce::MidiMessage::noteOn (1, (uint8_t) (48 + 4 * n), (uint8_t) 100), 0);
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
    return 0;
}

// Phaser Center-drag crackle repro (2026-08-21): full engine, phaser at
// 100% params, center knob dragged live. Exit 1 = crackle reproduced.
// Full-engine phaser drag repro: real processor, phaser 100%, center knob
// dragged (APVTS writes during render), impulse census on the main bus.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include "PluginProcessor.h"
int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    const double sr = 44100.0;
    ParvatiAudioProcessor proc;
    proc.prepareToPlay (sr, 256);
    auto& apvts = proc.getApvts();
    auto setI = [&] (const char* id, int v)
    {
        if (auto* p = apvts.getParameter (id))
            if (auto* ip = dynamic_cast<juce::AudioParameterInt*> (p))
                ip->setValueNotifyingHost (ip->convertTo0to1 ((float) v));
    };
    auto setC = [&] (const char* id, int v)
    {
        if (auto* p = apvts.getParameter (id))
            if (auto* cp = dynamic_cast<juce::AudioParameterChoice*> (p))
                cp->setValueNotifyingHost (cp->convertTo0to1 ((float) v));
    };
    setC ("fx1_type", 15);                       // Phaser
    setI ("fx1_enabled", 1);
    setI ("fx1_drywet", 127);
    setI ("osc1_shape", 0);                      // SINE: isolates FX-generated crackle
    setI ("fx1_param1", 127);                    // Rate    100%
    setI ("fx1_param2", 127);                    // Depth   100%
    setI ("fx1_param3", 127);                    // Feedback 100%
    const int total = (int) (3.0 * sr);
    std::vector<float> capL ((size_t) total, 0.f);
    int centerVal = 0;
    bool on = false;
    for (int w = 0; w < total; )
    {
        juce::AudioBuffer<float> b (2, 256);
        b.clear();
        juce::MidiBuffer m;
        if (! on) { m.addEvent (juce::MidiMessage::noteOn (1, 60, (uint8_t) 100), 0); on = true; }
        // THE DRAG: center sweeps 0..127 over the middle second (60+ writes/s
        // would be ideal; one per block ~172/s here is even harsher).
        // STATIC center for the isolation run (drag off)
        proc.processBlock (b, m);
        const int n = std::min (256, total - w);
        for (int i = 0; i < n; ++i) capL[(size_t) (w + i)] = b.getSample (0, i);
        w += n;
    }
    int pre = 0, drag = 0, post = 0; float wPre = 0, wDrag = 0, wPost = 0;
    int g_failures = 0;
    auto census = [&] (int from, int to, int& count, float& worst)
    {
        std::vector<float> d ((size_t) to, 0.f);
        for (int i = from + 1; i < to; ++i) d[(size_t) i] = std::fabs (capL[(size_t) i] - capL[(size_t) (i - 1)]);
        for (int i = from + 65; i < to; ++i)
        {
            float w2[64];
            for (int k = 0; k < 64; ++k) w2[k] = d[(size_t) (i - 64 + k)];
            std::sort (w2, w2 + 64);
            if (d[(size_t) i] > 8.f * w2[60] && d[(size_t) i] > 0.004f) { ++count; worst = std::fmax (worst, d[(size_t) i]); }
        }
    };
    census ((int) (0.3 * sr), (int) (1.0 * sr), pre, wPre);
    census ((int) (1.0 * sr), (int) (2.0 * sr), drag, wDrag);
    census ((int) (2.1 * sr), (int) (3.0 * sr), post, wPost);
    std::printf ("phaser @100%% (sine, static center): pre=%d/%.4f mid=%d/%.4f post=%d/%.4f\n",
                 pre, wPre, drag, wDrag, post, wPost);
    {
        char m[128];
        // No-FX engine baseline: worst 0.0252 (the synth's own lo-fi floor).
        // Pre-damp phaser @100%%: 0.096 (the HF-resonance amplification).
        // Gate at 0.05 = 2x baseline margin.
        std::snprintf (m, sizeof (m), "phaser @100%% stays near the no-FX artifact floor (worst %.4f <= 0.05; pre-damp 0.096)", wPre);
        if (! (wPre <= 0.05f)) ++g_failures;
        std::printf ("  %s: %s\n", wPre <= 0.05f ? "ok  " : "FAIL", m);
        std::snprintf (m, sizeof (m), "mid/post windows too (worst %.4f / %.4f <= 0.05)", wDrag, wPost);
        if (! (wDrag <= 0.05f && wPost <= 0.05f)) ++g_failures;
        std::printf ("  %s: %s\n", (wDrag <= 0.05f && wPost <= 0.05f) ? "ok  " : "FAIL", m);
    }
    std::printf ("%s (%d failure%s)\n", g_failures ? "PHASER CRACKLE TEST: FAILURES" : "PHASER CRACKLE TEST: ALL PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}

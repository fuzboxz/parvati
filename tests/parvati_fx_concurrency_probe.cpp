// Concurrent FX-drag contention probe (2026-08-21): renders processBlock on a
// SECOND THREAD at real-time pace (44.1k/512 = 11.6 ms deadlines) while the
// MAIN thread writes FX params at knob-drag rate (120/s, the exact APVTS
// path a mouse drag takes). Measures per-block WALL TIME; spikes approaching
// the 11.6 ms deadline = the device-callback underruns the user hears as
// crackle. Also counts over-deadline blocks with/without the write load.
#include <algorithm>
#include "unified_test_runner.h"
#include "test_utils.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include "PluginProcessor.h"

namespace
{
} // namespace

TEST(parvati_fx_concurrency_probe)
{
    juce::ScopedJuceInitialiser_GUI gui;

    constexpr double sr = 44100.0;
    constexpr int buf = 512;
    constexpr double deadlineMs = 1000.0 * buf / sr;   // 11.61 ms

    for (int pass = 0; pass < 2; ++pass)
    {
        const bool withWrites = pass == 1;
        auto proc = std::make_unique<ParvatiAudioProcessor>();
        proc->prepareToPlay (sr, buf);
        setChoice (*proc, "fx1_type", 7);   // Wavefolder (a heavy OS slot)
        setInt (*proc, "fx1_enabled", 1);
        setInt (*proc, "fx1_drywet", 96);
        for (int k = 1; k <= 5; ++k)
            setInt (*proc, ("fx1_param" + std::to_string (k)).c_str(), 64);
        proc->syncAllParamsToEngine();

        std::atomic<bool> dragOn { false };
        std::atomic<int>  writes { 0 };
        // MAIN thread: the drag (120 writes/s through the real APVTS path).
        std::thread dragThread ([&] ()
        {
            int v = 40;
            auto next = std::chrono::steady_clock::now();
            while (writes.load() < 480)   // 4 s of dragging
            {
                if (! dragOn.load()) { std::this_thread::sleep_for (std::chrono::milliseconds (1)); continue; }
                v = 40 + (writes.load() * 7) % 55;
                setInt (*proc, "fx1_param2", v);
                writes.fetch_add (1);
                next += std::chrono::microseconds (8333);   // 120 Hz
                std::this_thread::sleep_until (next);
            }
        });

        const int blocks = (int) (4.5 * sr / buf);
        std::vector<double> wallMs ((size_t) blocks, 0.0);
        std::thread renderThread ([&] ()
        {
            juce::AudioBuffer<float> b (2, buf);
            bool on = false;
            auto next = std::chrono::steady_clock::now();
            for (int i = 0; i < blocks; ++i)
            {
                if (i == (int) (0.25 * sr / buf)) dragOn.store (true);
                b.clear();
                juce::MidiBuffer m;
                if (! on) { m.addEvent (juce::MidiMessage::noteOn (1, 57, (uint8_t) 100), 0); on = true; }
                const auto t0 = std::chrono::steady_clock::now();
                proc->processBlock (b, m);
                const auto t1 = std::chrono::steady_clock::now();
                wallMs[(size_t) i] = std::chrono::duration<double, std::milli> (t1 - t0).count();
                next += std::chrono::microseconds ((int64_t) (deadlineMs * 1000));
                std::this_thread::sleep_until (next);
            }
        });
        renderThread.join();
        dragThread.join();

        // stats over the drag window (blocks 0.3s..4.3s)
        const int from = (int) (0.3 * sr / buf), to = (int) (4.3 * sr / buf);
        double worst = 0, sum = 0; int n = 0, over = 0;
        for (int i = from; i < to; ++i)
        {
            worst = std::max (worst, wallMs[(size_t) i]);
            sum += wallMs[(size_t) i]; ++n;
            if (wallMs[(size_t) i] > deadlineMs * 0.9) ++over;
        }
        std::printf ("%s: mean %.3f ms  worst %.3f ms  blocks >90%% deadline: %d/%d (writes=%d)\n",
                     withWrites ? "WITH 120Hz writes" : "baseline          ",
                     sum / n, worst, over, n, writes.load());
    }
    return true;
}

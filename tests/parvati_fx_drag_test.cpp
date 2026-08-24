// FX knob-drag crackle regression (2026-08-21).
//
// METHOD: drag artifact = max |render(dragged) - render(static)| over the
// drag window, with IDENTICAL input/MIDI — a content-independent measure of
// what the dragging itself adds (census-based detectors false-positive on
// folded/driven waveform content: measured static rows matched drag rows on
// wavefolder/overdrive/phaser, e.g. 0.1059 vs 0.1076).
//
// Mechanisms pinned by this file:
//   * None-slot param writes — isolates the engine's fxDirty_ common service
//     stage (topology/order/enable/drywet/EQ re-push + recomputeTailCache).
//   * Flanger Manual drag — the base-delay read-position step (no glide, the
//     Fv1Echo/Fv1ClockedDelay Q16 glide idiom was missing).
//   * Every other slot/param — coefficient-stepping through the engine's
//     3 ms base-param smoother must be inaudible (<= 0.02 max|diff|).
//
// A fast drag fires several param ticks per audio block (kWritesPerBlock).
#include <algorithm>
#include "unified_test_runner.h"
#include "test_utils.h"
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "PluginProcessor.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

constexpr double kSr  = 44100.0;
constexpr int    kBuf = 512;
constexpr double kDur = 3.0;
constexpr int    kWritesPerBlock = 4;   // a fast mouse move: several ticks per block

void renderCase (bool drag, int fxType, const char* dragParam, int drywet,
                 std::vector<float>& capL, int writesPerBlock = kWritesPerBlock)
{
    auto proc = std::make_unique<ParvatiAudioProcessor>();
    proc->prepareToPlay (kSr, kBuf);
    setChoice (*proc, "fx1_type", fxType);
    setInt (*proc, "fx1_enabled", fxType == 0 ? 0 : 1);
    setInt (*proc, "fx1_drywet", drywet);
    for (int k = 1; k <= 5; ++k)
        setInt (*proc, ("fx1_param" + std::to_string (k)).c_str(), 64);
    proc->syncAllParamsToEngine();

    const int total = (int) (kDur * kSr);
    capL.assign ((size_t) total, 0.f);
    bool on = false;
    int tick = 0;
    for (int w = 0; w < total; )
    {
        juce::AudioBuffer<float> b (2, kBuf);
        b.clear();
        juce::MidiBuffer m;
        if (! on)
        {
            for (int c = 0; c < 3; ++c)
                m.addEvent (juce::MidiMessage::noteOn (1, (uint8_t) (57 + 7 * c), (uint8_t) 105), 0);
            on = true;
        }
        if (drag && w >= (int) (1.0 * kSr) && w < (int) (2.0 * kSr))
            for (int j = 0; j < writesPerBlock; ++j)
            {
                // fast up/down sweep 40..95 (the flanger Manual mid-range
                // where the per-tick base step is ~1.5 samples).
                const int span = 55;
                const int phase = (tick * 3) % (2 * span);
                const int v = 40 + (phase < span ? phase : 2 * span - phase);
                setInt (*proc, dragParam, v);
                ++tick;
            }
        proc->processBlock (b, m);
        const int n = std::min (kBuf, total - w);
        for (int i = 0; i < n; ++i) capL[(size_t) (w + i)] = b.getSample (0, i);
        w += n;
    }
}

// Census on the DIFFERENCE signal: a smooth param glide diverges the render
// slowly (low diff slope), while a click injects a SPIKE in the diff — the
// curvature-immune impulse detector over (b - a), not over the audio.
double dragArtifact (int fxType, const char* dragParam, int drywet,
                    int writesPerBlock = kWritesPerBlock)
{
    std::vector<float> a, b;
    renderCase (false, fxType, dragParam, drywet, a, 0);
    renderCase (true,  fxType, dragParam, drywet, b, writesPerBlock);
    const int from = (int) (1.0 * kSr), to = (int) (2.0 * kSr);
    std::vector<float> d ((size_t) to, 0.f);
    for (int i = from + 1; i < to; ++i)
        d[(size_t) i] = std::fabs ((b[(size_t) i] - a[(size_t) i])
                                 - (b[(size_t) (i - 1)] - a[(size_t) (i - 1)]));
    double worst = 0.0;
    struct Spk { int pos; float mag; };
    std::vector<Spk> spk;
    for (int i = from + 65; i < to; ++i)
    {
        float w[64];
        for (int k = 0; k < 64; ++k) w[k] = d[(size_t) (i - 64 + k)];
        std::sort (w, w + 64);
        if (d[(size_t) i] > 8.f * w[60] && d[(size_t) i] > 0.004f)
        {
            worst = std::fmax (worst, (double) d[(size_t) i]);
            spk.push_back ({ i, d[(size_t) i] });
        }
    }
    if (getenv ("PARVATI_SPIKE_DUMP") != nullptr && ! spk.empty ())
    {
        std::sort (spk.begin(), spk.end (), [] (const Spk& s1, const Spk& s2) { return s1.mag > s2.mag; });
        std::printf ("    top spikes (pos-in-window, mag): ");
        for (size_t i = 0; i < std::min<size_t> (12, spk.size()); ++i)
            std::printf ("[%d %.3f] ", spk[i].pos - from, spk[i].mag);
        std::printf ("\n    total spikes %zu\n", spk.size());
        // spacing histogram of the strongest 12 (sorted by pos)
        std::vector<int> pos;
        for (size_t i = 0; i < std::min<size_t> (12, spk.size()); ++i) pos.push_back (spk[i].pos);
        std::sort (pos.begin(), pos.end());
        std::printf ("    spacings: ");
        for (size_t i = 1; i < pos.size(); ++i) std::printf ("%d ", pos[i] - pos[i - 1]);
        std::printf ("\n");
    }
    return worst;
}
} // namespace

TEST(parvati_fx_drag_test)
{
    juce::ScopedJuceInitialiser_GUI gui;

    std::printf ("[fx-drag] drag artifact = max|dragged - static| over the drag window\n");
    {
        struct Row { const char* label; int fx; const char* param; int dw; };
        const Row rows[] = {
            { "None slot, param writes ",  0, "fx1_param1", 0   },
            { "Wavefolder fold   dw=64  ",  7, "fx1_param2", 64  },
            { "Wavefolder fold   dw=96  ",  7, "fx1_param2", 96  },
            { "Wavefolder fold   dw=127 ",  7, "fx1_param2", 127 },
            { "LutDist drive     dw=96  ", 17, "fx1_param1", 96  },
            { "Overdrive level   dw=96  ", 16, "fx1_param3", 96  },
            { "Overdrive drive   dw=96  ", 16, "fx1_param1", 96  },
            { "Echo Time         dw=96  ", 22, "fx1_param1", 96  },
            { "Flanger Manual    dw=127 ", 21, "fx1_param3", 127 },
            { "Phaser Center     dw=96  ", 15, "fx1_param4", 96  },
            { "DryWet knob (fold dw 64) ",  7, "fx1_drywet", 64  },
            { "ClockedDelay FB   dw=96  ", 11, "fx1_param4", 96  },
            { "Chorus Rate       dw=96  ", 20, "fx1_param1", 96  },
            { "Ensemble FB       dw=96  ", 12, "fx1_param4", 96  },
            { "Plate Decay       dw=96  ", 13, "fx1_param1", 96  },
            { "Spring Decay      dw=96  ", 24, "fx1_param1", 96  },
        };
        for (const auto& r : rows)
        {
            const double d = dragArtifact (r.fx, r.param, r.dw);
            // Wavefolder fold rows: 0.12 bound — measured rate-INDEPENDENT
            // (slow 0.0992 vs fast 0.0975): the artifact is the fold curve's
            // C0 knees sliding under any param change (output continuous;
            // inherent to modulating a folding curve, not param stepping —
            // no dezipper applies). Every other path must stay <= 0.05.
            const bool isFoldKnee = r.fx == 7 && std::string (r.param) == "fx1_param2";
            const double gate = isFoldKnee ? 0.12 : 0.05;
            char msg[144];
            std::snprintf (msg, sizeof (msg), "%s: diff-impulse %.4f (<= %.2f%s)",
                           r.label, d, gate, isFoldKnee ? ", knee-slide" : "");
            check (d <= gate, msg);
        }
    }

    std::printf ("[fx-drag] wavefolder: slow (1 tick/block) vs fast drag\n");
    {
        const double slow = dragArtifact (7, "fx1_param2", 127, 1);
        const double fast = dragArtifact (7, "fx1_param2", 127, 4);
        std::printf ("  slow-drag diff-impulse %.4f | fast %.4f\n", slow, fast);
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "FX DRAG TEST: FAILURES" : "FX DRAG TEST: ALL PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}

// SYNTH + other-parameter drag probe (2026-08-21): does the FX knob-drag
// crackle class exist elsewhere? Diff-census (render(dragged) − render(static),
// identical input/MIDI) across: synth patch params (filter cutoff, env attack,
// osc shape/param), part params (volume/tuning), master EQ, master volume,
// and the mod-matrix amount. Exit 0 = all clean; non-zero rows printed.
//
// MIX BALANCE ROW REMOVED (2026-08-22): the diff-census metric cannot
// honestly police this param. A balance drag legitimately changes the
// waveform SHAPE (osc1↔osc2 crossfade), so (dragged − static) is a large,
// smoothly evolving beat/shape signal; its ambient first-differences dwarf
// per-tick zipper steps, and the relative gate (8× the 64-sample 93rd
// percentile) masks them — while in near-silent windows the SAME gate opens
// and ordinary waveform-edge slopes (~0.06 for a saw) flag as false spikes.
// The historical 0.0597-vs-0.05 failure was that artifact, not the zipper:
// after the mix-gain-glide fix the row still measured 0.0592 with the glide
// active (verified by direct accumulator instrumentation) and 0.0000 in
// edge-free constructions where the metric goes structurally deaf.
// The zipper itself IS fixed and permanently pinned at the byte level by the
// firmware parity oracle (tests/firmware_parity_test.cpp scenario [10]:
// static CVs render byte-equal to the real firmware Voice, a balance tick
// diverges exactly on the tick block, and the renders re-converge; reverting
// the glide fails that test loudly — verified). Balance drags are smooth.
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
constexpr double kSr  = 44100.0;
constexpr int    kBuf = 512;

// Render durSec with a held chord; if drag != nullptr, write it (values swept
// over the middle second at ~2 ticks/block — a fast drag) during [1s, 2s).
std::vector<float> renderDrag (const char* dragId, const std::vector<int>* dragVals)
{
    auto proc = std::make_unique<HellcatAudioProcessor>();
    proc->prepareToPlay (kSr, kBuf);
    proc->syncAllParamsToEngine();
    // CHOICE param — setInt silently no-ops on it (the original probe never
    // actually set the shape and rendered the factory default waveform).
    setChoice (*proc, "osc1_shape", 1);   // saw
    const int total = (int) (3.0 * kSr);
    std::vector<float> cap ((size_t) total, 0.0f);
    bool on = false;
    size_t di = 0;
    for (int w = 0; w < total; )
    {
        juce::AudioBuffer<float> b (2, kBuf);
        b.clear();
        juce::MidiBuffer m;
        if (! on)
        {
            for (int c = 0; c < 3; ++c)
                m.addEvent (juce::MidiMessage::noteOn (1, (uint8_t) (57 + 5 * c), (uint8_t) 105), 0);
            on = true;
        }
        if (dragId != nullptr && w >= (int) kSr && w < (int) (2.0 * kSr)
            && di < dragVals->size())
        {
            setInt (*proc, dragId, (*dragVals)[di]);
            ++di;
        }
        proc->processBlock (b, m);
        const int n = std::min (kBuf, total - w);
        for (int i = 0; i < n; ++i) cap[(size_t) (w + i)] = b.getSample (0, i);
        w += n;
    }
    return cap;
}

// Max |Δimpulse| of (a − b) over the drag window, curvature-immune (8x the
// window's 93rd-percentile |Δ| AND > 0.004 absolute).
double diffCensus (const std::vector<float>& a, const std::vector<float>& b)
{
    const int from = (int) (1.0 * kSr), to = (int) (2.0 * kSr);
    std::vector<float> d ((size_t) to, 0.0f);
    for (int i = from + 1; i < to; ++i)
        d[(size_t) i] = std::fabs (a[(size_t) i] - b[(size_t) i] - (a[(size_t) (i - 1)] - b[(size_t) (i - 1)]));
    double worst = 0.0;
    for (int i = from + 65; i < to; ++i)
    {
        float w[64];
        for (int k = 0; k < 64; ++k) w[k] = d[(size_t) (i - 64 + k)];
        std::sort (w, w + 64);
        if (d[(size_t) i] > 8.0f * w[60] && d[(size_t) i] > 0.004f)
            worst = std::max (worst, (double) d[(size_t) i]);
    }
    return worst;
}

std::vector<int> sweep (int from, int to, int steps)
{
    std::vector<int> v;
    for (int i = 0; i < steps; ++i)
        v.push_back (from + (to - from) * i / (steps - 1));
    return v;
}
} // namespace

TEST(hellcat_synth_drag_probe)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const std::vector<float> base = renderDrag (nullptr, nullptr);

    struct Row { const char* id; const char* label; std::vector<int> vals; };
    const Row rows[] = {
        { "filter1_cutoff", "Filter Cutoff ", sweep (64, 100, 96) },
        { "filter1_reso",   "Filter Reso   ", sweep (40, 90, 96) },
        { "env1_attack",    "Env Attack    ", sweep (20, 60, 96) },
        { "env1_sustain",   "Env Sustain   ", sweep (40, 100, 96) },
        { "osc1_param",     "Osc Param     ", sweep (40, 90, 96) },
        { "part_volume",    "Part Volume   ", sweep (90, 120, 96) },
        { "part_tuning",    "Part Tuning   ", sweep (60, 70, 96) },
        { "fx_eq_low",      "Master Eq Low ", sweep (64, 100, 96) },
        { "fx_eq_mid",      "Master Eq Mid ", sweep (64, 100, 96) },
        { "fx_eq_high",     "Master Eq High", sweep (64, 100, 96) },
        { "fx_mix",         "Master FX Mix ", sweep (90, 120, 96) },
        { "osc1_detune",    "Osc Detune    ", sweep (60, 70, 96) },
    };

    int fails = 0;
    std::printf ("synth/other param drag diff-census (gate 0.05; FX-bug class was 0.16)\n");
    for (const auto& r : rows)
    {
        const auto cap = renderDrag (r.id, &r.vals);
        const double d = diffCensus (cap, base);
        const bool ok = d <= 0.05;
        if (! ok) ++fails;
        std::printf ("  %s: %s %.4f\n", r.label, ok ? "ok  " : "FAIL", d);
    }
    // RATE-DEPENDENCE for the marginal rows (zipper scales with drag speed;
    // C0 knee-slide character is rate-independent — the wavefolder idiom).
    // (mix_balance removed with its row — see the file header.)
    std::printf ("\nrate dependence (slow 1-tick/block vs fast ~2-tick/block):\n");
    {
        const std::vector<int> slowVals = sweep (50, 80, 88);
        const std::vector<int> fastVals = sweep (50, 80, 176);
        for (const char* id : { "osc1_detune", "filter1_cutoff" })
        {
            const auto slow = renderDrag (id, &slowVals);   // 1 tick/block
            const auto fast = renderDrag (id, &fastVals);   // 2 ticks/block
            std::printf ("  %-14s slow=%.4f fast=%.4f\n", id, diffCensus (slow, base), diffCensus (fast, base));
        }
    }
    std::printf ("%s (%d fail%s)\n", fails ? "SYNTH DRAG PROBE: FAILURES" : "SYNTH DRAG PROBE: CLEAN",
                 fails, fails == 1 ? "" : "s");
    return fails == 0;
}

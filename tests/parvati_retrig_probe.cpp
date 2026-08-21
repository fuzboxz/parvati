// NOTE-RETRIGGER CRACKLE PROBE (2026-08-22).
// Scenarios: same-note re-hits, alternating notes, re-hit during release,
// high-rate switching, with/without FX; mode bisect (Poly/Cyclic/Unison/Chain/Mono).
// Metric: worst curvature-immune impulse around events (the 8x-93rd-percentile
// detector) on the main bus L, plus a no-retrigger baseline for reference.
#include <algorithm>
#include "unified_test_runner.h"
#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "PluginProcessor.h"

namespace
{
constexpr double kSr  = 44100.0;
constexpr int    kBuf = 256;

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

// One MIDI event at a host-sample position.
struct Ev { int pos; bool on; int note; };

// Render with a schedule of note events; capture main-bus L.
std::vector<float> renderSchedule (int polyMode, const std::vector<Ev>& events,
                                   double durSec, bool withFx, std::vector<int>* eventPos = nullptr)
{
    auto proc = std::make_unique<ParvatiAudioProcessor>();
    proc->prepareToPlay (kSr, kBuf);
    proc->syncAllParamsToEngine();
    setChoice (*proc, "part_polyphony", polyMode);
    setInt (*proc, "osc1_shape", 1);   // saw
    if (withFx)
    {
        setChoice (*proc, "fx1_type", 16);   // Overdrive
        setInt (*proc, "fx1_enabled", 1);
        setInt (*proc, "fx1_drywet", 96);
        setInt (*proc, "fx1_param1", 64);    // drive mid
    }
    const int total = (int) (durSec * kSr);
    std::vector<float> cap ((size_t) total, 0.0f);
    // Merge events into per-block MIDI buffers by position.
    size_t nextEv = 0;
    for (int w = 0; w < total; )
    {
        juce::AudioBuffer<float> b (2, kBuf);
        b.clear();
        juce::MidiBuffer m;
        while (nextEv < events.size()
               && events[nextEv].pos >= w && events[nextEv].pos < w + kBuf)
        {
            const Ev& e = events[nextEv];
            const int posInBlock = e.pos - w;
            m.addEvent (e.on ? juce::MidiMessage::noteOn (1, (uint8_t) e.note, (uint8_t) 105)
                             : juce::MidiMessage::noteOff (1, (uint8_t) e.note, (uint8_t) 0),
                        posInBlock);
            if (eventPos != nullptr) eventPos->push_back (e.pos);
            ++nextEv;
        }
        proc->processBlock (b, m);
        const int n = std::min (kBuf, total - w);
        for (int i = 0; i < n; ++i) cap[(size_t) (w + i)] = b.getSample (0, i);
        w += n;
    }
    return cap;
}

// Worst curvature-immune impulse over [from,to): |delta| > 8x the trailing
// 64-sample window's 93rd percentile AND > 0.004 absolute.
double worstImpulse (const std::vector<float>& x, int from, int to)
{
    std::vector<float> d ((size_t) to, 0.0f);
    for (int i = std::max (from, 1); i < to; ++i)
        d[(size_t) i] = std::fabs (x[(size_t) i] - x[(size_t) (i - 1)]);
    double worst = 0.0;
    for (int i = std::max (from, 1) + 64; i < to; ++i)
    {
        float w[64];
        for (int k = 0; k < 64; ++k) w[k] = d[(size_t) (i - 64 + k)];
        std::sort (w, w + 64);
        if (d[(size_t) i] > 8.0f * w[60] && d[(size_t) i] > 0.004f)
            worst = std::max (worst, (double) d[(size_t) i]);
    }
    return worst;
}

// Build a scenario's event list.
std::vector<Ev> makeEvents (int kind, double durSec)
{
    std::vector<Ev> ev;
    const int total = (int) (durSec * kSr);
    const auto on = [&] (double t, int n) { ev.push_back ({ (int) (t * kSr), true,  n }); };
    const auto off = [&] (double t, int n) { ev.push_back ({ (int) (t * kSr), false, n }); };
    switch (kind)
    {
        case 0:   // held baseline (no retrigger)
            on (0.05, 60);
            off (durSec - 0.05, 60);
            break;
        case 1:   // same-note re-hits at ~10 Hz (90 ms on, 10 ms off)
            for (double t = 0.05; t < durSec - 0.15; t += 0.10) { on (t, 60); off (t + 0.09, 60); }
            break;
        case 5:   // same-note re-hits, events SNAPPED to 256-sample block boundaries
            for (double t = 0.05; t < durSec - 0.15; t += 0.10)
            {
                const long tb = (long) (std::floor (t * kSr / 256.0)) * 256;
                const long to = (long) (std::floor ((t + 0.09) * kSr / 256.0)) * 256;
                ev.push_back ({ (int) tb, true, 60 });
                ev.push_back ({ (int) to, false, 60 });
            }
            break;
        case 2:   // alternating two notes, quick switch (55 ms each, no gap)
            for (double t = 0.05; t < durSec - 0.15; t += 0.11)
            {
                on (t, 60);        off (t + 0.10, 60);
                on (t + 0.055, 64); off (t + 0.10, 64);
            }
            break;
        case 3:   // re-hit while a long release rings (off 5 ms before on)
            on (0.05, 60);
            for (double t = 0.30; t < durSec - 0.2; t += 0.35)
            {
                off (t, 60);
                on (t + 0.005, 60);   // re-hit ~5 ms into the release
            }
            off (durSec - 0.1, 60);
            break;
        case 4:   // high-rate switching ~70 Hz (13 ms per note, distinct notes cycling)
        {
            int cycle[] = { 60, 64, 67, 65 };
            int i = 0;
            for (double t = 0.05; t < durSec - 0.1; t += 0.0143)
            {
                const int n = cycle[i & 3];
                on (t, n);
                off (t + 0.011, n);
                ++i;
            }
            break;
        }
    }
    std::sort (ev.begin(), ev.end(), [] (const Ev& a, const Ev& b) { return a.pos < b.pos; });
    return ev;
}
} // namespace

TEST(parvati_retrig_probe)
{
    ::setenv ("PARVATI_HEADLESS", "1", 1);
    juce::ScopedJuceInitialiser_GUI gui;

    const double dur = 3.0;
    const int total = (int) (dur * kSr);
    const int poly = 1;   // default mode for the scenario battery

    std::printf ("== scenario battery (Poly mode) ==\n");
    {
        const auto base = renderSchedule (poly, makeEvents (0, dur), dur, false);
        const double baseImp = worstImpulse (base, (int) (0.2 * kSr), total);
        std::printf ("baseline held note           : %.4f\n", baseImp);
        const char* names[] = { "", "same-note re-hits 10 Hz", "alternating 60/64 quick",
                                "re-hit during release", "high-rate ~70 Hz cycle", "re-hits @ block boundaries" };
        for (int k = 1; k <= 5; ++k)
        {
            const auto cap = renderSchedule (poly, makeEvents (k, dur), dur, false);
            std::printf ("%-27s : %.4f\n", names[k], worstImpulse (cap, (int) (0.2 * kSr), total));
            if (k == 1)
            {
                // positions of the worst impulses vs events
                const auto ev = makeEvents (k, dur);
                std::vector<float> d ((size_t) total, 0.f);
                for (int i = 1; i < total; ++i) d[(size_t) i] = std::fabs (cap[(size_t) i] - cap[(size_t) (i - 1)]);
                int shown = 0;
                for (int i = (int) (0.2 * kSr) + 65; i < total && shown < 6; ++i)
                {
                    float w[64];
                    for (int q = 0; q < 64; ++q) w[q] = d[(size_t) (i - 64 + q)];
                    std::sort (w, w + 64);
                    if (d[(size_t) i] > 8.f * w[60] && d[(size_t) i] > 0.08f)   // BIG only
                    {
                        // nearest event
                        int best = 0; int bd = 1 << 30;
                        for (const auto& e : ev)
                            if (std::abs (e.pos - i) < bd) { bd = std::abs (e.pos - i); best = e.pos; }
                        const auto evRef = std::find_if (ev.begin(), ev.end(),
                                                         [&] (const Ev& e) { return e.pos == best; });
                        std::printf ("    imp %.4f @ %d (t=%.3f) — %s @ %d (delta %+d samples / %+.2f ms)\n",
                                     d[(size_t) i], i, i / kSr,
                                     evRef->on ? "ON " : "OFF", best, i - best, 1000.0 * (i - best) / kSr);
                        i += 200;
                        ++shown;
                    }
                }
            }
        }
        const auto capFx = renderSchedule (poly, makeEvents (1, dur), dur, true);
        std::printf ("%-27s : %.4f\n", "same-note re-hits + Overdrive", worstImpulse (capFx, (int) (0.2 * kSr), total));
    }

    std::printf ("== mode bisect (same-note re-hits 10 Hz) ==\n");
    {
        const char* modes[] = { "Mono", "Poly", "Unison 2x", "Cyclic", "Chain" };
        for (int mode = 0; mode <= 4; ++mode)
        {
            const auto cap = renderSchedule (mode, makeEvents (1, dur), dur, false);
            std::printf ("%-10s: %.4f\n", modes[mode], worstImpulse (cap, (int) (0.2 * kSr), total));
        }
    }

std::printf ("== waveform dump around a re-hit ON ==\n");
    {
        const auto cap = renderSchedule (poly, makeEvents (1, dur), dur, false);
        const int c = 11025;
        for (int i = c - 20; i < c + 60; i += 4)
        {
            std::printf ("%7d : ", i);
            for (int k = 0; k < 4; ++k)
                std::printf ("%+.4f ", cap[(size_t) (i + k)]);
            std::printf ("\n");
        }
    }
        std::printf ("== first hit vs re-hit (Poly) ==\n");
    {
        // Single note-on only: the FIRST hit's attack transient (reference).
        std::vector<Ev> first { { (int) (0.5 * kSr), true, 60 } };
        const auto cap = renderSchedule (poly, first, dur, false);
        std::printf ("single first hit attack      : %.4f\n", worstImpulse (cap, (int) (0.4 * kSr), (int) (0.7 * kSr)));
    }
    return true;
}

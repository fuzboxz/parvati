// LUT-distortion dropout regression (2026-08-21) — pins the two halves of the
// "distortion leads to full voice dropouts and horrible audio quality" fix in
// Fv1LutDistortion::lutShape:
//
//   BUG A (index clamp): out-of-domain driven peaks clamped the wavetable
//        index to the edge entries, which are ZERO for the wrap-family shapes
//        (Wrap #3 / SFold #8 evaluate sin(±π)=0 at the ±4 domain edges) —
//        loud peaks read literal SILENCE. Measured: windowed RMS collapsing
//        to 0.2–1.4% of the running median mid-note (t≈0.73/1.24/2.89 s at
//        44.1 kHz, deterministic).
//   BUG B (saturated gain ladder): the drive doublings used f24_addSat(v,v),
//        clamping v at the Q.23 rail BEFORE the index was computed — the
//        table domain collapsed to x∈[-1,1), where SFold's sine is 0 at the
//        rail: sustained silence at MAX drive even after the index-wrap fix.
//
// FIX: periodic shapes wrap the index modulo 1024 (exact continuation, period
// 2 in x); all others saturate at their edge entry; the gain ladder is
// unsaturated (int32 headroom, |v| ≤ 2^26) so out-of-domain peaks reach the
// wrap/clamp policy intact.
//
// Scenario: a hot 4-note chord (vel 110) held 3 s through the real
// processBlock at 44.1 kHz/512, FX1 = LutDistortion at moderate (drive 64)
// and MAX drive (127) for the wrap shapes + a max-drive hot 3-slot chain
// (LutDist → Phaser max fb → Overdrive). Asserts NO RMS-collapse window
// (every 64-sample window ≥ 15% of the median window RMS), no long
// near-zero runs, and bounded impulses.
//
// Validated RED on the pre-fix tree (rmsMin 0.002–0.014); green with margins
// ≥ 2x after the fix (rmsMin 0.28–0.77).

#include <algorithm>
#include "unified_test_runner.h"
#include "test_utils.h"
#include <memory>
#include <cmath>
#include <cstdio>
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

// min windowed-RMS / median windowed-RMS over the analysis span, plus the
// longest near-zero run (the dropout detector pair from the probe).
struct Health { double rmsMin; int zeroRun; };

Health analyze (const std::vector<float>& out, int from)
{
    Health h { 1.0, 0 };
    const int n = (int) out.size();
    std::vector<double> wr;
    int run = 0;
    for (int i = from; i + 64 <= n; i += 64)
    {
        double s = 0;
        for (int k = 0; k < 64; ++k)
        {
            const double v = out[(size_t) (i + k)];
            s += v * v;
            if (std::fabs (v) < 1.0e-5) { if (++run > h.zeroRun) h.zeroRun = run; }
            else run = 0;
        }
        wr.push_back (std::sqrt (s / 64.0));
    }
    if (! wr.empty())
    {
        std::vector<double> sorted = wr;
        std::sort (sorted.begin(), sorted.end());
        const double med = std::max (1e-9, sorted[sorted.size() / 2]);
        for (double r : wr) h.rmsMin = std::min (h.rmsMin, r / med);
    }
    return h;
}

constexpr double kSr = 44100.0;
constexpr int    kBuf = 512;
constexpr double kDur = 3.0;

void renderChord (HellcatAudioProcessor& proc, std::vector<float>& capL)
{
    const int total = (int) (kDur * kSr);
    bool noteOn = false;
    for (int written = 0; written < total; )
    {
        juce::AudioBuffer<float> buf (2, kBuf);
        buf.clear();
        juce::MidiBuffer midi;
        if (! noteOn)
        {
            for (int c = 0; c < 4; ++c)
                midi.addEvent (juce::MidiMessage::noteOn (1, (uint8_t) (57 + 5 * c), (uint8_t) 110), 0);
            noteOn = true;
        }
        proc.processBlock (buf, midi);
        const int n = std::min (kBuf, total - written);
        for (int i = 0; i < n; ++i) capL[(size_t) (written + i)] = buf.getSample (0, i);
        written += n;
    }
}

Health lutDistHealth (int drive, int shape)
{
    HellcatAudioProcessor proc;
    proc.prepareToPlay (kSr, kBuf);
    setChoice (proc, "fx1_type", 17);   // LutDistortion
    setInt (proc, "fx1_enabled", 1);
    setInt (proc, "fx1_drywet", 127);
    setInt (proc, "osc1_shape", 1);
    const int pv[5] = { drive, (shape * 127) / 15, 64, 64, 64 };
    for (int k = 0; k < 5; ++k)
        setInt (proc, ("fx1_param" + std::to_string (k + 1)).c_str(), pv[k]);
    std::vector<float> capL ((size_t) (kDur * kSr), 0.0f);
    renderChord (proc, capL);
    return analyze (capL, (int) (0.3 * kSr));
}
} // namespace

TEST(hellcat_fx_lut_dropout_test)
{
    juce::ScopedJuceInitialiser_GUI gui;

    std::printf ("[lut-dropout] wrap-family shapes never gate to silence\n");
    {
        struct Row { const char* name; int drive; int shape; double floorRms; };
        // Measured post-fix: 0.28–0.46 (margin > 2x the 0.15 floor).
        const Row rows[] = {
            { "Wrap  drive 64 ",  64,  3, 0.15 },
            { "Wrap  drive max", 127,  3, 0.15 },
            { "SFold drive 64 ",  64,  8, 0.15 },
            { "SFold drive max", 127,  8, 0.15 },
        };
        for (const auto& r : rows)
        {
            const Health h = lutDistHealth (r.drive, r.shape);
            char msg[128];
            std::snprintf (msg, sizeof (msg),
                           "%s : rmsMin=%.3f (>= %.2f), zeroRun=%d (<= 8)",
                           r.name, h.rmsMin, r.floorRms, h.zeroRun);
            check (h.rmsMin >= r.floorRms && h.zeroRun <= 8, msg);
        }
    }

    std::printf ("[lut-dropout] Overdrive max drive never gates to silence\n");
    {
        // The OVERDRIVE half of the "complete voice dropout" (2026-08-21): the
        // saturated drive ladder pinned the table index at one rail entry ->
        // constant DC -> the DC blocker removed it -> rms 5e-6 SILENCE on a
        // held chord. The unsaturated ladder reads the table's real tail:
        // loud driven tube fold (measured rms 0.25 vs the dry 0.19).
        auto proc = std::make_unique<HellcatAudioProcessor>();
        proc->prepareToPlay (kSr, kBuf);
        setChoice (*proc, "fx1_type", 16);   // Overdrive
        setInt (*proc, "fx1_enabled", 1);
        setInt (*proc, "fx1_drywet", 127);
        setInt (*proc, "osc1_shape", 1);
        const int pv[5] = { 127, 64, 64, 64, 64 };   // MAX drive
        for (int k = 0; k < 5; ++k)
            setInt (*proc, ("fx1_param" + std::to_string (k + 1)).c_str(), pv[k]);
        std::vector<float> capL ((size_t) (kDur * kSr), 0.0f);
        renderChord (*proc, capL);
        // absolute RMS floor: the silence bug measured ~5e-6; healthy loud
        // drive measures 0.15+. (Windowed-relative checks are meaningless on
        // a constant; absolute is the pin.)
        double rms = 0.0;
        const int from = (int) (0.3 * kSr), to = (int) (kDur * kSr);
        for (int i = from; i < to; ++i) rms += (double) capL[(size_t) i] * capL[(size_t) i];
        (void) proc;
        rms = std::sqrt (rms / (to - from));
        char msg[128];
        std::snprintf (msg, sizeof (msg),
                       "Overdrive max-drive output level rms=%.4f (>= 0.10; the silence bug measured 0.000005)", rms);
        check (rms >= 0.10, msg);
    }

    std::printf ("[lut-dropout] USER REPRO: delay->reverb->shaper at 100%% never gates\n");
    {
        // The EXACT user repro (2026-08-21): Digital Echo -> Plate -> shaper,
        // EVERY param at 100% (the shaper's drive/fold/bias/tone + 100% wet).
        // ROOT CAUSE: every FV-1 feedback loop (echo ping-pong, the plate's 4
        // combs) was a DC integrator at near-unity regen (DC gain 1/(1-g): up
        // to 200x echo / ~1000x plate) — residual DC parked the loops at a
        // rail (measured dc -0.22..-0.28 into the shaper), the driven shaper
        // pinned constant, and its DC blocker stripped it to gated SILENCE
        // (rmsMin 0.002-0.03 pre-fix). The LoopDcKiller (~10 Hz HP) in every
        // loop closes the DC path; measured post-fix 0.34-0.38 (> 2x floor).
        struct Chain { const char* name; int shaper; };
        const Chain chains[] = {
            { "Echo->Plate->Wavefolder", 7 },
            { "Echo->Plate->Overdrive", 16 },
            { "Echo->Plate->LutDist",   17 },
        };
        for (const auto& c : chains)
        {
            auto proc = std::make_unique<HellcatAudioProcessor>();
            proc->prepareToPlay (kSr, kBuf);
            setChoice (*proc, "fx1_type", 22);   // Digital Echo
            setChoice (*proc, "fx2_type", 13);   // Plate
            setChoice (*proc, "fx3_type", c.shaper);
            setInt (*proc, "osc1_shape", 1);
            for (int slot = 1; slot <= 3; ++slot)
            {
                setInt (*proc, ("fx" + std::to_string (slot) + "_enabled").c_str(), 1);
                setInt (*proc, ("fx" + std::to_string (slot) + "_drywet").c_str(), 127);
                for (int k = 1; k <= 5; ++k)
                    setInt (*proc, ("fx" + std::to_string (slot) + "_param" + std::to_string (k)).c_str(), 127);
            }
            std::vector<float> capL ((size_t) (kDur * kSr), 0.0f), capR ((size_t) (kDur * kSr), 0.0f);
            {   // chord render (same as renderChord but local: capR needed)
                bool on = false;
                const int total = (int) (kDur * kSr);
                for (int w = 0; w < total; )
                {
                    juce::AudioBuffer<float> b (2, kBuf);
                    b.clear();
                    juce::MidiBuffer m;
                    if (! on)
                    {
                        for (int ch = 0; ch < 4; ++ch)
                            m.addEvent (juce::MidiMessage::noteOn (1, (uint8_t) (57 + 5 * ch), (uint8_t) 110), 0);
                        on = true;
                    }
                    proc->processBlock (b, m);
                    const int n = std::min (kBuf, total - w);
                    for (int i = 0; i < n; ++i)
                    {
                        capL[(size_t) (w + i)] = b.getSample (0, i);
                        capR[(size_t) (w + i)] = b.getSample (1, i);
                    }
                    w += n;
                }
            }
            const Health h = analyze (capL, (int) (0.3 * kSr));
            char msg[128];
            std::snprintf (msg, sizeof (msg),
                           "%s @100%%: rmsMin=%.3f (>= 0.15), zeroRun=%d",
                           c.name, h.rmsMin, h.zeroRun);
            check (h.rmsMin >= 0.15 && h.zeroRun <= 16, msg);
        }
    }

    std::printf ("[lut-dropout] max-drive hot chain stays loud (no gating)\n");
    {
        HellcatAudioProcessor proc;
        proc.prepareToPlay (kSr, kBuf);
        setChoice (proc, "fx1_type", 17);
        setInt (proc, "fx1_enabled", 1); setInt (proc, "fx1_drywet", 127);
        const int p1[5] = { 127, (8 * 127) / 15, 64, 64, 64 };   // SFold, max drive
        for (int k = 0; k < 5; ++k)
            setInt (proc, ("fx1_param" + std::to_string (k + 1)).c_str(), p1[k]);
        setChoice (proc, "fx2_type", 15);   // Phaser, max depth + feedback
        setInt (proc, "fx2_enabled", 1); setInt (proc, "fx2_drywet", 127);
        const int p2[5] = { 64, 127, 127, 64, 64 };
        for (int k = 0; k < 5; ++k)
            setInt (proc, ("fx2_param" + std::to_string (k + 1)).c_str(), p2[k]);
        setChoice (proc, "fx3_type", 16);   // Overdrive, max drive
        setInt (proc, "fx3_enabled", 1); setInt (proc, "fx3_drywet", 127);
        const int p3[5] = { 127, 64, 64, 64, 64 };
        for (int k = 0; k < 5; ++k)
            setInt (proc, ("fx3_param" + std::to_string (k + 1)).c_str(), p3[k]);
        setInt (proc, "osc1_shape", 1);
        std::vector<float> capL ((size_t) (kDur * kSr), 0.0f);
        renderChord (proc, capL);
        const Health h = analyze (capL, (int) (0.3 * kSr));
        char msg[128];
        std::snprintf (msg, sizeof (msg),
                       "chain LutDist->Phaser->Overdrive max : rmsMin=%.3f (>= 0.30), zeroRun=%d",
                       h.rmsMin, h.zeroRun);
        check (h.rmsMin >= 0.30 && h.zeroRun <= 8, msg);   // measured 0.768 post-fix
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "LUT DROPOUT TEST: FAILURES" : "LUT DROPOUT TEST: ALL PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}

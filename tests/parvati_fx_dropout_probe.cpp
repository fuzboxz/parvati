// FX dropout/quality probe (2026-08-21): per-FX full parameter sweep measuring
//   - max consecutive near-zero samples mid-note (dropout runs)
//   - railing fraction (|x| >= 0.999 — saturated garbage)
//   - DC offset
//   - per-window RMS collapse (power < 1% of the running median)
//   - worst curvature-immune impulse (same detector family as the onset test)
// over a 3 s held loud chord at 48k/512, for Phaser / Overdrive / LUT Dist /
// Wavefolder. NOT a pass/fail gate — a diagnostic dump.
#include <array>
#include <algorithm>
#include "unified_test_runner.h"
#include "test_utils.h"
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "PluginProcessor.h"
#include "dsp/fx/fv1/Fv1Overdrive.h"
#include "dsp/fx/FxTypes.h"

namespace
{

struct Metrics
{
    int    zeroRun   = 0;    // longest consecutive |x| < 1e-5 run
    double railFrac  = 0.0;  // fraction |x| >= 0.999
    double dc        = 0.0;  // mean
    double rmsMin    = 1.0;  // min windowed RMS / median windowed RMS
    double worstImp  = 0.0;
};

Metrics analyze (const std::vector<float>& out, int from)
{
    Metrics m;
    const int n = (int) out.size();
    // zero runs + dc + rail
    int run = 0; double sum = 0; int rails = 0; int cnt = 0;
    for (int i = from; i < n; ++i)
    {
        const float v = out[(size_t) i];
        sum += v; ++cnt;
        if (std::fabs (v) >= 0.999f) ++rails;
        if (std::fabs (v) < 1.0e-5f) { if (++run > m.zeroRun) m.zeroRun = run; }
        else run = 0;
    }
    m.dc = sum / std::max (1, cnt);
    m.railFrac = (double) rails / std::max (1, cnt);
    // windowed RMS collapse (64-sample windows)
    std::vector<double> wr;
    for (int i = from; i + 64 <= n; i += 64)
    {
        double s = 0;
        for (int k = 0; k < 64; ++k) { const double v = out[(size_t) (i + k)]; s += v * v; }
        wr.push_back (std::sqrt (s / 64.0));
    }
    if (! wr.empty())
    {
        std::vector<double> sorted = wr;
        std::sort (sorted.begin(), sorted.end());
        const double med = std::max (1e-9, sorted[sorted.size() / 2]);
        for (double r : wr) m.rmsMin = std::min (m.rmsMin, r / med);
    }
    // impulse detector (onset-test family)
    for (int i = from + 1 + 64; i < n; ++i)
    {
        float window[64];
        for (int k = 0; k < 64; ++k)
        {
            const int j = i - 64 + k;
            window[k] = std::fabs (out[(size_t) j] - out[(size_t) (j - 1)]);
        }
        std::sort (window, window + 64);
        const float base = window[60];
        const float d = std::fabs (out[(size_t) i] - out[(size_t) (i - 1)]);
        if (d > 8.0f * base && d > 0.004f) m.worstImp = std::max (m.worstImp, (double) d);
    }
    return m;
}

void render (ParvatiAudioProcessor& proc, double sr, int bufSize,
             std::vector<float>& capL, std::vector<float>& capR)
{
    const int total = (int) capL.size();
    int written = 0; bool noteOn = false;
    while (written < total)
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
        for (int i = 0; i < n; ++i)
        {
            capL[(size_t) (written + i)] = buf.getSample (0, i);
            capR[(size_t) (written + i)] = buf.getSample (1, i);
        }
        written += n;
    }
}

void probeFx (const char* name, int fxType, const int base[5],
              int sweepIdx, const std::vector<int>& sweepVals,
              double sr, int bufSize, double durSec)
{
    std::printf ("== %s (sweep fx1_param%d) ==\n", name, sweepIdx + 1);
    const int total = (int) (durSec * sr);
    for (int v : sweepVals)
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (sr, bufSize);
        setChoice (proc, "fx1_type", fxType);
        setInt (proc, "fx1_enabled", 1);
        setInt (proc, "fx1_drywet", 127);
        setInt (proc, "osc1_shape", 1);
        int pv[5];
        for (int k = 0; k < 5; ++k) pv[k] = base[k];
        pv[sweepIdx] = v;
        for (int k = 0; k < 5; ++k)
            setInt (proc, ("fx1_param" + std::to_string (k + 1)).c_str(), pv[k]);
        std::vector<float> capL ((size_t) total, 0.0f), capR ((size_t) total, 0.0f);
        render (proc, sr, bufSize, capL, capR);
        const Metrics a = analyze (capL, (int) (0.3 * sr));
        std::printf ("  p%d=%3d : zeroRun=%4d rail=%5.3f%% dc=%+.3f rmsMin=%.3f imp=%.3f\n",
                     sweepIdx + 1, v, a.zeroRun, 100.0 * a.railFrac, a.dc, a.rmsMin, a.worstImp);
        if (a.rmsMin < 0.05)
        {
            // Dump the windowed-RMS time series around the collapse: find the
            // minimum window and print its neighbourhood (one line per window,
            // 64 samples per window).
            std::vector<double> wr;
            for (int i = (int) (0.3 * sr); i + 64 <= (int) capL.size(); i += 64)
            {
                double s2 = 0;
                for (int k = 0; k < 64; ++k) { const double v2 = capL[(size_t) (i + k)]; s2 += v2 * v2; }
                wr.push_back (std::sqrt (s2 / 64.0));
            }
            int minIdx = 0;
            for (size_t i = 1; i < wr.size(); ++i) if (wr[i] < wr[(size_t) minIdx]) minIdx = (int) i;
            const int from = std::max (0, minIdx - 10), to = std::min ((int) wr.size(), minIdx + 12);
            std::printf ("    collapse @ win %d (t=%.3fs): ", minIdx,
                         (0.3 * sr + 64.0 * minIdx) / sr);
            for (int i = from; i < to; ++i)
                std::printf ("%.4f ", wr[(size_t) i]);
            std::printf ("\n");
        }
    }
}
} // namespace

TEST(parvati_fx_dropout_probe)
{
    juce::ScopedJuceInitialiser_GUI gui;
    // Unified-harness port: the argv[1] sample-rate and argv[2] buffer-size
    // overrides are replaced by the fixed defaults the probe always used;
    // the opt-in DIRECT-ISOLATION (was argc>2) and EXTREME-matrix (was argc>3)
    // sections are gated on PARVATI_TEST_HOLD so the default run is
    // deterministic and terminates.
    const double sr = 48000.0;
    int bufSize = 512;
    const bool extended = std::getenv ("PARVATI_TEST_HOLD") != nullptr;
    const double dur = 3.0;

    // Phaser: p0 rate, p1 depth, p2 feedback, p3 center (p4 unused)
    {
        const int base[5] = { 64, 64, 64, 64, 64 };
        probeFx ("Phaser", (int) FxType::Phaser, base, 0, { 0, 32, 64, 96, 127 }, sr, bufSize, dur);
        probeFx ("Phaser", (int) FxType::Phaser, base, 1, { 0, 32, 64, 96, 127 }, sr, bufSize, dur);
        probeFx ("Phaser", (int) FxType::Phaser, base, 2, { 0, 32, 64, 96, 127 }, sr, bufSize, dur);
        probeFx ("Phaser", (int) FxType::Phaser, base, 3, { 0, 32, 64, 96, 127 }, sr, bufSize, dur);
    }
    // Overdrive: drive/level?
    {
        const int base[5] = { 64, 64, 64, 64, 64 };
        probeFx ("Overdrive", (int) FxType::Overdrive, base, 0, { 0, 32, 64, 96, 127 }, sr, bufSize, dur);
        probeFx ("Overdrive", (int) FxType::Overdrive, base, 1, { 0, 32, 64, 96, 127 }, sr, bufSize, dur);
    }
    // LUT Distortion
    {
        const int base[5] = { 64, 64, 64, 64, 64 };
        probeFx ("LutDistortion", (int) FxType::LutDistortion, base, 0, { 0, 32, 64, 96, 127 }, sr, bufSize, dur);
        probeFx ("LutDistortion", (int) FxType::LutDistortion, base, 1, { 0, 32, 64, 96, 127 }, sr, bufSize, dur);
    }
    // Wavefolder (the "Distortion" category)
    {
        const int base[5] = { 64, 64, 64, 64, 64 };
        probeFx ("Wavefolder", (int) FxType::Wavefolder, base, 0, { 0, 32, 64, 96, 127 }, sr, bufSize, dur);
    }
    // USER REPRO (2026-08-21): delay -> reverb -> shaper, shaper params
    // 100% everything (drive/fold/bias/tone + 100% wet).
    {
        struct R { const char* name; int fx1; int fx2; int fx3; };
        const R repro[] = {
            { "Echo->Plate->Wavefolder", 22, 13, 7 },
            { "Echo->Plate->Overdrive",  22, 13, 16 },
            { "Echo->Plate->LutDist",    22, 13, 17 },
            { "CDelay->CVerb->Wavefolder", 11, 3, 7 },
            { "CDelay->CVerb (no shaper)", 11, 3, 0 },
            { "CVerb alone",               0,  3, 0 },
        };
        for (const auto& r : repro)
        {
            ParvatiAudioProcessor proc;
            proc.prepareToPlay (sr, bufSize);
            setChoice (proc, "fx1_type", r.fx1);
            setChoice (proc, "fx2_type", r.fx2);
            setChoice (proc, "fx3_type", r.fx3);
            setInt (proc, "osc1_shape", 1);
            for (int slot = 1; slot <= 3; ++slot)
            {
                setInt (proc, ("fx" + std::to_string (slot) + "_enabled").c_str(), 1);
                setInt (proc, ("fx" + std::to_string (slot) + "_drywet").c_str(), 127);
                for (int k = 1; k <= 5; ++k)
                    setInt (proc, ("fx" + std::to_string (slot) + "_param" + std::to_string (k)).c_str(), 127);
            }
            const int total2 = (int) (dur * sr);
            std::vector<float> capL ((size_t) total2, 0.0f), capR ((size_t) total2, 0.0f);
            if (std::string (r.name).find ("Overdrive") != std::string::npos)
                ::setenv ("PARVATI_TAP_ON", "1", 1);
            render (proc, sr, bufSize, capL, capR);
            ::unsetenv ("PARVATI_TAP_ON");
            const Metrics a = analyze (capL, (int) (0.3 * sr));
            std::printf ("REPRO %-26s: zeroRun=%4d rail=%5.2f%% dc=%+.3f rmsMin=%.4f imp=%.3f\n",
                         r.name, a.zeroRun, 100.0 * a.railFrac, a.dc, a.rmsMin, a.worstImp);
            // Dump min-RMS window position + the peak level AT the shaper
            // input: render once more with fx3 DISABLED to see what level the
            // delay+reverb pair actually delivers.
            {
                ParvatiAudioProcessor p2;
                p2.prepareToPlay (sr, bufSize);
                setChoice (p2, "fx1_type", r.fx1);
                setChoice (p2, "fx2_type", r.fx2);
                setChoice (p2, "fx3_type", 0);
                setInt (p2, "osc1_shape", 1);
                for (int slot = 1; slot <= 2; ++slot)
                {
                    setInt (p2, ("fx" + std::to_string (slot) + "_enabled").c_str(), 1);
                    setInt (p2, ("fx" + std::to_string (slot) + "_drywet").c_str(), 127);
                    for (int k = 1; k <= 5; ++k)
                        setInt (p2, ("fx" + std::to_string (slot) + "_param" + std::to_string (k)).c_str(), 127);
                }
                const int total2 = (int) (dur * sr);
                std::vector<float> pre ((size_t) total2, 0.0f), preR ((size_t) total2, 0.0f);
                render (p2, sr, bufSize, pre, preR);
                double peak = 0, rms = 0; int cnt = 0;
                for (int i = (int) (0.3 * sr); i < total2; ++i)
                { const double v = std::fabs (pre[(size_t) i]); if (v > peak) peak = v; rms += v * v; ++cnt; }
                std::printf ("    shaper INPUT (delay+rev only): peak=%.3f rms=%.4f\n",
                             peak, std::sqrt (rms / cnt));
                // time structure of the input too (is IT pulsed?)
                std::vector<double> wr2;
                for (int i = (int) (0.3 * sr); i + 64 <= (int) pre.size(); i += 64)
                {
                    double s2 = 0;
                    for (int k = 0; k < 64; ++k) { const double v2 = pre[(size_t) (i + k)]; s2 += v2 * v2; }
                    wr2.push_back (std::sqrt (s2 / 64.0));
                }
                double mx2 = 0; for (double w : wr2) mx2 = std::max (mx2, w);
                {   // DC + zero-crossing census of the shaper input
                    double mean = 0; int zc = 0, cnt2 = 0;
                    for (int i = (int) (0.3 * sr) + 1; i < (int) pre.size(); ++i)
                    {
                        mean += pre[(size_t) i]; ++cnt2;
                        if ((pre[(size_t) i] < 0) != (pre[(size_t) (i - 1)] < 0)) ++zc;
                    }
                    std::printf ("    INPUT dc=%+.4f  zeroCrossings/sec=%.1f\n",
                                 mean / cnt2, zc / (cnt2 / sr));
                }
                std::printf ("    INPUT windows (0.3s..3s, one per 64smp): ");
                for (size_t i = 0; i < wr2.size(); i += 40)
                    std::printf ("%.3f ", wr2[i]);
                std::printf ("(max %.3f)\n", mx2);
            if (r.fx3 == 16)
            {
                std::vector<double> wr;
                for (int i = (int) (0.3 * sr); i + 64 <= (int) capL.size(); i += 64)
                {
                    double s2 = 0;
                    for (int k = 0; k < 64; ++k) { const double v2 = capL[(size_t) (i + k)]; s2 += v2 * v2; }
                    wr.push_back (std::sqrt (s2 / 64.0));
                }
                int minIdx = 0;
                for (size_t i = 1; i < wr.size(); ++i) if (wr[i] < wr[(size_t) minIdx]) minIdx = (int) i;
                const int f0 = std::max (0, minIdx - 20), t0 = std::min ((int) wr.size(), minIdx + 20);
                std::printf ("    collapse @t=%.3fs  rms windows:\n",
                             (0.3 * sr + 64.0 * minIdx) / sr);
                // waveform dump at the collapse
                {
                    const int c0 = (int) (0.3 * sr) + 64 * minIdx;
                    for (int i = c0 - 16; i < c0 + 48; i += 8)
                    {
                        std::printf ("      %7d : ", i);
                        for (int k = 0; k < 8; ++k) std::printf ("%+.4f ", capL[(size_t) (i + k)]);
                        std::printf ("\n");
                    }
                }
                for (int i = f0; i < t0; ++i)
                    std::printf ("%.4f ", wr[(size_t) i]);
                std::printf ("\n");
            }
            }
        }
    }

    // DIRECT ISOLATION: capture the Echo->Plate output, run it through a
    // standalone Fv1Overdrive at the repro params, and analyze.
    if (extended)
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (sr, bufSize);
        setChoice (proc, "fx1_type", 22);
        setChoice (proc, "fx2_type", 13);
        setInt (proc, "osc1_shape", 1);
        for (int slot = 1; slot <= 2; ++slot)
        {
            setInt (proc, ("fx" + std::to_string (slot) + "_enabled").c_str(), 1);
            setInt (proc, ("fx" + std::to_string (slot) + "_drywet").c_str(), 127);
            for (int k = 1; k <= 5; ++k)
                setInt (proc, ("fx" + std::to_string (slot) + "_param" + std::to_string (k)).c_str(), 127);
        }
        const int total2 = (int) (dur * sr);
        std::vector<float> pre ((size_t) total2, 0.0f), preR ((size_t) total2, 0.0f);
        render (proc, sr, bufSize, pre, preR);
        // standalone Overdrive at repro params (drive/bias/tone/level ALL 1.0)
        // — called ONE-GIANT-BLOCK vs CHAIN-CADENCE SUB-CHUNKS (~45 smp).
        {
            parvati::fv1::Fv1Overdrive od;
            od.prepare (sr, total2);
            std::array<float, kNumFxSlotParams> p5 = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
            od.setParams (p5);
            std::vector<float> out = pre, outR = preR;
            od.process (out.data(), outR.data(), total2);
            const Metrics a = analyze (out, (int) (0.3 * sr));
            std::printf ("ISO OD one-call    : zeroRun=%4d rail=%.2f%% rmsMin=%.4f imp=%.3f\n",
                         a.zeroRun, 100.0 * a.railFrac, a.rmsMin, a.worstImp);
        }
        {
            parvati::fv1::Fv1Overdrive od;
            od.prepare (sr, 512);   // chain-prep maxBlock
            std::array<float, kNumFxSlotParams> p5 = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
            od.setParams (p5);
            std::vector<float> out = pre, outR = preR;
            for (int off = 0; off < total2; off += 45)   // the ~980 Hz sub-chunk cadence
            {
                const int n = std::min (45, total2 - off);
                od.process (out.data() + off, outR.data() + off, n);
            }
            const Metrics a = analyze (out, (int) (0.3 * sr));
            std::printf ("ISO OD 45-smp chunks: zeroRun=%4d rail=%.2f%% rmsMin=%.4f imp=%.3f\n",
                         a.zeroRun, 100.0 * a.railFrac, a.rmsMin, a.worstImp);
        }
    }

    // EXTREME matrix (user report): every LUT shape at MAX drive, and a hot
    // 3-slot chain (LutDist max -> Phaser max fb -> Overdrive max).
    if (extended)
    {
        for (int shape = 0; shape < 16; ++shape)
        {
            ParvatiAudioProcessor proc;
            proc.prepareToPlay (sr, bufSize);
            setChoice (proc, "fx1_type", (int) FxType::LutDistortion);
            setInt (proc, "fx1_enabled", 1);
            setInt (proc, "fx1_drywet", 127);
            setInt (proc, "osc1_shape", 1);
            const int pv[5] = { 127, (shape * 127) / 15, 64, 64, 64 };
            for (int k = 0; k < 5; ++k)
                setInt (proc, ("fx1_param" + std::to_string (k + 1)).c_str(), pv[k]);
            const int total2 = (int) (dur * sr);
            std::vector<float> capL ((size_t) total2, 0.0f), capR ((size_t) total2, 0.0f);
            render (proc, sr, bufSize, capL, capR);
            const Metrics a = analyze (capL, (int) (0.3 * sr));
            std::printf ("SHAPE %2d drive-max : zeroRun=%4d rail=%5.3f%% dc=%+.3f rmsMin=%.3f imp=%.3f\n",
                         shape, a.zeroRun, 100.0 * a.railFrac, a.dc, a.rmsMin, a.worstImp);
        }
        // Hot chain: LutDist(SFold, max drive) -> Phaser(max depth+fb) -> Overdrive(max drive)
        {
            ParvatiAudioProcessor proc;
            proc.prepareToPlay (sr, bufSize);
            setChoice (proc, "fx1_type", (int) FxType::LutDistortion);
            setInt (proc, "fx1_enabled", 1); setInt (proc, "fx1_drywet", 127);
            const int p1[5] = { 127, (8 * 127) / 15, 64, 64, 64 };
            for (int k = 0; k < 5; ++k) setInt (proc, ("fx1_param" + std::to_string (k + 1)).c_str(), p1[k]);
            setChoice (proc, "fx2_type", (int) FxType::Phaser);
            setInt (proc, "fx2_enabled", 1); setInt (proc, "fx2_drywet", 127);
            const int p2[5] = { 64, 127, 127, 64, 64 };
            for (int k = 0; k < 5; ++k) setInt (proc, ("fx2_param" + std::to_string (k + 1)).c_str(), p2[k]);
            setChoice (proc, "fx3_type", (int) FxType::Overdrive);
            setInt (proc, "fx3_enabled", 1); setInt (proc, "fx3_drywet", 127);
            const int p3[5] = { 127, 64, 64, 64, 64 };
            for (int k = 0; k < 5; ++k) setInt (proc, ("fx3_param" + std::to_string (k + 1)).c_str(), p3[k]);
            setInt (proc, "osc1_shape", 1);
            const int total2 = (int) (dur * sr);
            std::vector<float> capL ((size_t) total2, 0.0f), capR ((size_t) total2, 0.0f);
            render (proc, sr, bufSize, capL, capR);
            const Metrics a = analyze (capL, (int) (0.3 * sr));
            std::printf ("CHAIN max : zeroRun=%4d rail=%5.3f%% dc=%+.3f rmsMin=%.3f imp=%.3f\n",
                         a.zeroRun, 100.0 * a.railFrac, a.dc, a.rmsMin, a.worstImp);
        }
    }
    std::printf ("PROBE DONE\n");
    return true;
}

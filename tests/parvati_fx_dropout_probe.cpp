// FX dropout/quality probe (2026-08-21): per-FX full parameter sweep measuring
//   - max consecutive near-zero samples mid-note (dropout runs)
//   - railing fraction (|x| >= 0.999 — saturated garbage)
//   - DC offset
//   - per-window RMS collapse (power < 1% of the running median)
//   - worst curvature-immune impulse (same detector family as the onset test)
// over a 3 s held loud chord at 48k/512, for Phaser / Overdrive / LUT Dist /
// Wavefolder. NOT a pass/fail gate — a diagnostic dump.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "PluginProcessor.h"
#include "dsp/fx/FxTypes.h"

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

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const double sr = (argc > 1) ? std::atof (argv[1]) : 48000.0;
    const int bufSize = (argc > 2) ? std::atoi (argv[2]) : 512;
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
    // EXTREME matrix (user report): every LUT shape at MAX drive, and a hot
    // 3-slot chain (LutDist max -> Phaser max fb -> Overdrive max).
    if (argc > 3)
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
    return 0;
}

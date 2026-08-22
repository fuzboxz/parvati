// Hot-input foldback probe (2026-08-21): drives the 1x-rate FV-1 effects
// (Phaser / Flanger / Chorus) and the distortions with a LOUD chord-level
// input (peak ~2.0 — legal through the chain's unity-per-voice ceiling) and
// measures inharmonic foldback energy (Goertzel at non-harmonic bins) plus
// the harmonic energy, at the effect's OUTPUT. Diagnostic dump.
#include <array>
#include <cmath>
#include "unified_test_runner.h"
#include <cstdio>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "dsp/fx/fv1/Fv1Phaser.h"
#include "dsp/fx/fv1/Fv1Flanger.h"

using parvati::fv1::Fv1Phaser;
using parvati::fv1::Fv1Flanger;

namespace
{
// Hann-windowed Goertzel (rect-window leakage was faking -56 dB "foldback"
// from the modulated comb's near-fundamental sideband cluster).
double goertzelHann (const std::vector<float>& x, double f, double sr)
{
    const double w = 2.0 * 3.141592653589793 * f / sr;
    const double coeff = 2.0 * std::cos (w);
    double s1 = 0.0, s2 = 0.0;
    const int n = (int) x.size();
    for (int i = 0; i < n; ++i)
    {
        const double wnd = 0.5 - 0.5 * std::cos (2.0 * 3.141592653589793 * i / (n - 1));
        const double s0 = wnd * x[(size_t) i] + coeff * s1 - s2;
        s2 = s1; s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

// Inharmonic foldback score: total energy at non-harmonic bins between
// 300 Hz and 15 kHz, relative to the strongest harmonic.
double foldbackScore (const std::vector<float>& x, double f0, double sr)
{
    const double fund = goertzelHann (x, f0, sr);
    double inharm = 0.0;
    for (int k = 1; k <= 60; ++k)
    {
        const double f = f0 * (k + 0.5);   // between-harmonic bins only
        if (f > 0.3 * sr) break;
        inharm += goertzelHann (x, f, sr);
    }
    return 10.0 * std::log10 ((inharm + 1e-300) / (fund + 1e-300));
}

constexpr double kInternal = 32768.0;

void runPhaser (const char* label, float inPeak, float fb)
{
    Fv1Phaser fx;
    fx.prepare (kInternal, 32768);
    std::array<float, kNumFxSlotParams> p = { 0.5f, 0.5f, fb, 0.5f, 0.0f };
    fx.setParams (p);
    const int N = 32768;
    std::vector<float> x ((size_t) N);
    const double f0 = 220.0;
    for (int i = 0; i < N; ++i)
        x[(size_t) i] = (float) (inPeak * std::sin (2.0 * 3.14159265 * f0 * i / kInternal));
    std::vector<float> rbuf ((size_t) N, 0.0f);
    fx.process (x.data(), rbuf.data(), N);   // phaser reads lin; writes lout=rout
    std::printf ("Phaser  %-22s in=%4.1f fb=%.1f : foldback %6.1f dB below f0\n",
                 label, inPeak, fb, foldbackScore (x, f0, kInternal));
}

void runFlanger (const char* label, float inPeak, float fb,
                 float rateP = 0.5f, float depthP = 0.6f, bool dumpBins = false)
{
    Fv1Flanger fx;
    fx.prepare (kInternal, 32768);
    std::array<float, kNumFxSlotParams> p = { rateP, depthP, 0.5f, fb, 0.0f };
    fx.setParams (p);
    const int N = 32768;
    std::vector<float> x ((size_t) N);
    const double f0 = 220.0;
    for (int i = 0; i < N; ++i)
        x[(size_t) i] = (float) (inPeak * std::sin (2.0 * 3.14159265 * f0 * i / kInternal));
    std::vector<float> rbuf ((size_t) N, 0.0f);
    float* L = x.data();
    float* R = rbuf.data();
    fx.process (L, R, N);
    std::printf ("Flanger %-22s in=%4.1f fb=%.2f : foldback %6.1f dB below f0\n",
                 label, inPeak, fb, foldbackScore (x, f0, kInternal));
    if (dumpBins)
    {
        const double fund = goertzelHann (x, f0, kInternal);
        double worst5[5] = {0,0,0,0,0}; double worst5f[5] = {0,0,0,0,0};
        for (int k = 1; k <= 200; ++k)
        {
            const double f = f0 * (k + 0.5);
            if (f > 0.45 * kInternal) break;
            const double g = goertzelHann (x, f, kInternal) / (fund + 1e-300);
            for (int s2i = 0; s2i < 5; ++s2i)
                if (g > worst5[s2i])
                {
                    for (int m = 4; m > s2i; --m) { worst5[m] = worst5[m-1]; worst5f[m] = worst5f[m-1]; }
                    worst5[s2i] = g; worst5f[s2i] = f;
                    break;
                }
        }
        std::printf ("    worst bins:");
        for (int s2i = 0; s2i < 5; ++s2i)
            std::printf ("  %.0fHz:%.0fdB", worst5f[s2i], 10.0 * std::log10 (worst5[s2i] + 1e-300));
        std::printf ("\n");
    }
}
} // namespace

void runEngine (const char* label, int fxType, double sr, int dummy = 0, float fb = -1.0f)
{
    ParvatiAudioProcessor proc;
    proc.prepareToPlay (sr, 512);
    {
        juce::AudioProcessorValueTreeState::Parameter* pr = nullptr;
        (void) pr;
    }
    juce::ignoreUnused (dummy);
    auto setInt = [] (ParvatiAudioProcessor& p, const char* id, int v)
    {
        if (auto* param = p.getApvts().getParameter (id))
            if (auto* ip = dynamic_cast<juce::AudioParameterInt*> (param))
                ip->setValueNotifyingHost (ip->convertTo0to1 ((float) v));
    };
    auto setChoice = [] (ParvatiAudioProcessor& p, const char* id, int v)
    {
        if (auto* param = p.getApvts().getParameter (id))
            if (auto* cp = dynamic_cast<juce::AudioParameterChoice*> (param))
                cp->setValueNotifyingHost (cp->convertTo0to1 ((float) v));
    };
    // EXACT lut-test sequence/order (bisecting why that engages and the old
    // order did not): type -> enabled -> drywet -> osc shape -> ALL 5 params.
    setChoice (proc, "fx1_type", fxType);
    setInt (proc, "fx1_enabled", 1);
    setInt (proc, "fx1_drywet", 127);
    setInt (proc, "osc1_shape", 1);   // saw (the lut test's shape)
    {
        int pv[5] = { 64, 64, 64, 64, 64 };
        if (fxType == 16) pv[0] = 127;   // Overdrive max drive when requested
        if (fxType == 15 && fb >= 0.0f) { pv[2] = (int) (127.0f * (0.5f + fb / 1.8f)); }
        if (fxType == 21 && fb >= 0.0f) { pv[3] = (int) (fb * 127.0f); }
        for (int k = 0; k < 5; ++k)
            setInt (proc, ("fx1_param" + std::to_string (k + 1)).c_str(), pv[k]);
    }
    std::printf ("    [params AFTER] type=%d enabled=%d drywet=%d\n",
                 (int) *proc.getApvts().getRawParameterValue ("fx1_type"),
                 (int) *proc.getApvts().getRawParameterValue ("fx1_enabled"),
                 (int) *proc.getApvts().getRawParameterValue ("fx1_drywet"));
    const int N = (int) (2.0 * sr);
    std::vector<float> cap ((size_t) N, 0.0f);
    bool on = false;
    for (int w = 0; w < N; )
    {
        juce::AudioBuffer<float> b (2, 512);
        b.clear();
        juce::MidiBuffer m;
        if (! on)
        {
            for (int c = 0; c < 4; ++c)
                m.addEvent (juce::MidiMessage::noteOn (1, (uint8_t) (57 + 5 * c), (uint8_t) 110), 0);
            on = true;
        }
        proc.processBlock (b, m);
        const int n = std::min (512, N - w);
        for (int i = 0; i < n; ++i) cap[(size_t) (w + i)] = b.getSample (0, i);
        w += n;
    }
    // analyze the last 1s (steady), f0 = note 57
    const double f0 = 440.0 * std::pow (2.0, (57 - 69) / 12.0);
    const int a0 = N - (int) (1.0 * sr);
    std::vector<float> seg (cap.begin() + a0, cap.end());
    {
        std::printf ("    [engine] fxRingCount(part0)=%d\n", proc.getEngine().debugLastFxRingCount (0));
        {
            // Wet check: does part 0's FX-output differ from its raw voicecard sum?
            const auto& fx = proc.getEngine().getFxOutputBuffers()[(size_t) 0];
            const auto& vc = proc.getEngine().getVoiceCardBuffers();
            double fxRms = 0.0;
            int nCmp = std::min (256, fx.getNumSamples());
            for (int i = 0; i < nCmp; ++i) fxRms += std::fabs (fx.getSample (0, i));
            std::printf ("    [engine] fxOut(part0) rms=%f (nonzero => chain wet)\n", fxRms / nCmp);
        }
        double fund = goertzelHann (seg, f0, sr);
        double rms = 0.0;
        for (float v : seg) rms += (double) v * v;
        rms = std::sqrt (rms / seg.size());
        std::printf ("engine %-10s @%.0fk : inharm %6.1f dB below f0 | fund=%.3e rms=%.4f peak=%.4f\n",
                     label, sr / 1000.0, foldbackScore (seg, f0, sr), fund, rms,
                     *std::max_element (seg.begin(), seg.end(), [] (float a, float b)
                                        { return std::fabs (a) < std::fabs (b); }));
    }
}

int probeMain()
{
    runEngine ("Overdrv max ", 16, 44100.0, 64);   // ENGAGEMENT SANITY: must differ wildly
    runEngine ("no-FX       ", 0,  44100.0, 64);
    runEngine ("Flanger fb0 ", 21, 44100.0, 64);
    runEngine ("Flanger fb.9", 21, 44100.0, 64, 0.9f);
    runEngine ("Phaser      ", 15, 44100.0, 64);
    runEngine ("Phaser fb.9 ", 15, 44100.0, 64, 0.9f);
    return 0;
}

TEST(parvati_fx_foldback_probe)
{
    // HARNESS NOTE (2026-08-21): without this initialiser the MessageManager
    // binds to a background thread at the first juce object, every main-thread
    // APVTS write takes the DEFERRED param path, and nothing drains without a
    // message loop — FX params silently never reach the engine (dry chains).
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("(lower dB = less foldback; baseline = the pure input sine itself)\n");
    {
        const int N = 32768;
        std::vector<float> x ((size_t) N);
        for (int i = 0; i < N; ++i)
            x[(size_t) i] = (float) (0.5 * std::sin (2.0 * 3.14159265 * 220.0 * i / kInternal));
        std::printf ("BASELINE pure sine             : foldback %6.1f dB below f0\n",
                     foldbackScore (x, 220.0, kInternal));
    }
    runPhaser ("unity input",      0.5f, 0.0f);
    runPhaser ("hot input",        2.0f, 0.0f);
    runPhaser ("hot input + fb",   2.0f, 0.9f);
    runPhaser ("unity + fb",       0.5f, 0.9f);
    runFlanger ("unity input",      0.5f, 0.0f);
    runFlanger ("hot input",        2.0f, 0.0f);
    runFlanger ("hot input + fb",   2.0f, 0.92f);
    runFlanger ("unity + fb",       0.5f, 0.92f);
    runFlanger ("fb, NO sweep",     0.5f, 0.92f, 0.0f, 0.0f, true);   // static delay
    runFlanger ("fb, slow sweep",   0.5f, 0.92f, 0.1f, 0.6f,  true);
    runFlanger ("fb, tiny depth",   0.5f, 0.92f, 0.5f, 0.02f, true);
    runFlanger ("no fb, full dp",   0.5f, 0.0f,  0.5f, 1.0f,  true);
    probeMain();
    // TIME-DOMAIN crackle check on the default case: worst curvature-immune
    // impulse (the dropout-probe detector) + consecutive-sample jump census.
    {
        Fv1Flanger fx; fx.prepare (kInternal, 32768);
        std::array<float, kNumFxSlotParams> p = { 0.5f, 0.6f, 0.5f, 0.92f, 0.0f }; fx.setParams (p);
        const int N = 32768;
        std::vector<float> x ((size_t) N), r ((size_t) N);
        for (int i = 0; i < N; ++i)
            x[(size_t) i] = (float) (0.5 * std::sin (2.0 * 3.14159265 * 220.0 * i / kInternal));
        fx.process (x.data(), r.data(), N);
        int impulses = 0; double worst = 0.0;
        std::vector<float> d ((size_t) N, 0.f);
        for (int i = 1; i < N; ++i) d[(size_t) i] = std::fabs (x[(size_t) i] - x[(size_t) (i - 1)]);
        for (int i = 65; i < N; ++i)
        {
            float w[64];
            for (int k = 0; k < 64; ++k) w[k] = d[(size_t) (i - 64 + k)];
            std::sort (w, w + 64);
            if (d[(size_t) i] > 8.f * w[60] && d[(size_t) i] > 0.004f)
            { ++impulses; worst = std::fmax (worst, d[(size_t) i]); }
        }
        std::printf ("TIME-DOMAIN flanger fb=0.92 default: impulses=%d worst=%.4f\n", impulses, worst);
        // dump 48 samples around the worst jump
        int wi = 1; for (int i = 1; i < N; ++i) if (d[(size_t) i] > d[(size_t) wi]) wi = i;
        std::printf ("  around worst jump @%d:\n", wi);
        for (int i = std::max (1, wi - 12); i < wi + 12; ++i)
            std::printf ("    %6d : %+9.6f  (d=%+9.6f)\n", i, x[(size_t) i], x[(size_t) i] - x[(size_t) (i - 1)]);
    }
    return true;
}

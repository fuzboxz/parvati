// Filter-card topology verification for Parvati.
// Renders a sustained saw through the full processor for each of the 3
// selectable filter cards (4-pole Ladder / 4-pole "4P" = two series
// StateVariableTPTFilter lowpass / 2-pole SVF) and asserts all three are
// DISTINCT filters: their output LEVELS differ and their sample-by-sample
// output differs pairwise (proving three different filter implementations,
// not one shared code path). Also verifies the Ladder "Filter Drive" control is
// wired (its output changes with drive). Sections [4]-[8] verify the 4th card,
// the SMR4 OTA cascade, at the model level (direct AnalogFilter class):
// stability across the cutoff/resonance plane, the 24 dB/oct slope, the
// saturation knee, the self-oscillation onset, and the processor wiring.
// Sections [9]-[14] verify the 5th card, the Polivoks SVF, the same way.
// Sections [15]-[19] pin its character layer: the growl dirt against a
// local tanh reference, the even harmonics from the asymmetric rails, the
// duty-cycle shift, the distinctness from the SMR4, and long-render
// stability at the extremes.

#include <algorithm>
#include "unified_test_runner.h"
#include <cmath>
#include <cstdio>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "dsp/analog_filter.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg) { std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg); if (! cond) ++g_failures; }

double rms (const std::vector<float>& v)
{
    if (v.empty()) return 0.0;
    double s = 0.0;
    for (float x : v) s += double (x) * double (x);
    return std::sqrt (s / double (v.size()));
}

// Steady-state gain of the OTA model at one sine frequency: renders a small
// (linear-zone) sine through a fresh filter and returns out-RMS / in-RMS from
// the second half of the render (first half = settling).
double otaSineGain (double hz, double sineHz, float res, float drive, float amp)
{
    ambika::dsp::AnalogFilter f;
    constexpr double kFs = 48000.0;
    f.prepare (kFs, 64);
    f.setTopology (ambika::dsp::FilterTopology::FOUR_POLE_OTA);
    f.setMode (0);
    f.setCutoffHz (static_cast<float> (hz));
    f.setResonance (res);
    f.setDrive (drive);
    f.commit();
    const int kN = 16384;
    double inSq = 0.0, outSq = 0.0;
    for (int i = 0; i < kN; ++i)
    {
        if ((i % 40) == 0)
            f.commit();
        const double ph = 2.0 * juce::MathConstants<double>::pi * sineHz * double (i) / kFs;
        const float x = amp * static_cast<float> (std::sin (ph));
        const float y = f.processSample (x);
        if (i >= kN / 2) { inSq += double (x) * double (x); outSq += double (y) * double (y); }
    }
    return std::sqrt (outSq / juce::jmax (1e-30, inSq));
}

// Same steady-state gain measurement for the Polivoks model. mode 0 = LP,
// mode 1 = BP. The measurement is AC: the character layer holds a small DC
// operating point (the input offset), so the gain must subtract the mean.
double polivoksSineGain (double hz, double sineHz, float res, float drive, float amp, int mode)
{
    ambika::dsp::AnalogFilter f;
    constexpr double kFs = 48000.0;
    f.prepare (kFs, 64);
    f.setTopology (ambika::dsp::FilterTopology::TWO_POLE_POLIVOKS);
    f.setMode (mode);
    f.setCutoffHz (static_cast<float> (hz));
    f.setResonance (res);
    f.setDrive (drive);
    f.commit();
    const int kN = 16384;
    double outSum = 0.0;
    std::vector<float> out;
    out.reserve (kN / 2);
    for (int i = 0; i < kN; ++i)
    {
        if ((i % 40) == 0)
            f.commit();
        const double ph = 2.0 * juce::MathConstants<double>::pi * sineHz * double (i) / kFs;
        const float x = amp * static_cast<float> (std::sin (ph));
        const float y = f.processSample (x);
        if (i >= kN / 2) { outSum += double (y); out.push_back (y); }
    }
    const double mean = outSum / double (out.size());
    double outSq = 0.0;
    for (double v : out) { const double d = v - mean; outSq += d * d; }
    const double inRms = amp / std::sqrt (2.0);
    return std::sqrt (outSq / double (out.size())) / juce::jmax (1e-30, inRms);
}

double diffRms (const std::vector<float>& a, const std::vector<float>& b)
{
    const size_t n = std::min (a.size(), b.size());
    std::vector<float> d;
    d.reserve (n);
    for (size_t i = 0; i < n; ++i) d.push_back (a[i] - b[i]);
    return rms (d);
}

// The PRE-REWORK Polivoks model (the shipped tanh pair), kept here as the
// character reference. Same linear skeleton, same onset, but smooth and
// symmetric: no asymmetry, no sag, no offset, no rate limit. The character
// tests compare against it.
struct PvTanhRef
{
    void prepare (double fs) { sampleRate = fs; reset(); }
    void reset() { s1 = 0.0; s2 = 0.0; }
    void setParams (double hz, float res, float drv)
    {
        const double nyq = 0.49 * sampleRate;
        const double fc  = juce::jlimit (20.0, juce::jmin (16000.0, nyq), hz);
        const double pi  = juce::MathConstants<double>::pi;
        g      = std::tan (pi * fc / sampleRate);
        knee   = 0.052 / juce::jmax (0.05, double (drv));
        gk     = g * knee;
        invKnee = 1.0 / knee;
        R      = 2.0 * (1.0 - juce::jlimit (0.0, 1.0, double (res)));
        invDen = 1.0 / (1.0 + g * R + g * g);
    }
    float processSample (float x)
    {
        const double yLin = (s1 + g * (double (x) - s2)) * invDen;
        double lo = s1 - gk, hi = s1 + gk;
        double ybp = juce::jlimit (lo, hi, yLin);
        double a = lo, b = hi;
        const double tol = 1.0e-6 * (1.0 + gk);
        for (int it = 0; it < 24; ++it)
        {
            const double tbp = std::tanh (ybp * invKnee);
            const double u   = double (x) - R * ybp - s2 - gk * tbp;
            const double tu  = std::tanh (u * invKnee);
            const double F   = ybp - s1 - gk * tu;
            if (std::fabs (F) < tol) break;
            if (F > 0.0) b = juce::jmin (b, ybp); else a = juce::jmax (a, ybp);
            const double dF = 1.0 + g * (1.0 - tu * tu) * (R + g * (1.0 - tbp * tbp));
            double yn = ybp - F / dF;
            if (! (yn > a && yn < b)) yn = 0.5 * (a + b);
            ybp = yn;
        }
        const double ylp = s2 + gk * std::tanh (ybp * invKnee);
        s1 = 2.0 * ybp - s1;
        s2 = 2.0 * ylp - s2;
        return static_cast<float> (ylp);
    }
    // Linear variant: the same skeleton with the tanh terms removed.
    float processSampleLinear (float x)
    {
        const double ybp = (s1 + g * (double (x) - s2)) * invDen;
        const double ylp = s2 + g * ybp;
        s1 = 2.0 * ybp - s1;
        s2 = 2.0 * ylp - s2;
        return static_cast<float> (ylp);
    }
    double sampleRate = 48000.0;
    double s1 = 0.0, s2 = 0.0, g = 0.0, knee = 0.05, gk = 0.0, invKnee = 20.0, R = 2.0, invDen = 1.0;
};

// Band-limited saw at `hz`, `harmonics` partials, amplitude `amp`, sample i.
float testSaw (double hz, int harmonics, float amp, int i, double fs)
{
    double v = 0.0;
    for (int h = 1; h <= harmonics; ++h)
        v += std::sin (2.0 * juce::MathConstants<double>::pi * hz * h * double (i) / fs) / double (h);
    return amp * static_cast<float> (v * (2.0 / juce::MathConstants<double>::pi));
}

// Amplitude of harmonic `k` of f0 in v, by direct correlation (Goertzel-style
// magnitude, exact for integer periods; the tests choose N accordingly).
double harmonicAmp (const std::vector<float>& v, double f0, int k, double fs)
{
    const double w = 2.0 * juce::MathConstants<double>::pi * f0 * double (k) / fs;
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < v.size(); ++i)
    {
        const double a = w * double (i);
        re += double (v[i]) * std::cos (a);
        im += double (v[i]) * std::sin (a);
    }
    return 2.0 * std::sqrt (re * re + im * im) / double (v.size());
}

// One-pole high-pass at cutoffHz: returns the RMS of the filtered signal.
double hfRms (const std::vector<float>& v, double cutoffHz, double fs)
{
    const double a = std::exp (-2.0 * juce::MathConstants<double>::pi * cutoffHz / fs);
    double lp = 0.0;
    double acc = 0.0; int n = 0;
    for (float x : v)
    {
        lp = a * lp + (1.0 - a) * double (x);
        const double hp = double (x) - lp;
        acc += hp * hp; ++n;
    }
    return std::sqrt (acc / double (n));
}

// Render `card` (0=Ladder,1=SSM2164,2=SVF): saw osc, low cutoff, no resonance,
// sustained note, capture the stereo channel-0 output and return its RMS.
double renderCard (ParvatiAudioProcessor& proc, int card, std::vector<float>& capture)
{
    auto& apvts = proc.getApvts();
    apvts.getParameterAsValue ("osc1_shape")      = 1.0f;     // SAW
    apvts.getParameterAsValue ("filter1_cutoff")  = 110.0f;   // ~low cutoff
    apvts.getParameterAsValue ("filter1_reso")    = 0.0f;     // no resonance
    apvts.getParameterAsValue ("filter1_mode")    = 0.0f;     // LP (fair for all 3)
    apvts.getParameterAsValue ("filter_card")     = static_cast<float> (card);
    proc.syncAllParamsToEngine();   // force topology + patch into the engine

    const int kBlocks = 220;
    const int kBlock  = 256;

    // Silence any note still ringing from a previous card, then flush a block.
    proc.getEngine().allNotesOff (1, true);
    {
        juce::AudioBuffer<float> flush (2, kBlock);
        juce::MidiBuffer empty;
        flush.clear();
        proc.processBlock (flush, empty);
    }

    juce::AudioBuffer<float> audio (2, kBlock);   // STEREO (voices write ch 0+1)
    capture.clear();

    bool noteSent = false;
    for (int b = 0; b < kBlocks; ++b)
    {
        juce::MidiBuffer midi;
        if (! noteSent) { midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0); noteSent = true; }
        audio.clear();
        proc.processBlock (audio, midi);
        const float* d = audio.getReadPointer (0);   // measure channel 0
        for (int i = 0; i < kBlock; ++i) capture.push_back (d[i]);
    }
    return rms (capture);
}
// Render the LADDER card at a given Filter Drive choice index (0="1.0"..7="12.0")
// with HIGH resonance (to exercise the tanh saturator) and return the RMS of the
// sustained output. Used to prove Filter Drive is actually wired.
double renderLadderDrive (ParvatiAudioProcessor& proc, int driveIndex, std::vector<float>& capture)
{
    auto& apvts = proc.getApvts();
    apvts.getParameterAsValue ("osc1_shape")      = 1.0f;     // SAW
    apvts.getParameterAsValue ("filter1_cutoff")  = 1500.0f;  // signal passes + resonance peaks
    apvts.getParameterAsValue ("filter1_reso")    = 0.8f;     // high Q -> drives the saturator
    apvts.getParameterAsValue ("filter1_mode")    = 0.0f;     // LP
    apvts.getParameterAsValue ("filter_card")     = 0.0f;     // Ladder
    apvts.getParameterAsValue ("filter_drive")    = static_cast<float> (driveIndex);
    proc.syncAllParamsToEngine();

    const int kBlocks = 220;
    const int kBlock  = 256;

    proc.getEngine().allNotesOff (1, true);
    {
        juce::AudioBuffer<float> flush (2, kBlock);
        juce::MidiBuffer empty;
        flush.clear();
        proc.processBlock (flush, empty);
    }

    juce::AudioBuffer<float> audio (2, kBlock);
    capture.clear();

    bool noteSent = false;
    for (int b = 0; b < kBlocks; ++b)
    {
        juce::MidiBuffer midi;
        if (! noteSent) { midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0); noteSent = true; }
        audio.clear();
        proc.processBlock (audio, midi);
        const float* d = audio.getReadPointer (0);
        for (int i = 0; i < kBlock; ++i) capture.push_back (d[i]);
    }
    return rms (capture);
}
}  // namespace

TEST(filter_topology_test)
{
    juce::ScopedJuceInitialiser_GUI gui;

    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);

    std::printf ("[1] The 3 filter cards render distinct output levels\n");
    std::vector<float> ladder, ssm, svf;
    const double rLadder = renderCard (proc, 0, ladder);
    const double rSSM    = renderCard (proc, 1, ssm);
    const double rSVF    = renderCard (proc, 2, svf);
    std::printf ("     RMS  Ladder=%.5f  SSM2164=%.5f  SVF=%.5f\n", rLadder, rSSM, rSVF);
    {
        const bool distinctLevels = std::fabs (rLadder - rSSM) > 1e-3
                                 && std::fabs (rLadder - rSVF) > 1e-3
                                 && std::fabs (rSSM    - rSVF) > 1e-3;
        check (distinctLevels, "3 distinct output levels (Ladder / SSM2164 / SVF)");
    }

    std::printf ("\n[2] The 3 cards are distinct FILTERS (pairwise sample-diff RMS)\n");
    {
        const double dLS = diffRms (ladder, ssm);   // ladder vs custom cascade
        const double dLV = diffRms (ladder, svf);   // 4-pole vs 2-pole
        const double dSV = diffRms (ssm,    svf);
        std::printf ("     diff RMS  Ladder-SSM2164=%.5f  Ladder-SVF=%.5f  SSM2164-SVF=%.5f\n", dLS, dLV, dSV);
        char msg[160];
        std::snprintf (msg, sizeof (msg), "ladder != 4P (2xSVF cascade): diff %.5f > 1e-3", dLS);
        check (dLS > 1e-3, msg);
        std::snprintf (msg, sizeof (msg), "ladder != SVF (4-pole vs 2-pole): diff %.5f > 1e-3", dLV);
        check (dLV > 1e-3, msg);
        std::snprintf (msg, sizeof (msg), "4P (2xSVF) != SVF: diff %.5f > 1e-3", dSV);
        check (dSV > 1e-3, msg);
    }

    std::printf ("\n[3] Ladder Filter Drive is wired (output changes with drive)\n");
    {
        // drive index 0 = "1.0", index 7 = "12.0". JUCE's LadderFilter scales both
        // the tanh input and its derived gain by drive, so a wired control must
        // change the output. If setDrive were never called (control not wired)
        // both renders would be identical (ladder at its 1.2 default) -> the
        // diff would be ~0 and this check would FAIL (that is the teeth).
        std::vector<float> lo, hi;
        const double rLo = renderLadderDrive (proc, 0, lo);   // drive 1.0
        const double rHi = renderLadderDrive (proc, 7, hi);   // drive 12.0
        const double dDH = diffRms (lo, hi);
        std::printf ("     Ladder RMS  drive1.0=%.5f  drive12.0=%.5f  diffRMS=%.5f\n", rLo, rHi, dDH);
        char msg[160];
        std::snprintf (msg, sizeof (msg), "Filter Drive changes the ladder output (diff %.5f > 1e-3)", dDH);
        check (dDH > 1e-3, msg);
        // Sanity: the drive levels themselves should differ (gain scales with drive).
        check (std::fabs (rLo - rHi) > 1e-3, "Ladder RMS differs between drive 1.0 and 12.0");
    }

    std::printf ("\n[4] OTA model: finite and bounded across the cutoff/resonance plane\n");
    {
        // Impulse + step + hot sine blend: exercises both transient and
        // sustained excitation. The bound 32 is generous: the tanh structure
        // bounds each stage's per-sample move to gk, and the closed loop is
        // bounded; 32 only guards a numeric runaway (a real failure mode would
        // hit infinity/NaN, caught by the finite check).
        bool allFinite = true, allBounded = true;
        for (double hz : { 30.0, 100.0, 500.0, 2000.0, 8000.0, 18000.0 })
        {
            for (float res : { 0.0f, 0.5f, 0.99f, 1.0f, 1.2f })
            {
                ambika::dsp::AnalogFilter f;
                f.prepare (48000.0, 64);
                f.setTopology (ambika::dsp::FilterTopology::FOUR_POLE_OTA);
                f.setMode (0);
                f.setCutoffHz (static_cast<float> (hz));
                f.setResonance (res);   // 1.2 clamps to 1.0 internally
                f.commit();
                for (int i = 0; i < 8192; ++i)
                {
                    if ((i % 40) == 0)
                        f.commit();
                    const double ph = 2.0 * juce::MathConstants<double>::pi * 220.0 * double (i) / 48000.0;
                    const float x = 0.5f * static_cast<float> (std::sin (ph))
                                  + (i == 0 ? 0.5f : 0.0f) + (i < 64 ? 0.3f : 0.0f);
                    const float y = f.processSample (x);
                    if (! std::isfinite (y)) { allFinite = false; }
                    if (std::fabs (y) > 32.0f) { allBounded = false; }
                }
            }
        }
        check (allFinite, "OTA renders finite at 30 Hz..18 kHz x resonance 0..1.2");
        check (allBounded, "OTA output stays below the documented bound (32) everywhere");
    }

    std::printf ("\n[5] OTA model: 24 dB/oct lowpass slope\n");
    {
        // Linear-zone measurement (amp 0.002 << knee): the small-signal
        // response of the cascade must roll off at ~24 dB/oct. Compare the
        // gain 2 and 4 octaves above the 1 kHz cutoff: 24 dB/oct predicts
        // 48 dB over 2 octaves. Tolerance +-6 dB (measurement + analog-style
        // resonance skirt).
        const double g2k = otaSineGain (1000.0, 2000.0, 0.0f, 1.0f, 0.002f);
        const double g8k = otaSineGain (1000.0, 8000.0, 0.0f, 1.0f, 0.002f);
        const double db = 20.0 * std::log10 (g8k / juce::jmax (1e-30, g2k));
        std::printf ("     gain 2k=%.6f  8k=%.6f  slope=%.1f dB/2oct (expect ~ -48)\n", g2k, g8k, db);
        char msg[128];
        std::snprintf (msg, sizeof (msg), "OTA slope -42..-54 dB per 2 octaves (got %.1f)", db);
        check (db < -42.0 && db > -54.0, msg);
    }

    std::printf ("\n[6] OTA model: saturation knee engages at hot level (2Vt pin)\n");
    {
        // Same cutoff, same drive, two input levels: cold (0.001, below the
        // knee) is linear; hot (0.5, far above the knee at drive 12) must
        // compress. This pins the knee constant meaningfully: with no knee
        // scaling the two gains would match within a few percent.
        const float drive = 12.0f;   // knee = 0.052/12 = 4.3e-3
        const double gCold = otaSineGain (1500.0, 1500.0, 0.0f, drive, 0.001f);
        const double gHot  = otaSineGain (1500.0, 1500.0, 0.0f, drive, 0.5f);
        const double ratio = gHot / juce::jmax (1e-30, gCold);
        std::printf ("     gain cold=%.4f  hot=%.4f  compression ratio=%.3f\n", gCold, gHot, ratio);
        char msg[128];
        std::snprintf (msg, sizeof (msg), "hot-input gain compresses below 0.75x cold (got %.3f)", ratio);
        check (ratio < 0.75, msg);
        // And at drive 1.0 the compression must be milder than at drive 12.
        const double gHot1 = otaSineGain (1500.0, 1500.0, 0.0f, 1.0f, 0.5f);
        const double ratio1 = gHot1 / juce::jmax (1e-30, otaSineGain (1500.0, 1500.0, 0.0f, 1.0f, 0.001f));
        std::snprintf (msg, sizeof (msg), "drive 12 compresses more than drive 1 (%.3f < %.3f)", ratio, ratio1);
        check (ratio < ratio1, msg);
    }

    std::printf ("\n[7] OTA model: self-oscillation onset at resonance 1.0\n");
    {
        // Seed one impulse, then silence. The impulse lands on the first
        // stage's tanh, so the seeded ring is microscopic — far below the
        // feedback knee. The linear analysis therefore holds: at res = 1.0
        // the loop gain at the 180-degree point is exactly 1 (kfb*G^4 scaled
        // by kOnset): the ring sustains. At res = 0.99 the pole pair sits
        // inside the unit circle: the ring decays.
        const auto renderRing = [] (float res, std::vector<float>& out)
        {
            ambika::dsp::AnalogFilter f;
            f.prepare (48000.0, 64);
            f.setTopology (ambika::dsp::FilterTopology::FOUR_POLE_OTA);
            f.setMode (0);
            f.setCutoffHz (1000.0f);
            f.setResonance (res);
            f.commit();
            const int kN = 3 * 48000;
            out.clear();
            out.reserve (static_cast<size_t> (kN));
            for (int i = 0; i < kN; ++i)
            {
                if ((i % 40) == 0)
                    f.commit();
                const float x = (i == 0) ? 0.5f : 0.0f;
                out.push_back (f.processSample (x));
            }
        };
        auto thirdMaxAbs = [] (const std::vector<float>& v, int third)
        {
            const size_t n = v.size() / 3;
            double m = 0.0;
            for (size_t i = size_t (third) * n; i < size_t (third + 1) * n && i < v.size(); ++i)
                m = std::max (m, std::fabs (double (v[i])));
            return m;
        };
        std::vector<float> ring10, ring099;
        renderRing (1.0f, ring10);
        renderRing (0.99f, ring099);
        const double m1_10 = thirdMaxAbs (ring10, 0),  m3_10 = thirdMaxAbs (ring10, 2);
        const double m1_99 = thirdMaxAbs (ring099, 0), m3_99 = thirdMaxAbs (ring099, 2);
        const double gMax = *std::max_element (ring10.begin(), ring10.end(),
            [] (float a, float b) { return std::fabs (a) < std::fabs (b); });
        std::printf ("     res 1.00: third1=%.5f third3=%.5f (sustain)   res 0.99: third1=%.5f third3=%.5f (decay)\n",
                     m1_10, m3_10, m1_99, m3_99);
        check (m3_10 > 0.3 * m1_10 && m1_10 > 1e-9, "OTA ring sustains at resonance 1.0 (onset tracks the knob)");
        check (m3_10 < 8.0 && std::isfinite (gMax), "OTA self-oscillation stays bounded");
        check (m3_99 < 0.05 * m1_99 && m1_99 > 1e-9, "OTA ring decays at resonance 0.99");
    }

    std::printf ("\n[8] OTA card (filter_card=3) through the processor: a 4th distinct filter\n");
    {
        std::vector<float> ota;
        const double rOTA = renderCard (proc, 3, ota);
        std::printf ("     RMS  OTA(SMR4)=%.5f\n", rOTA);
        const double d0 = diffRms (ladder, ota);
        const double d1 = diffRms (ssm, ota);
        const double d2 = diffRms (svf, ota);
        std::printf ("     diff RMS  OTA-Ladder=%.5f  OTA-4P=%.5f  OTA-SVF=%.5f\n", d0, d1, d2);
        check (d0 > 1e-3 && d1 > 1e-3 && d2 > 1e-3, "OTA card differs from all 3 existing cards");
        check (rOTA > 1e-3, "OTA card carries energy");
    }

    std::printf ("\n[9] Polivoks model: finite and bounded across the cutoff/resonance plane\n");
    {
        // Impulse + step + hot sine blend, LP and BP taps: exercises transient
        // and sustained excitation. The bound 32 is a numeric guard only. The
        // tanh integrators bound the loop; a real failure hits inf/NaN.
        bool allFinite = true, allBounded = true;
        for (double hz : { 30.0, 100.0, 500.0, 2000.0, 8000.0, 18000.0 })
        {
            for (float res : { 0.0f, 0.5f, 0.99f, 1.0f, 1.2f })
            {
                for (int mode : { 0, 1 })
                {
                    ambika::dsp::AnalogFilter f;
                    f.prepare (48000.0, 64);
                    f.setTopology (ambika::dsp::FilterTopology::TWO_POLE_POLIVOKS);
                    f.setMode (mode);
                    f.setCutoffHz (static_cast<float> (hz));
                    f.setResonance (res);   // 1.2 clamps to 1.0 internally
                    f.commit();
                    for (int i = 0; i < 8192; ++i)
                    {
                        if ((i % 40) == 0)
                            f.commit();
                        const double ph = 2.0 * juce::MathConstants<double>::pi * 220.0 * double (i) / 48000.0;
                        const float x = 0.5f * static_cast<float> (std::sin (ph))
                                      + (i == 0 ? 0.5f : 0.0f) + (i < 64 ? 0.3f : 0.0f);
                        const float y = f.processSample (x);
                        if (! std::isfinite (y)) { allFinite = false; }
                        if (std::fabs (y) > 32.0f) { allBounded = false; }
                    }
                }
            }
        }
        check (allFinite, "Polivoks renders finite at 30 Hz..18 kHz x resonance 0..1.2 (LP and BP)");
        check (allBounded, "Polivoks output stays below the documented bound (32) everywhere");
    }

    std::printf ("\n[10] Polivoks model: 12 dB/oct lowpass slope\n");
    {
        // Linear-zone measurement (amp 0.002 << knee). A 2-pole LP rolls off
        // at ~12 dB/oct: 24 dB over 2 octaves. Tolerance +-6 dB.
        const double g2k = polivoksSineGain (1000.0, 2000.0, 0.0f, 1.0f, 0.002f, 0);
        const double g8k = polivoksSineGain (1000.0, 8000.0, 0.0f, 1.0f, 0.002f, 0);
        const double db = 20.0 * std::log10 (g8k / juce::jmax (1e-30, g2k));
        std::printf ("     gain 2k=%.6f  8k=%.6f  slope=%.1f dB/2oct (expect ~ -24)\n", g2k, g8k, db);
        char msg[128];
        std::snprintf (msg, sizeof (msg), "Polivoks LP slope -18..-30 dB per 2 octaves (got %.1f)", db);
        check (db < -18.0 && db > -30.0, msg);
    }

    std::printf ("\n[11] Polivoks model: bandpass shape (BP tap rejects the skirts)\n");
    {
        // BP at resonance 0.5: energy concentrates at the cutoff. The skirts
        // at fc/8 and 8*fc must sit well below the center. Analog Q = 1/R = 1
        // predicts center/skirt ~ 8x. Margin: > 2.5x.
        const double gc  = polivoksSineGain (1000.0, 1000.0, 0.5f, 1.0f, 0.002f, 1);
        const double glo = polivoksSineGain (1000.0, 125.0, 0.5f, 1.0f, 0.002f, 1);
        const double ghi = polivoksSineGain (1000.0, 8000.0, 0.5f, 1.0f, 0.002f, 1);
        std::printf ("     BP center=%.4f  lo(125Hz)=%.4f  hi(8kHz)=%.4f\n", gc, glo, ghi);
        char msg[128];
        std::snprintf (msg, sizeof (msg), "BP center exceeds the low skirt 2.5x (%.3f)", gc / juce::jmax (1e-30, glo));
        check (gc > 2.5 * glo, msg);
        std::snprintf (msg, sizeof (msg), "BP center exceeds the high skirt 2.5x (%.3f)", gc / juce::jmax (1e-30, ghi));
        check (gc > 2.5 * ghi, msg);
    }

    std::printf ("\n[12] Polivoks model: saturation knee engages at hot level\n");
    {
        // Same cutoff and drive, cold (below the knee) vs hot (far above it at
        // drive 12). The hot gain must compress. Drive 1.0 compresses milder.
        const float drive = 12.0f;
        const double gCold = polivoksSineGain (1500.0, 1500.0, 0.0f, drive, 0.001f, 0);
        const double gHot  = polivoksSineGain (1500.0, 1500.0, 0.0f, drive, 0.5f, 0);
        const double ratio = gHot / juce::jmax (1e-30, gCold);
        std::printf ("     gain cold=%.4f  hot=%.4f  compression ratio=%.3f\n", gCold, gHot, ratio);
        char msg[128];
        std::snprintf (msg, sizeof (msg), "hot-input gain compresses below 0.75x cold (got %.3f)", ratio);
        check (ratio < 0.75, msg);
        const double gHot1 = polivoksSineGain (1500.0, 1500.0, 0.0f, 1.0f, 0.5f, 0);
        const double ratio1 = gHot1 / juce::jmax (1e-30, polivoksSineGain (1500.0, 1500.0, 0.0f, 1.0f, 0.001f, 0));
        std::snprintf (msg, sizeof (msg), "drive 12 compresses more than drive 1 (%.3f < %.3f)", ratio, ratio1);
        check (ratio < ratio1, msg);
    }

    std::printf ("\n[13] Polivoks model: self-oscillation onset at resonance 1.0\n");
    {
        // Seed one impulse, then silence. At res = 1.0 (R = 0) the linear loop
        // is exactly marginal: the ring sustains. At res = 0.99 the pole pair
        // sits strictly inside the unit circle: the ring decays fully. The
        // res = 1.2 setting clamps to 1.0 and matches the sustain.
        const auto renderRing = [] (float res, std::vector<float>& out)
        {
            ambika::dsp::AnalogFilter f;
            f.prepare (48000.0, 64);
            f.setTopology (ambika::dsp::FilterTopology::TWO_POLE_POLIVOKS);
            f.setMode (0);
            f.setCutoffHz (1000.0f);
            f.setResonance (res);
            f.commit();
            const int kN = 3 * 48000;
            out.clear();
            out.reserve (static_cast<size_t> (kN));
            for (int i = 0; i < kN; ++i)
            {
                if ((i % 40) == 0)
                    f.commit();
                const float x = (i == 0) ? 0.5f : 0.0f;
                out.push_back (f.processSample (x));
            }
        };
        auto thirdMaxAbs = [] (const std::vector<float>& v, int third)
        {
            // AC measurement: the character layer holds a small DC operating
            // point (the input offset), so the ring tails settle there, not at
            // zero. Subtract each third's mean, then take the peak.
            const size_t n = v.size() / 3;
            const size_t b = size_t (third) * n, e = size_t (third + 1) * n;
            double mean = 0.0;
            for (size_t i = b; i < e && i < v.size(); ++i)
                mean += double (v[i]);
            mean /= double (e - b);
            double m = 0.0;
            for (size_t i = b; i < e && i < v.size(); ++i)
                m = std::max (m, std::fabs (double (v[i]) - mean));
            return m;
        };
        std::vector<float> ring10, ring099, ring12;
        renderRing (1.0f, ring10);
        renderRing (0.99f, ring099);
        renderRing (1.2f, ring12);
        const double m1_10 = thirdMaxAbs (ring10, 0),   m3_10 = thirdMaxAbs (ring10, 2);
        const double m1_99 = thirdMaxAbs (ring099, 0), m3_99 = thirdMaxAbs (ring099, 2);
        const double m1_12 = thirdMaxAbs (ring12, 0),  m3_12 = thirdMaxAbs (ring12, 2);
        std::printf ("     res 1.00: third1=%.6f third3=%.6f (sustain)   res 0.99: third1=%.6f third3=%.6f (decay)   res 1.20: third3=%.6f\n",
                     m1_10, m3_10, m1_99, m3_99, m3_12);
        check (m3_10 > 0.2 * m1_10 && m1_10 > 1e-9, "Polivoks ring sustains at resonance 1.0 (onset tracks the knob)");
        check (m3_10 < 8.0 && std::isfinite (m3_10), "Polivoks self-oscillation stays bounded");
        check (m3_99 < 1e-3 * m1_99 && m1_99 > 1e-9, "Polivoks ring decays fully at resonance 0.99");
        check (m3_12 > 0.2 * m1_12, "resonance 1.2 clamps to the 1.0 onset (same sustain)");
    }

    std::printf ("\n[14] Polivoks card (filter_card=4) through the processor: a 5th distinct filter\n");
    {
        std::vector<float> pv;
        const double rPV = renderCard (proc, 4, pv);
        std::printf ("     RMS  Polivoks=%.5f\n", rPV);
        const double d0 = diffRms (ladder, pv);
        const double d1 = diffRms (ssm, pv);
        const double d2 = diffRms (svf, pv);
        std::vector<float> ota;
        renderCard (proc, 3, ota);
        const double d3 = diffRms (ota, pv);
        std::printf ("     diff RMS  PV-Ladder=%.5f  PV-4P=%.5f  PV-SVF=%.5f  PV-OTA=%.5f\n", d0, d1, d2, d3);
        check (d0 > 1e-3 && d1 > 1e-3 && d2 > 1e-3 && d3 > 1e-3, "Polivoks card differs from all 4 existing cards");
        check (rPV > 1e-3, "Polivoks card carries energy");
    }

    std::printf ("\n[15] Polivoks character: growl dirt exceeds the tanh reference at low cutoff\n");
    {
        // Hot band-limited saw at 110 Hz through fc = 150 Hz, res = 0.95,
        // drive = 1.2: the classic Polivoks growl corner. Dirt = deviation of
        // the character render from ITS OWN linear skeleton, plus the energy
        // above 1 kHz (a lowpass at 150 Hz passes no linear content there).
        // The tanh reference measures the same way. The character layer must
        // exceed it on both metrics.
        constexpr double kFs = 48000.0;
        constexpr int kN = 131072;
        std::vector<float> in (kN);
        for (int i = 0; i < kN; ++i)
            in[(size_t) i] = testSaw (110.0, 24, 0.8f, i, kFs);

        auto renderNew = [&] (bool linear, std::vector<float>& out)
        {
            ambika::dsp::AnalogFilter f;
            f.prepare (kFs, 64);
            f.setTopology (ambika::dsp::FilterTopology::TWO_POLE_POLIVOKS);
            f.setMode (0);
            f.setCutoffHz (150.0f);
            f.setResonance (0.95f);
            f.setDrive (1.2f);
            f.setTestLinearBypass (linear);
            f.commit();
            out.clear(); out.reserve (kN);
            for (int i = 0; i < kN; ++i) { if ((i % 40) == 0) f.commit(); out.push_back (f.processSample (in[(size_t) i])); }
        };
        auto renderRef = [&] (bool linear, std::vector<float>& out)
        {
            PvTanhRef f;
            f.prepare (kFs);
            f.setParams (150.0, 0.95f, 1.2f);
            out.clear(); out.reserve (kN);
            for (int i = 0; i < kN; ++i) out.push_back (linear ? f.processSampleLinear (in[(size_t) i]) : f.processSample (in[(size_t) i]));
        };
        std::vector<float> nN, nL, rN, rL;
        renderNew (false, nN); renderNew (true, nL);
        renderRef (false, rN); renderRef (true, rL);
        std::vector<float> dN (kN), dR (kN);
        for (int i = 0; i < kN; ++i) { dN[(size_t) i] = nN[(size_t) i] - nL[(size_t) i]; dR[(size_t) i] = rN[(size_t) i] - rL[(size_t) i]; }
        // Skip the first 16384 samples (settling) in every metric.
        std::vector<float> dN2 (dN.begin() + 16384, dN.end());
        std::vector<float> dR2 (dR.begin() + 16384, dR.end());
        std::vector<float> oN (nN.begin() + 16384, nN.end());
        std::vector<float> oR (rN.begin() + 16384, rN.end());
        const double dirtNew = rms (dN2), dirtRef = rms (dR2);
        const double hfNew = hfRms (oN, 1000.0, kFs), hfRef = hfRms (oR, 1000.0, kFs);
        std::printf ("     dirt: character=%.5f tanh=%.5f   HF(>1kHz): character=%.5f tanh=%.5f\n", dirtNew, dirtRef, hfNew, hfRef);
        char msg[160];
        std::snprintf (msg, sizeof (msg), "growl dirt exceeds the tanh reference (%.5f > %.5f)", dirtNew, dirtRef);
        check (dirtNew > dirtRef, msg);
        std::snprintf (msg, sizeof (msg), "high-frequency dirt exceeds the tanh reference (%.5f > %.5f)", hfNew, hfRef);
        check (hfNew > hfRef, msg);
        // Absolute pins at ~60 percent of the measured values (dirt 1.75,
        // HF 0.105): they catch a regression of the character layer while
        // leaving tuning latitude.
        check (dirtNew > 1.0, "growl dirt magnitude pin (character layer clearly engaged)");
        check (hfNew > 0.06, "high-frequency dirt magnitude pin");
    }

    std::printf ("\n[16] Polivoks character: asymmetric clipping makes even harmonics\n");
    {
        // Pure sine at the cutoff, mid band, moderate Q, hot level. The
        // asymmetric rails must make the 2nd harmonic far stronger than the
        // tanh reference (which is odd-symmetric: its 2nd harmonic sits near
        // the numerical floor).
        constexpr double kFs = 48000.0;
        constexpr int kN = 1 << 17;
        std::vector<float> oN, oR;
        {
            ambika::dsp::AnalogFilter f;
            f.prepare (kFs, 64);
            f.setTopology (ambika::dsp::FilterTopology::TWO_POLE_POLIVOKS);
            f.setMode (0);
            f.setCutoffHz (440.0f);
            f.setResonance (0.5f);
            f.setDrive (1.2f);
            f.commit();
            for (int i = 0; i < kN; ++i)
            {
                if ((i % 40) == 0) f.commit();
                const float x = 0.9f * std::sin (2.0f * juce::MathConstants<float>::pi * 440.0f * float (i) / 48000.0f);
                const float y = f.processSample (x);
                if (i >= kN / 4) oN.push_back (y);
            }
        }
        {
            PvTanhRef f;
            f.prepare (kFs);
            f.setParams (440.0, 0.5f, 1.2f);
            for (int i = 0; i < kN; ++i)
            {
                const float x = 0.9f * std::sin (2.0f * juce::MathConstants<float>::pi * 440.0f * float (i) / 48000.0f);
                const float y = f.processSample (x);
                if (i >= kN / 4) oR.push_back (y);
            }
        }
        const double h1N = harmonicAmp (oN, 440.0, 1, kFs);
        const double h2N = harmonicAmp (oN, 440.0, 2, kFs);
        const double h3N = harmonicAmp (oN, 440.0, 3, kFs);
        const double h1R = harmonicAmp (oR, 440.0, 1, kFs);
        const double h2R = harmonicAmp (oR, 440.0, 2, kFs);
        const double dbN = 20.0 * std::log10 (juce::jmax (1e-30, h2N) / juce::jmax (1e-30, h1N));
        const double dbR = 20.0 * std::log10 (juce::jmax (1e-30, h2R) / juce::jmax (1e-30, h1R));
        std::printf ("     2nd harmonic: character=%.1f dB  tanh=%.1f dB  (3rd character=%.1f dB)\n", dbN, dbR,
                     20.0 * std::log10 (juce::jmax (1e-30, h3N) / juce::jmax (1e-30, h1N)));
        char msg[160];
        std::snprintf (msg, sizeof (msg), "Polivoks 2nd harmonic above -35 dB (got %.1f)", dbN);
        check (dbN > -35.0, msg);
        std::snprintf (msg, sizeof (msg), "Polivoks 2nd harmonic exceeds the tanh reference by 10 dB (%.1f vs %.1f)", dbN, dbR);
        check (dbN - dbR > 10.0, msg);
    }

    std::printf ("\n[17] Polivoks character: asymmetric rates shift the output duty cycle\n");
    {
        // Long hot render at high drive. The asymmetric rails and the slower
        // negative rate cap move the output sign duty away from 50 percent.
        constexpr double kFs = 48000.0;
        constexpr int kN = 1 << 18;
        ambika::dsp::AnalogFilter f;
        f.prepare (kFs, 64);
        f.setTopology (ambika::dsp::FilterTopology::TWO_POLE_POLIVOKS);
        f.setMode (0);
        f.setCutoffHz (800.0f);
        f.setResonance (0.9f);
        f.setDrive (12.0f);
        f.commit();
        int pos = 0, count = 0;
        for (int i = 0; i < kN; ++i)
        {
            if ((i % 40) == 0) f.commit();
            const float x = 0.9f * std::sin (2.0f * juce::MathConstants<float>::pi * 220.0f * float (i) / 48000.0f);
            const float y = f.processSample (x);
            if (i >= kN / 4) { if (y > 0.0f) ++pos; ++count; }
        }
        const double duty = double (pos) / double (count);
        std::printf ("     duty cycle=%.4f (asymmetry %.2f%% from 50)\n", duty, 100.0 * std::fabs (duty - 0.5));
        char msg[160];
        std::snprintf (msg, sizeof (msg), "output duty cycle shifts more than 3%% from 50%% (got %.2f%%)", 100.0 * std::fabs (duty - 0.5));
        check (std::fabs (duty - 0.5) > 0.03, msg);
    }

    std::printf ("\n[18] Polivoks character: distinct from the SMR4 at matched settings\n");
    {
        // Identical settings through both custom cards. The outputs must
        // differ materially, and the even-harmonic profile must differ by a
        // wide margin (the SMR4 tanh stages are odd-symmetric). A pure sine
        // input carries no even partials, so any 2nd harmonic in the output
        // comes from the card itself.
        constexpr double kFs = 48000.0;
        auto renderCardModel = [] (ambika::dsp::FilterTopology t, std::vector<float>& out)
        {
            constexpr double kFs2 = 48000.0;
            constexpr int kN2 = 1 << 17;
            ambika::dsp::AnalogFilter f;
            f.prepare (kFs2, 64);
            f.setTopology (t);
            f.setMode (0);
            f.setCutoffHz (800.0f);
            f.setResonance (0.7f);
            f.setDrive (1.2f);
            f.commit();
            out.clear();
            for (int i = 0; i < kN2; ++i)
            {
                if ((i % 40) == 0) f.commit();
                const float x = 0.9f * std::sin (2.0f * juce::MathConstants<float>::pi * 800.0f * float (i) / 48000.0f);
                const float y = f.processSample (x);
                if (i >= kN2 / 4) out.push_back (y);
            }
        };
        std::vector<float> oP, oS;
        renderCardModel (ambika::dsp::FilterTopology::TWO_POLE_POLIVOKS, oP);
        renderCardModel (ambika::dsp::FilterTopology::FOUR_POLE_OTA, oS);
        const double rp = rms (oP), rs = rms (oS);
        const double nrd = diffRms (oP, oS) / juce::jmax (1e-30, 0.5 * (rp + rs));
        const double h1P = harmonicAmp (oP, 800.0, 1, kFs), h2P = harmonicAmp (oP, 800.0, 2, kFs);
        const double h1S = harmonicAmp (oS, 800.0, 1, kFs), h2S = harmonicAmp (oS, 800.0, 2, kFs);
        const double dbP = 20.0 * std::log10 (juce::jmax (1e-30, h2P) / juce::jmax (1e-30, h1P));
        const double dbS = 20.0 * std::log10 (juce::jmax (1e-30, h2S) / juce::jmax (1e-30, h1S));
        std::printf ("     normalized RMS diff=%.1f%%   2nd harmonic: Polivoks=%.1f dB SMR4=%.1f dB\n", 100.0 * nrd, dbP, dbS);
        char msg[160];
        std::snprintf (msg, sizeof (msg), "Polivoks differs from the SMR4 by more than 15%% RMS (got %.1f%%)", 100.0 * nrd);
        check (nrd > 0.15, msg);
        std::snprintf (msg, sizeof (msg), "even-harmonic profile differs by more than 10 dB (%.1f vs %.1f)", dbP, dbS);
        check (dbP - dbS > 10.0, msg);
    }

    std::printf ("\n[19] Polivoks character: long-render stability at the extremes\n");
    {
        // 5 seconds at every extreme corner (cutoff, resonance, drive) with a
        // hot saw. Every render must stay finite, bounded, and DC-stable.
        constexpr double kFs = 48000.0;
        constexpr int kN = 5 * 48000;
        bool allFinite = true, allBounded = true, allDcStable = true;
        for (double hz : { 30.0, 200.0, 1000.0, 18000.0 })
        {
            for (float res : { 0.8f, 0.99f, 1.0f, 1.2f })
            {
                for (float drv : { 0.05f, 1.2f, 12.0f })
                {
                    ambika::dsp::AnalogFilter f;
                    f.prepare (kFs, 64);
                    f.setTopology (ambika::dsp::FilterTopology::TWO_POLE_POLIVOKS);
                    f.setMode (0);
                    f.setCutoffHz (static_cast<float> (hz));
                    f.setResonance (res);
                    f.setDrive (drv);
                    f.commit();
                    double peak = 0.0, sum = 0.0, sumPrev = 0.0; int tail = 0, prev = 0;
                    for (int i = 0; i < kN; ++i)
                    {
                        if ((i % 40) == 0) f.commit();
                        const float y = f.processSample (testSaw (110.0, 24, 0.9f, i, kFs));
                        if (! std::isfinite (y)) allFinite = false;
                        peak = std::max (peak, std::fabs (double (y)));
                        if (i >= kN - 48000) { sum += double (y); ++tail; }
                        else if (i >= kN - 96000) { sumPrev += double (y); ++prev; }
                    }
                    const double mean = sum / double (tail);
                    const double meanPrev = sumPrev / double (prev);
                    // DC stability = the operating point CONVERGES. The
                    // asymmetric clipping legitimately rectifies a hot ring,
                    // so the mean may sit away from zero; it must stop moving.
                    if (peak > 8.0 || std::fabs (mean - meanPrev) > 0.05)
                        std::printf ("     corner hz=%.0f res=%.2f drv=%.2f peak=%.3f mean=%.4f dmean=%.4f\n", hz, res, drv, peak, mean, mean - meanPrev);
                    if (peak > 8.0) allBounded = false;
                    if (std::fabs (mean - meanPrev) > 0.05) allDcStable = false;
                }
            }
        }
        check (allFinite, "Polivoks stays finite through every extreme corner");
        check (allBounded, "Polivoks stays bounded (< 8, the supply clamp) through every extreme corner");
        check (allDcStable, "Polivoks DC operating point converges at every corner (last-second drift < 0.05)");
    }

    std::printf ("\n%s (%d failures)\n",
                 g_failures ? "FILTER TOPOLOGY TEST: FAILURES" : "FILTER TOPOLOGY TEST: ALL CHECKS PASSED",
                 g_failures);
    return g_failures == 0;
}

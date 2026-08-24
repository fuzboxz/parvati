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

double diffRms (const std::vector<float>& a, const std::vector<float>& b)
{
    const size_t n = std::min (a.size(), b.size());
    std::vector<float> d;
    d.reserve (n);
    for (size_t i = 0; i < n; ++i) d.push_back (a[i] - b[i]);
    return rms (d);
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

    std::printf ("\n%s (%d failures)\n",
                 g_failures ? "FILTER TOPOLOGY TEST: FAILURES" : "FILTER TOPOLOGY TEST: ALL CHECKS PASSED",
                 g_failures);
    return g_failures == 0;
}

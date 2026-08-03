// Filter-card topology verification for Parvati.
// Renders a sustained saw through the full processor for each of the 3
// selectable filter cards (4-pole LM13700 ladder / 4-pole "4P" = two series
// StateVariableTPTFilter lowpass / 2-pole SVF) and asserts all three are
// DISTINCT filters: their output LEVELS differ and their sample-by-sample
// output differs pairwise (proving three different filter implementations,
// not one shared code path). Also verifies the Ladder "Filter Drive" control is
// wired (its output changes with drive).

#include <algorithm>
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

int main()
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

    std::printf ("\n%s (%d failures)\n",
                 g_failures ? "FILTER TOPOLOGY TEST: FAILURES" : "FILTER TOPOLOGY TEST: ALL CHECKS PASSED",
                 g_failures);
    return g_failures ? 1 : 0;
}

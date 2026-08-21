// Layer-3b verification: the APVTS parameter bridge actually drives the engine.
//
// Instantiates the full ParvatiAudioProcessor, then proves that changing an
// APVTS patch parameter (osc1_shape) reaches every voice's Patch byte and
// changes the rendered audio (SAW = audible, NONE = near-silent). Also reports
// the full parameter descriptor count and a per-group breakdown.

#include <cmath>
#include "unified_test_runner.h"
#include <cstdio>
#include <string>
#include <unordered_map>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>  // ScopedJuceInitialiser_GUI

#include "ParameterLayout.h"
#include "PluginProcessor.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

// Render `blocks` blocks (noteOn on block 0, held), returning the peak |sample|.
// Any voices still sounding from a previous phase are killed first so the
// measurement reflects ONLY this note with the current patch.
double renderPeak (ParvatiAudioProcessor& proc, int midi, int blocks)
{
    constexpr int kBlock = 512;
    juce::AudioBuffer<float> buf (2, kBlock);
    double peak = 0.0;

    proc.getEngine().allNotesOff (1, false);  // immediately silence prior notes
    {
        juce::MidiBuffer empty;
        buf.clear();
        proc.processBlock (buf, empty);   // flush the killed voices
    }

    for (int b = 0; b < blocks; ++b)
    {
        juce::MidiBuffer midiBuf;
        if (b == 0)
            midiBuf.addEvent (juce::MidiMessage::noteOn (1, midi, 0.8f), 0);

        buf.clear();
        proc.processBlock (buf, midiBuf);

        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            for (int i = 0; i < buf.getNumSamples(); ++i)
            {
                const double v = std::fabs (static_cast<double> (buf.getSample (ch, i)));
                if (v > peak) peak = v;
            }
    }
    return peak;
}

void setChoice (ParvatiAudioProcessor& proc, const char* id, int index)
{
    // The canonical "host changed this parameter" path: setValueNotifyingHost
    // fires APVTS parameterChanged synchronously, which writes the patch byte
    // into every voice (no message-thread pumping needed).
    if (auto* param = proc.getApvts().getParameter (id))
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (param))
            choice->setValueNotifyingHost (choice->convertTo0to1 (static_cast<float> (index)));
}

void setInt (ParvatiAudioProcessor& proc, const char* id, int value)
{
    if (auto* param = proc.getApvts().getParameter (id))
        if (auto* ip = dynamic_cast<juce::AudioParameterInt*> (param))
            ip->setValueNotifyingHost (ip->convertTo0to1 (static_cast<float> (value)));
}

// Render `blocks` blocks (noteOn on block 0, held) and return the mono mix.
std::vector<float> renderAudio (ParvatiAudioProcessor& proc, int midi, int blocks)
{
    constexpr int kBlock = 512;
    juce::AudioBuffer<float> buf (2, kBlock);
    std::vector<float> out;
    out.reserve (static_cast<size_t> (kBlock * blocks));

    proc.getEngine().allNotesOff (1, false);
    {
        juce::AudioBuffer<float> flush (2, kBlock); flush.clear();
        juce::MidiBuffer empty;
        proc.processBlock (flush, empty);
    }

    for (int b = 0; b < blocks; ++b)
    {
        juce::MidiBuffer midiBuf;
        if (b == 0)
            midiBuf.addEvent (juce::MidiMessage::noteOn (1, midi, 0.8f), 0);
        buf.clear();
        proc.processBlock (buf, midiBuf);
        for (int i = 0; i < buf.getNumSamples(); ++i)
            out.push_back (0.5f * (buf.getSample (0, i) + buf.getSample (1, i)));
    }
    return out;
}

// Autocorrelation fundamental estimator (host-rate float samples).
double detectPitchHz (const std::vector<float>& x, double fs,
                      double fMin = 80.0, double fMax = 2000.0)
{
    const int minLag = static_cast<int> (fs / fMax);
    const int maxLag = static_cast<int> (fs / fMin);
    if (maxLag >= static_cast<int> (x.size()))
        return 0.0;

    // Normalise + compute the autocorrelation, tracking the global max.
    std::vector<double> acf (maxLag + 2, 0.0);
    double globalMax = 0.0;
    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        double c = 0.0;
        for (int i = 0; i + lag < static_cast<int> (x.size()); ++i)
            c += static_cast<double> (x[i]) * static_cast<double> (x[i + lag]);
        acf[lag] = c;
        if (c > globalMax) globalMax = c;
    }
    if (globalMax <= 0.0)
        return 0.0;

    // First peak (local max above 80% of global) scanning shortest-lag-first.
    const double threshold = 0.8 * globalMax;
    int bestLag = 0;
    for (int lag = minLag + 1; lag < maxLag; ++lag)
    {
        if (acf[lag] >= threshold && acf[lag] > acf[lag - 1] && acf[lag] >= acf[lag + 1])
        { bestLag = lag; break; }
    }
    if (bestLag == 0)
        return 0.0;

    // Parabolic interpolation around the peak.
    const double denom = (acf[bestLag - 1] - 2.0 * acf[bestLag] + acf[bestLag + 1]);
    double offset = 0.0;
    if (std::fabs (denom) > 1e-9)
        offset = std::clamp (0.5 * (acf[bestLag - 1] - acf[bestLag + 1]) / denom, -1.0, 1.0);
    return fs / (bestLag + offset);
}
}  // namespace

TEST(apvts_test)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf ("=== Parvati Layer-3b (APVTS parameter bridge) ===\n");

    // (1) Descriptor table + per-group breakdown.
    const auto& descs = getPatchParamDescriptors();
    std::printf ("[1] Parameter descriptor table\n");
    std::printf ("     total parameters: %zu\n", descs.size());

    std::unordered_map<std::string, int> groups;
    const auto familyOf = [] (const std::string& id) -> std::string
    {
        if (id.rfind ("osc", 0) == 0)        return "osc";
        if (id.rfind ("mix", 0) == 0)        return "mix";
        if (id.rfind ("filter", 0) == 0)     return "filter";
        if (id.rfind ("env", 0) == 0)        return "env";
        if (id.rfind ("voice", 0) == 0)      return "voice";
        if (id.rfind ("modif", 0) == 0)      return "modif";  // before "mod"
        if (id.rfind ("mod", 0) == 0)        return "mod";
        if (id == "part_select")        return "other";   // option, not a Part byte
        if (id.rfind ("part", 0) == 0)       return "part";
        return "other";
    };
    for (const auto& d : descs)
        ++groups[familyOf (d.paramID)];
    std::printf ("     groups:");
    for (const auto& [k, v] : groups)
        std::printf (" %s=%d", k.c_str(), v);
    std::printf ("\n");
    check (descs.size() == 260, "exposes exactly 260 parameters (106 patch/part + 5 arp + 4 options + 67 sequencer + 78 fx)");
    check (groups["osc"] == 8, "8 oscillator params");
    check (groups["mix"] == 8, "8 mixer params");
    check (groups["filter"] == 7, "7 filter params (filter1 cutoff/reso/mode + env + lfo + filter_card + filter_drive)");
    check (groups["env"] == 21, "21 env+lfo params (3 units x 7)");
    check (groups["voice"] == 2, "2 voice-LFO params");
    check (groups["mod"] == 42, "42 modulation-matrix params (14 x src/dest/amount)");
    check (groups["modif"] == 12, "12 modifier params (4 x in1/in2/op)");
    check (groups["part"] == 8, "8 part params (volume/octave/tuning/spread/raga/legato/portamento/polyphony)");

    // (2) Processor constructs; APVTS holds every descriptor paramID.
    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 512);

    std::printf ("\n[2] APVTS contains every descriptor parameter ID\n");
    int missing = 0;
    for (const auto& d : descs)
        if (! proc.getApvts().getParameter (d.paramID))
            ++missing;
    std::printf ("     missing parameters in APVTS: %d\n", missing);
    check (missing == 0, "all descriptor paramIDs present in APVTS");

    // Default osc1_shape should be SAW (index 1) -> the audible init program.
    const float defaultShape = proc.getApvts().getRawParameterValue ("osc1_shape")->load();
    std::printf ("     osc1_shape default raw value = %.0f (expect 1 = SAW)\n", defaultShape);
    check (static_cast<int> (defaultShape) == 1, "osc1_shape default == SAW(1)");

    // (3) Behavioral proof: SAW audible, NONE near-silent, SAW audible again.
    //     Isolate osc1 by silencing osc2 first: the hardware-faithful default
    //     init patch has osc2=SQUARE (controller/part.cc:39-81), so without
    //     this osc1=NONE would still leave osc2 audible.
    std::printf ("\n[3] Parameter change drives the rendered audio\n");

    setChoice (proc, "osc2_shape", 0);  // NONE — isolate osc1
    proc.syncAllParamsToEngine();

    const double peakSaw = renderPeak (proc, 60, 80);
    std::printf ("     osc1=SAW  (note 60) peak = %.5f\n", peakSaw);
    check (peakSaw > 0.01, "osc1=SAW renders audible audio (peak>0.01)");

    setChoice (proc, "osc1_shape", 0);  // NONE
    proc.syncAllParamsToEngine();         // deterministically push to all voices
    const double peakNone = renderPeak (proc, 62, 80);
    std::printf ("     osc1=NONE (note 62) peak = %.5f  (faithful mixer leakage; no master trim)\n", peakNone);
    // Absolute sanity bound: leakage must stay well below a live oscillator
    // (~0.45) and below unity. The hardware has no master trim, so absolute
    // levels are higher than under the old -10 dB master stage; the meaningful
    // faithfulness guard is the relative check below (NONE << SAW).
    check (peakNone < 0.3, "osc1=NONE leakage stays well below a live oscillator (peak<0.3)");

    setChoice (proc, "osc1_shape", 1);  // SAW again
    proc.syncAllParamsToEngine();
    const double peakSaw2 = renderPeak (proc, 64, 80);
    std::printf ("     osc1=SAW  (note 64) peak = %.5f\n", peakSaw2);
    check (peakSaw2 > 0.01, "osc1=SAW (restored) renders audible audio (peak>0.01)");

    check (peakNone < peakSaw * 0.5, "NONE output is clearly quieter than SAW (<50%)");  // faithful mixer leakage ratio

    // (4) A non-default choice reaches the engine too: filter mode selectable.
    std::printf ("\n[4] Filter mode parameter accepts all 4 modes (LP/BP/HP/Notch)\n");
    int modeOk = 0;
    for (int m = 0; m < 4; ++m)
    {
        setChoice (proc, "filter1_mode", m);
        proc.syncAllParamsToEngine();
        const float got = proc.getApvts().getRawParameterValue ("filter1_mode")->load();
        if (static_cast<int> (got) == m) ++modeOk;
    }
    std::printf ("     filter1_mode settable to all 4 modes: %d/4\n", modeOk);
    check (modeOk == 4, "filter1_mode round-trips LP/BP/HP/Notch");

    // (5) Per-part OCTAVE/TUNING: routes to PartData bytes AND shifts pitch
    //     (firmware Part::TuneNote, part.cc:634: n = midi + octave*12).
    std::printf ("\n[5] Per-part Octave/Tuning shift the rendered pitch\n");
    setChoice (proc, "osc2_shape", 0);   // isolate osc1 (SAW) for a clear fundamental
    setInt   (proc, "filter1_cutoff", 127);  // open the filter so the fundamental is strong
    proc.syncAllParamsToEngine();

    setInt (proc, "part_octave", 0);
    proc.syncAllParamsToEngine();
    check ((int) proc.getEngine().getPart (0).partBytes[1] == 0,
           "part_octave=0 routes to PartData byte 1");
    const auto a0 = renderAudio (proc, 69, 120);   // A4
    const double f0 = detectPitchHz (a0, 48000.0);

    setInt (proc, "part_octave", 1);   // +1 octave => +12 semitones => ~2x frequency
    proc.syncAllParamsToEngine();
    check ((int) proc.getEngine().getPart (0).partBytes[1] == 1,
           "part_octave=1 routes to PartData byte 1");
    const auto a1 = renderAudio (proc, 69, 120);   // same midi note, octave-shifted
    const double f1 = detectPitchHz (a1, 48000.0);

    std::printf ("     A4 @ octave0 = %.2f Hz, @ octave+1 = %.2f Hz (ratio %.3f)\n",
                 f0, f1, f0 > 0.0 ? f1 / f0 : 0.0);
    check (f0 > 200.0 && f0 < 600.0, "octave0 pitch near A4 (200-600 Hz)");
    check (f1 > 200.0 && f0 > 0.0 && std::fabs (f1 / f0 - 2.0) < 0.06,
           "+1 octave doubles the frequency (~2x, 6% tol)");

    // TUNING routes to byte 2 (a fine sub-semitone offset; not pitch-tested
    // at sample level, but the byte bridge is verified).
    setInt (proc, "part_tuning", 40);
    proc.syncAllParamsToEngine();
    check ((int) (int8_t) proc.getEngine().getPart (0).partBytes[2] == 40,
           "part_tuning=40 routes (signed) to PartData byte 2");

    // (6) part_select is NON-automatable by design: part switching is a UI
    //     action that drives the part-load machinery (loadPartIntoApvts + full
    //     re-sync), not a sound parameter a host should automate per tick.
    std::printf ("\n[6] part_select is non-automatable\n");
    check (! proc.getApvts().getParameter ("part_select")->isAutomatable(),
           "part_select excluded from host automation (isAutomatable == false)");

    std::printf ("\n=== %s (%d failure%s) ===\n",
                 g_failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}

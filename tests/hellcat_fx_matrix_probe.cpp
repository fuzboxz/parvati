// FX "loud crackle" scenario-matrix probe (diagnostic). Complements the onset
// probe with the realistic user-interaction events, per FX type:
//   A) note-on, fresh plugin, FX pre-enabled at full wet
//   B) note-on after 1s silence, FX running at full wet
//   C) fx1_enabled 0->1 mid-note (drywet already 100%)
//   D) fx1_type None->X mid-note (enabled, full wet)
//   E) drywet 0->127 step mid-note
//   F) note-on at 50% dry/wet (the common "just add some FX" setting)
// Each scenario reports the worst curvature-immune impulse + raw max |delta| in
// the 200 ms window after the event, vs the no-FX baseline.
//
// Build: linked as hellcat_fx_matrix_probe_test (EXCLUDE_FROM_ALL).

#include <algorithm>
#include "unified_test_runner.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "PluginProcessor.h"
#include "test_utils.h"              // shared setInt/setChoice (host-path helpers)

namespace
{

const char* fxName (int ti)
{
    switch (ti)
    {
        case 0: return "None";       case 1: return "Diffuser";
        case 2: return "PitchShift"; case 3: return "Reverb";
        case 4: return "LoopDelay";  case 5: return "WSOLA";
        case 6: return "Spectral";   case 7: return "Wavefolder";
        case 8: return "FreqShift";  case 9: return "RingMod";
        case 10: return "Resonator"; case 11: return "ClockedDly";
        case 12: return "Ensemble";  case 13: return "PlateRev";
        case 14: return "VinylComp"; case 15: return "Phaser";
        case 16: return "Overdrive"; case 17: return "LUT Dist";
        case 18: return "Comp"; case 19: return "Gate";
        case 20: return "Chorus"; case 21: return "Flanger";
        case 22: return "Echo"; case 23: return "Room"; case 24: return "Spring";
        default: return "?";
    }
}

struct WindowStats { double worstImp; double maxDelta; int impCount; };

// Worst curvature-immune impulse (8x the 93rd-percentile of the trailing
// 64-sample |delta| window, > 0.004 abs) + raw max |delta| over [from,to).
WindowStats analyze (const std::vector<float>& out, int from, int to)
{
    const int n = static_cast<int> (out.size ());
    std::vector<float> d (static_cast<size_t> (n), 0.0f);
    for (int i = std::max (from, 1); i < std::min (to, n); ++i)
        d[static_cast<size_t> (i)] = std::fabs (out[static_cast<size_t> (i)] - out[static_cast<size_t> (i - 1)]);
    WindowStats st { 0.0, 0.0, 0 };
    for (int i = std::max (from, 1); i < std::min (to, n); ++i)
        st.maxDelta = std::max (st.maxDelta, static_cast<double> (d[static_cast<size_t> (i)]));
    for (int i = std::max (from, 1) + 64; i < std::min (to, n); ++i)
    {
        float window[64];
        for (int k = 0; k < 64; ++k) window[k] = d[static_cast<size_t> (i - 64 + k)];
        std::sort (window, window + 64);
        const float base = window[60];
        if (d[static_cast<size_t> (i)] > 8.0f * base && d[static_cast<size_t> (i)] > 0.004f)
        {
            ++st.impCount;
            st.worstImp = std::max (st.worstImp, static_cast<double> (d[static_cast<size_t> (i)]));
        }
    }
    return st;
}

// Render engine: fresh processor; optional FX config applied at t=0 or at a
// mid-note event block; sustained note-on from block 0; captures main-bus L.
// eventKind: 0 = none. 1 = enable fx (block eventBlock). 2 = set type.
// 3 = drywet 0->127. fxPreSet = apply full FX config up front.
// chord = number of simultaneous note-ons (velocity 127 when > 1).
std::vector<float> render (int fxType, double sr, int bufferSize, double durSec,
                           int eventKind, int eventBlock, bool fxPreSet, int drywet,
                           int chord = 1)
{
    HellcatAudioProcessor proc;
    proc.prepareToPlay (sr, bufferSize);
    proc.getApvts().getParameterAsValue ("part_select") = 1.0f;
    setInt (proc, "osc1_shape", 1);              // SAW
    if (fxPreSet)
    {
        setChoice (proc, "fx1_type", fxType);
        setInt (proc, "fx1_enabled", 1);
        setInt (proc, "fx1_drywet", drywet);
        for (int k = 1; k <= 5; ++k)
        {
            // params 3+4 stay below the >0.5 freeze thresholds of the
            // buffer-based FX (Looper/WSOLA freeze = param3, Spectral =
            // param4); 64/127 = 0.504 would FREEZE them into silence.
            const int v = (k >= 3) ? 32 : 64;
            setInt (proc, ("fx1_param" + std::to_string (k)).c_str (), v);
        }
    }
    else if (eventKind != 2)
    {
        // type pre-set but disabled / drywet 0 (scenarios C/E start from None-ish)
        setChoice (proc, "fx1_type", fxType);
        for (int k = 1; k <= 5; ++k)
        {
            // params 3+4 stay below the >0.5 freeze thresholds of the
            // buffer-based FX (Looper/WSOLA freeze = param3, Spectral =
            // param4); 64/127 = 0.504 would FREEZE them into silence.
            const int v = (k >= 3) ? 32 : 64;
            setInt (proc, ("fx1_param" + std::to_string (k)).c_str (), v);
        }
    }

    const int total = static_cast<int> (durSec * sr);
    std::vector<float> cap (static_cast<size_t> (total), 0.0f);
    int written = 0, block = 0;
    bool noteOn = false;
    while (written < total)
    {
        juce::AudioBuffer<float> buf (2, bufferSize);
        buf.clear ();
        juce::MidiBuffer midi;
        if (! noteOn)
        {
            for (int c = 0; c < chord; ++c)
                midi.addEvent (juce::MidiMessage::noteOn (1, 60 + 4 * c,
                    static_cast<uint8_t> (chord > 1 ? 127 : 100)), 0);
            noteOn = true;
        }
        if (block == eventBlock)
        {
            if (eventKind == 1) { setInt (proc, "fx1_enabled", 1); setInt (proc, "fx1_drywet", 127); }
            if (eventKind == 2)
            {
                setChoice (proc, "fx1_type", fxType);
                setInt (proc, "fx1_enabled", 1);
                setInt (proc, "fx1_drywet", 127);
                for (int k = 1; k <= 5; ++k)
                {
                    const int v = (k >= 3) ? 32 : 64;
                    setInt (proc, ("fx1_param" + std::to_string (k)).c_str (), v);
                }
            }
            if (eventKind == 3)
            {
                setInt (proc, "fx1_enabled", 1);
                setInt (proc, "fx1_drywet", 127);
            }
        }
        proc.processBlock (buf, midi);
        const int n = std::min (bufferSize, total - written);
        const float* L = buf.getReadPointer (0);
        for (int i = 0; i < n; ++i)
            cap[static_cast<size_t> (written + i)] = L[i];
        written += n;
        ++block;
    }
    return cap;
}

void runScenario (const char* name, double sr, int bufSize,
                  int eventKind, int eventBlock, bool fxPreSet, int drywet,
                  double preSec, double winSec)
{
    const int winStart = static_cast<int> (preSec * sr);
    const int winLen   = static_cast<int> (winSec * sr);
    std::printf ("-- %s --\n", name);
    for (int t = 0; t <= 24; ++t)
    {
        const auto cap  = render (t, sr, bufSize, preSec + winSec, eventKind, eventBlock, fxPreSet, drywet);
        const auto base = analyze (cap, winStart, winStart + winLen);
        std::printf ("  %-11s worstImp=%.4f (%3d) maxDelta=%.4f%s\n", fxName (t),
                     base.worstImp, base.impCount, base.maxDelta,
                     base.worstImp > 0.10 ? "   <== LOUD" : (base.worstImp > 0.06 ? " <== ?" : ""));
    }
}

} // namespace

TEST(hellcat_fx_matrix_probe)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    constexpr double sr = 48000.0;
    constexpr int    buf = 256;
    std::printf ("=== FX crackle scenario matrix (48k, buf 256) ===\n\n");

    // A) fresh plugin, FX on, note-on at t=0: analyze first 200 ms
    runScenario ("A: note-on, FX pre-enabled full wet (first 200ms)",
                 sr, buf, 0, -1, true, 127, 0.0, 0.20);
    // A0) the same onset across rates x buffer sizes (the renderPartFx
    // sub-chunk cadence -- the previous crackle's driver -- depends on both).
    for (double r : { 44100.0, 48000.0, 96000.0 })
        for (int b : { 64, 128, 512, 1024 })
            runScenario ((std::string ("A0: onset, ") + std::to_string ((int) r)
                          + " Hz, buffer " + std::to_string (b)).c_str (),
                         r, b, 0, -1, true, 127, 0.0, 0.20);
    // A2) same, but sliced per 250 ms over 3 s to localize LATE crackles (the
    // buffer-based FX' wet arrives seconds in; WSOLA's first window splice
    // lands ~1.5 window periods in).
    std::printf ("-- A2: per-250ms worst impulse over first 3 s (full wet) --\n");
    for (int t = 0; t <= 24; ++t)
    {
        const auto cap = render (t, sr, buf, 3.0, 0, -1, true, 127);
        std::printf ("  %-11s", fxName (t));
        for (int slice = 0; slice < 12; ++slice)
        {
            const int from = slice * static_cast<int> (0.25 * sr);
            const auto st  = analyze (cap, from, from + static_cast<int> (0.25 * sr));
            std::printf (" %5.3f", st.worstImp);
        }
        std::printf ("\n");
    }
    // B) note re-onset after silence (2nd note): reuse onset probe semantics
    // C) enable mid-note at 0.5 s (drywet pre-set 127)
    runScenario ("C: fx enable 0->1 mid-note (drywet 100%)",
                 sr, buf, 1, static_cast<int> (0.5 * sr / buf), false, 127, 0.5, 0.20);
    // D) type None->X mid-note
    runScenario ("D: fx type None->X mid-note (enable+full wet)",
                 sr, buf, 2, static_cast<int> (0.5 * sr / buf), false, 127, 0.5, 0.20);
    // E) drywet 0->127 step mid-note
    runScenario ("E: drywet 0->127 mid-note",
                 sr, buf, 3, static_cast<int> (0.5 * sr / buf), false, 0, 0.5, 0.20);
    // F) note-on at 50% mix
    runScenario ("F: note-on, FX pre-enabled at 50% dry/wet",
                 sr, buf, 0, -1, true, 64, 0.0, 0.20);
    // G) LOUD chord onset (6 notes, vel 127) at full wet — the chain-input
    // ceiling / fold-clamp territory where the pre-Aug-17 bugs bit hardest.
    {
        const int winLen = static_cast<int> (0.25 * sr);
        std::printf ("-- G: 6-note chord onset, vel 127, full wet (first 250 ms) --\n");
        for (int t = 0; t <= 24; ++t)
        {
            const auto cap  = render (t, sr, buf, 0.5, 0, -1, true, 127, 6);
            const auto st   = analyze (cap, 0, winLen);
            double pk = 0.0; int pkPos = 0;
            for (size_t i = 0; i < cap.size (); ++i)
                if (std::fabs (static_cast<double> (cap[i])) > pk) { pk = std::fabs (static_cast<double> (cap[i])); pkPos = (int) i; }
            std::printf ("  %-11s worstImp=%.4f (%3d) maxDelta=%.4f peak=%.3f@%dms%s\n", fxName (t),
                         st.worstImp, st.impCount, st.maxDelta, pk, pkPos * 1000 / 48000,
                         pk > 1.0 ? "   <== ABOVE 0 dBFS" : "");
        }
    }
    return true;
}

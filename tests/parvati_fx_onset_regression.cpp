// FX note-ONSET + early-sustain crackle regression — covers the region the
// full-engine continuity test deliberately SKIPS (its 0.5 s warmup "note-
// attack + buffer-fill transients") plus the first ~1 s of sustain, through
// the real ParvatiAudioProcessor::processBlock from a FRESH plugin state
// (no warmup flush — exactly "start of playing") with EACH of the 15 FX at
// full wet. Scenarios:
//   1. single note (vel 100) onset, first 250 ms
//   2. 6-note chord (vel 127) onset, first 250 ms — the chain-input ceiling
//      territory where the pre-2026-08-17 LUT-overrun bugs bit hardest
//   3. single note early sustain, 250 ms..1.2 s — catches MID-NOTE crackle
//      (the measured pre-fix Wavefolder LUT-overrun garbage peaked at
//      ~854 ms, OUTSIDE any onset-only window).
//
// Params are PER-FX aware: the generic "everything mid" grid either FREEZES
// the buffer-based FX into a silent empty buffer (Looper/WSOLA freeze =
// param3, Spectral freeze = param4; 64/127 = 0.504 > the 0.5 threshold) or
// LOW-PASSES the output so hard it masks crackle (Wavefolder/WSOLA tone) or
// injects DESIGNED crackle (VinylCompressor's Crackle knob). Freeze params
// go to 0, tone/bright params to bypass, the designed-crackle knob to 0.
//
// VALIDATED regression (2026-08-17, against the pre-fix commit d2669c4 —
// the parent of the 69e678d crackle fixes): on d2669c4 this test FAILS on
// RingModulator in every scenario (0.34-0.98 — the stale Warps SRC history
// pointer) and on VinylCompressor onset (0.18-0.21); on the fixed tree all
// 60 checks pass with the margins documented in boundFor(). The Wavefolder
// LUT-overrun defect is NOT reachable through this engine-level scenario on
// d2669c4 (its sustained levels stay below the |sl|>2.295 trip point) — it
// is pinned directly by the Wavefolder LUT-domain checks in
// parvati_clouds_fx_test (rails-bounded + continuous under deep overrun,
// also validated to fail catastrophically on d2669c4).
//
// Build: part of the parvati_unified_tests binary (unified_test_runner harness).

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
int g_failures = 0;
void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

const char* fxName (int ti)
{
    switch (ti)
    {
        case 0: return "None";       case 1: return "Diffuser";
        case 2: return "PitchShifter"; case 3: return "Reverb";
        case 4: return "LoopingDelay"; case 5: return "WSOLA";
        case 6: return "Spectral";   case 7: return "Wavefolder";
        case 8: return "FreqShifter"; case 9: return "RingMod";
        case 10: return "Resonator"; case 11: return "ClockedDly";
        case 12: return "Ensemble";  case 13: return "PlateRev";
        case 14: return "VinylComp"; case 15: return "Phaser";
        case 16: return "Overdrive"; case 17: return "LUT Dist";
        case 18: return "Comp";      case 19: return "Gate";
        case 20: return "Chorus";    case 21: return "Flanger";
        case 22: return "Echo";      case 23: return "Room";
        case 24: return "Spring";
        default: return "?";
    }
}

// Per-FX param overrides on the "everything 64 (mid)" base grid, so the
// generic grid cannot silence, darken, or deliberately crackle an FX:
//   - Looper/WSOLA param3 (>0.5 = freeze) -> 0 (record normally)
//   - Spectral param4 (>0.5 = freeze)     -> 0
//   - WSOLA param4 (Tone one-pole LP ~2 kHz at 64) -> 127 (bypass — the
//     splice/gain-envelope transients this regression watches are broadband;
//     current code passes either way)
//   - VinylCompressor param2 (Crackle — a DESIGNED impulse generator) -> 0,
//     so its by-design noise cannot mask the bound
//   - Wavefolder gets NO override (all 64): its legit output at tone-bypass
//     is slope-gained attack edges + saturating-fold HF (measured 0.21-0.25
//     on the FIXED tree — not a defect), while the pinned pre-fix LUT-overrun
//     garbage is impulsive and pierces the default 2 kHz tone LP (0.61 pre-fix
//     vs ~0.07 fixed at the same settings) — the all-64 grid is exactly the
//     configuration that separates the two.
// (Param slots are fx1_param1..5 -> setParams param[0..4]; the semantic map
// per FX is in FxProcessors.h / Fv1*.h — double-check indices when editing.)
std::vector<int> paramsFor (int fx)
{
    std::vector<int> p { 64, 64, 64, 64, 64 };
    switch (fx)
    {
        case 4:  p[2] = 0;        break;                 // LoopingDelay: freeze off
        case 5:  p[2] = 0; p[3] = 127; break;            // WSOLA: freeze off, tone bypass
        case 6:  p[3] = 0;        break;                 // Spectral: freeze off
        case 14: p[1] = 0;        break;                 // VinylComp: designed crackle off
        default: break;
    }
    return p;
}

// Per-FX impulse bound. Default 0.10 sits above designed behavior on the
// fixed tree and far below the pinned defect class (RingMod 0.71-0.98,
// Wavefolder LUT-overrun 0.6+ on the pre-fix commit d2669c4). FX getting
// a measured-character bound:
//   - VinylCompressor / Phaser: the FV-1 family's DESIGNED lo-fi signal
//     chain (linear host<->internal resampling, no modern anti-alias filter,
//     Q.23 fixed-point path) floors at 0.112-0.136 at mid params — measured
//     IDENTICAL before/after the 69e678d fixes (VinylComp sustain 0.135
//     pre-fix vs 0.136 fixed), i.e. character, not the pinned defect.
//   - Wavefolder: legit slope-gain of a loud attack edge through the fold
//     (chord onset measured 0.098 on the FIXED tree at default Tone) — a
//     0.10 bound there is a 2% margin flake; the pinned pre-fix garbage
//     (0.61) clears 0.16 by ~4x.
//   - Overdrive (0.16, tightened from 0.20 by the 6x shaper oversampling):
//     the driven tube table squaring the saw's edges is the effect's sound
//     (onset measured 0.126-0.143 post-OS vs 0.176-0.183 before); the
//     pre-OS ALIASED crackle (the folded-harmonic burst that motivated the
//     oversampling) is pinned separately by parvati_fv1_alias_probe numbers
//     in the changelog.
//   - Compressor: the fast-attack gain grab on a note edge is a designed
//     transient (onset ~0.10); bound 0.16 like the lo-fi family floor.
//   - LUT Dist (0.16, 2026-08-19): the drive-calibration fix (>>13 -> >>16,
//     table now read at the true domain xT = D*x) makes a full-velocity note
//     edge genuinely DRIVEN at default params — the same designed driven-edge
//     slope class as Overdrive above (measured 0.129-0.143; was < 0.10 only
//     because the old 8x-hot calibration crushed the input deeper into the
//     table's flat region). The pinned stale-build defect class (0.6+) still
//     clears 0.16 by ~4x.
double boundFor (int fx)
{
    if (fx == 7 || fx == 14 || fx == 15 || fx == 16 || fx == 17 || fx == 18) return 0.16;   // Wavefolder / VinylComp / Phaser / Overdrive / LUT Dist / Compressor
    return 0.10;
}

// Worst curvature-immune impulse over [from, to): |delta| that is BOTH > 8x
// the 93rd-percentile |delta| of the trailing 64-sample window AND > 0.004
// absolute (same detector family as the continuity test, so ordinary note-
// attack slew is not flagged).
double worstImpulse (const std::vector<float>& out, int from, int to)
{
    const int n = static_cast<int> (out.size ());
    std::vector<float> d (static_cast<size_t> (n), 0.0f);
    for (int i = std::max (from, 1); i < std::min (to, n); ++i)
        d[static_cast<size_t> (i)] = std::fabs (out[static_cast<size_t> (i)] - out[static_cast<size_t> (i - 1)]);
    double worst = 0.0;
    for (int i = std::max (from, 1) + 64; i < std::min (to, n); ++i)
    {
        float window[64];
        for (int k = 0; k < 64; ++k) window[k] = d[static_cast<size_t> (i - 64 + k)];
        std::sort (window, window + 64);
        const float base = window[60];
        if (d[static_cast<size_t> (i)] > 8.0f * base && d[static_cast<size_t> (i)] > 0.004f)
            worst = std::max (worst, static_cast<double> (d[static_cast<size_t> (i)]));
    }
    return worst;
}

// Fresh processor -> FX1 = fxType, enabled, full wet, per-FX safe params ->
// chord note-on(s) at t=0 (NO warmup flush) -> render durSec -> analyze
// main-bus L/R over [fromSec, durSec).
double renderWorst (int fxType, double sr, int bufferSize, int chord, int vel,
                    double durSec, double fromSec)
{
    ParvatiAudioProcessor proc;
    proc.prepareToPlay (sr, bufferSize);
    proc.getApvts().getParameterAsValue ("part_select") = 1.0f;
    setInt (proc, "osc1_shape", 1);              // SAW
    setChoice (proc, "fx1_type", fxType);
    setInt (proc, "fx1_enabled", 1);
    setInt (proc, "fx1_drywet", 127);            // full wet
    const auto pv = paramsFor (fxType);
    for (int k = 0; k < 5; ++k)
        setInt (proc, ("fx1_param" + std::to_string (k + 1)).c_str (), pv[static_cast<size_t> (k)]);

    const int total = static_cast<int> (durSec * sr);
    std::vector<float> capL (static_cast<size_t> (total), 0.0f);
    std::vector<float> capR (static_cast<size_t> (total), 0.0f);
    int written = 0;
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
                    static_cast<uint8_t> (vel)), 0);
            noteOn = true;
        }
        proc.processBlock (buf, midi);
        const int n = std::min (bufferSize, total - written);
        const float* pl = buf.getReadPointer (0);
        const float* pr = buf.getReadPointer (1);
        for (int i = 0; i < n; ++i)
        {
            capL[static_cast<size_t> (written + i)] = pl[i];
            capR[static_cast<size_t> (written + i)] = pr[i];
        }
        written += n;
    }
    const int from = static_cast<int> (fromSec * sr);
    return std::max (worstImpulse (capL, from, total),
                     worstImpulse (capR, from, total));
}

// Bound rationale: see boundFor(). The comment documents measured floors.
void runScenario (const char* name, double sr, int bufferSize, int chord, int vel,
                  double durSec, double fromSec)
{
    std::printf ("-- %s @%.0fk buf %d --\n", name, sr / 1000.0, bufferSize);
    for (int t = 1; t < static_cast<int> (FxType::Count); ++t)
    {
        const double worst = renderWorst (t, sr, bufferSize, chord, vel, durSec, fromSec);
        const double bound = boundFor (t);
        char msg[160];
        std::snprintf (msg, sizeof (msg), "%-13s worst impulse=%.4f (must be < %.2f)",
                       fxName (t), worst, bound);
        check (worst < bound, msg);
    }
}
}  // namespace

TEST(parvati_fx_onset_regression)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf ("=== FX note-onset + early-sustain crackle regression (all 24 FX) ===\n");
    std::printf ("Fresh plugin, note-on at t=0, full wet, per-FX freeze/tone/crackle-safe params.\n\n");
    runScenario ("single note onset, vel 100", 48000.0, 256, 1, 100, 0.25, 0.0);
    runScenario ("single note onset, vel 100", 44100.0, 128, 1, 100, 0.25, 0.0);
    runScenario ("6-note chord onset, vel 127", 48000.0, 256, 6, 127, 0.25, 0.0);
    runScenario ("single note sustain 0.25-1.2s, vel 100", 48000.0, 256, 1, 100, 1.2, 0.25);
    std::printf ("\n%s\n", g_failures == 0
        ? "FX note-onset regression PASSED."
        : "FX note-onset regression FAILED.");
    return g_failures == 0;
}

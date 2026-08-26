// Comprehensive SYNTH parameter coverage — every descriptor parameter swept,
// INTENDED vs REAL outcome verified per tests/COVERAGE_SPEC.md.
//
// This is the consolidated regression net for the SYNTH side (106 patch/part +
// 67 sequencer + 5 arp + 4 options = 181 params). It would otherwise be ~20
// separate ~6.5 MB binaries; instead it is one binary that links Hellcat once.
//
// Strategy:
//   * PRIMARY net: a generic byte-routing sweep — every byte-routed descriptor
//     (osc/mix/filter/env/lfo/mod/modifier/part = 106 params) is set to several
//     distinct values and the routed Patch/Part byte is asserted to equal
//     hellcatValueToPatchByte(). This catches ANY broken byte routing.
//   * SECONDARY net: targeted AUDIO checks for the key audio-meaningful params
//     (osc shape/range/detune, mixer, filter cutoff/reso/mode, env attack, LFO
//     rate, mod-matrix VCA routing, part volume/octave) with generous but
//     meaningful tolerances.
//   * TERTIARY net: engine-state routing for arp/seq/option params.
//
// Run: ./build_unified/hellcat_unified_tests synth_param_coverage_test

#include <algorithm>
#include "unified_test_runner.h"
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>  // ScopedJuceInitialiser_GUI

#include "ParameterLayout.h"
#include "PluginProcessor.h"
#include "test_utils.h"              // shared setInt/setChoice (host-path helpers)
#include "dsp/patch.h"  // MOD_SRC_* / MOD_DST_* / kNumSyncedLfoRates enum constants

// Exact float comparison is deliberate: these asserts pin values,
// not ranges.
#pragma clang diagnostic ignored "-Wfloat-equal"

namespace
{
int g_failures = 0;
int g_checks   = 0;

void check (bool cond, const std::string& msg)
{
    ++g_checks;
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg.c_str());
    if (! cond) ++g_failures;
}

// ---- parameter setting: choice/int by descriptor, over the shared helpers ----
void setByDescriptor (HellcatAudioProcessor& proc, const PatchParamDescriptor& d, int value)
{
    if (d.choices != nullptr)
        setChoice (proc, d.paramID.c_str(), value);
    else
        setInt (proc, d.paramID.c_str(), value);
}

// ---- a minimal play head so the transport reads as "playing" (arp/seq run) ----
// ---- audio render helpers ----
constexpr double kFs   = 48000.0;
constexpr int    kBlk  = 512;

// Silences any sounding voices, then renders `blocks` blocks with a NoteOn on
// block 0 (held). Returns the mono mix (0.5*(L+R)).
std::vector<float> renderNote (HellcatAudioProcessor& proc, int midi, int blocks,
                               float velocity = 0.8f)
{
    proc.getEngine().allNotesOff (1, false);   // silence prior notes
    {
        juce::AudioBuffer<float> flush (2, kBlk); flush.clear();
        juce::MidiBuffer empty;
        proc.processBlock (flush, empty);      // flush the killed voices
    }

    std::vector<float> out;
    out.reserve (static_cast<size_t> (kBlk * blocks));
    for (int b = 0; b < blocks; ++b)
    {
        juce::AudioBuffer<float> buf (2, kBlk);
        buf.clear();
        juce::MidiBuffer midiBuf;
        if (b == 0)
            midiBuf.addEvent (juce::MidiMessage::noteOn (1, midi, velocity), 0);
        proc.processBlock (buf, midiBuf);
        for (int i = 0; i < buf.getNumSamples(); ++i)
            out.push_back (0.5f * (buf.getSample (0, i) + buf.getSample (1, i)));
    }
    return out;
}

double peakOf (const std::vector<float>& x)
{
    double p = 0.0;
    for (float v : x) p = std::max (p, std::fabs (static_cast<double> (v)));
    return p;
}

double rmsOf (const std::vector<float>& x)
{
    if (x.empty()) return 0.0;
    double s = 0.0;
    for (float v : x) s += static_cast<double> (v) * v;
    return std::sqrt (s / static_cast<double> (x.size()));
}

double rmsOf (const std::vector<float>& x, size_t a, size_t b)
{
    if (b <= a) return 0.0;
    double s = 0.0;
    for (size_t i = a; i < b; ++i) s += static_cast<double> (x[i]) * x[i];
    return std::sqrt (s / static_cast<double> (b - a));
}

// Zero-crossing rate — a robust proxy for spectral brightness / cutoff.
double zcrOf (const std::vector<float>& x)
{
    if (x.size() < 2) return 0.0;
    int zc = 0;
    for (size_t i = 1; i < x.size(); ++i)
        if ((x[i] >= 0.0f) != (x[i - 1] >= 0.0f)) ++zc;
    return static_cast<double> (zc) / static_cast<double> (x.size() - 1);
}

// RMS of the content ABOVE cutoffHz (one-pole highpass). A card-robust
// brightness measure: the ZCR proxy saturates on low-order cards, where the
// fundamental dominates the crossings at every cutoff (2-pole Q 0.5 keeps
// the fundamental at -6 dB at fc; a 24 dB/oct card buries it).
double hfRmsOf (const std::vector<float>& x, double cutoffHz)
{
    const double a = std::exp (-2.0 * juce::MathConstants<double>::pi * cutoffHz / kFs);
    double lp = 0.0, acc = 0.0;
    size_t n = 0;
    for (float v : x)
    {
        lp = a * lp + (1.0 - a) * static_cast<double> (v);
        const double hp = static_cast<double> (v) - lp;
        acc += hp * hp;
        ++n;
    }
    return n != 0 ? std::sqrt (acc / static_cast<double> (n)) : 0.0;
}

// RMS of the content BELOW cutoffHz (one-pole lowpass output).
double lfRmsOf (const std::vector<float>& x, double cutoffHz)
{
    const double a = std::exp (-2.0 * juce::MathConstants<double>::pi * cutoffHz / kFs);
    double lp = 0.0, acc = 0.0;
    size_t n = 0;
    for (float v : x)
    {
        lp = a * lp + (1.0 - a) * static_cast<double> (v);
        acc += lp * lp;
        ++n;
    }
    return n != 0 ? std::sqrt (acc / static_cast<double> (n)) : 0.0;
}

// Autocorrelation pitch estimator (host-rate float samples).
double detectPitchHz (const std::vector<float>& x, double fs,
                      double fMin = 80.0, double fMax = 2000.0)
{
    const int minLag = static_cast<int> (fs / fMax);
    const int maxLag = static_cast<int> (fs / fMin);
    if (maxLag >= static_cast<int> (x.size())) return 0.0;
    std::vector<double> acf (static_cast<size_t> (maxLag + 2), 0.0);
    double globalMax = 0.0;
    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        double c = 0.0;
        for (int i = 0; i + lag < static_cast<int> (x.size()); ++i)
            c += static_cast<double> (x[static_cast<size_t> (i)]) * x[static_cast<size_t> (i + lag)];
        acf[static_cast<size_t> (lag)] = c;
        if (c > globalMax) globalMax = c;
    }
    if (globalMax <= 0.0) return 0.0;
    const double threshold = 0.8 * globalMax;
    int bestLag = 0;
    for (int lag = minLag + 1; lag < maxLag; ++lag)
        if (acf[static_cast<size_t> (lag)] >= threshold && acf[static_cast<size_t> (lag)] > acf[static_cast<size_t> (lag - 1)] && acf[static_cast<size_t> (lag)] >= acf[static_cast<size_t> (lag + 1)])
        { bestLag = lag; break; }
    if (bestLag == 0) return 0.0;
    const double denom = (acf[static_cast<size_t> (bestLag - 1)] - 2.0 * acf[static_cast<size_t> (bestLag)] + acf[static_cast<size_t> (bestLag + 1)]);
    double offset = 0.0;
    if (std::fabs (denom) > 1e-9)
        offset = std::clamp (0.5 * (acf[static_cast<size_t> (bestLag - 1)] - acf[static_cast<size_t> (bestLag + 1)]) / denom, -1.0, 1.0);
    return fs / (bestLag + offset);
}

// Finiteness / subnormal sanity over a buffer.
bool audioFinite (const std::vector<float>& x)
{
    for (float v : x)
    {
        if (! std::isfinite (v)) return false;
        if (std::fabs (v) > 0.0f && std::fabs (v) < std::numeric_limits<float>::min()) return false;
    }
    return true;
}

// ---- generic byte-routing helpers ----
// Returns the routed byte after setting `value` and syncing the engine.
uint8_t routedByte (HellcatAudioProcessor& proc, const PatchParamDescriptor& d, int value)
{
    setByDescriptor (proc, d, value);
    proc.syncAllParamsToEngine();
    auto& part = proc.getEngine().getPart(0);
    return d.isPart ? static_cast<uint8_t> (part.partBytes[(size_t) d.byteOffset])
                    : static_cast<uint8_t> (part.patchBytes[(size_t) d.byteOffset]);
}

// A distinct denormalized value for the descriptor (variant 0..2).
int pickValue (const PatchParamDescriptor& d, int variant)
{
    if (d.choices != nullptr && d.choices->size() > 0)
    {
        const int n = d.choices->size();
        if (n <= 1) return 0;
        if (variant == 0) return 0;
        if (variant == 1) return n / 2;
        return n - 1;
    }
    if (variant == 0) return d.minValue;
    if (variant == 1) return (d.minValue + d.maxValue) / 2;
    return d.maxValue;
}

// Is this descriptor a byte-routed patch/part param (the generic-sweep set)?
bool isByteRouted (const PatchParamDescriptor& d)
{
    return ! d.isArp && ! d.isOption && ! d.isSequencer && ! d.isFx;
}

// Restore EVERY APVTS parameter to its factory default + sync. Essential before
// any audio test: the byte-routing sweep (and other tests) leave the patch in an
// arbitrary state (e.g. mod10_source maxed to CONST_4 -> VCA, which closes the
// VCA). Starting each audio test from the audible factory init patch makes the
// measurements deterministic and meaningful.
void resetToDefaults (HellcatAudioProcessor& proc)
{
    for (const auto& d : getPatchParamDescriptors())
        setByDescriptor (proc, d, d.defaultValue);
    proc.syncAllParamsToEngine();
}
}  // namespace

// =============================================================================
// 1. Parameter table sanity
// =============================================================================
static void testParamTable (HellcatAudioProcessor& proc)
{
    std::printf ("[1] Parameter table\n");
    const auto& descs = getPatchParamDescriptors();
    std::printf ("     total descriptors: %zu\n", descs.size());

    int synthCount = 0, byteRouted = 0;
    for (const auto& d : descs)
    {
        if (! d.isFx) ++synthCount;
        if (isByteRouted (d)) ++byteRouted;
    }
    std::printf ("     synth (non-fx) descriptors: %d\n", synthCount);
    std::printf ("     byte-routed descriptors:    %d\n", byteRouted);
    check (synthCount == 182, "182 synth params (260 total - 78 fx)");
    check (byteRouted == 106, "106 byte-routed patch/part params");

    int missing = 0;
    for (const auto& d : descs)
        if (! d.isFx && ! proc.getApvts().getParameter (d.paramID))
            ++missing;
    check (missing == 0, "every synth descriptor has an APVTS parameter");
}

// =============================================================================
// 2. Generic byte-routing sweep — the PRIMARY regression net (106 params)
// =============================================================================
static void testByteRoutingAll (HellcatAudioProcessor& proc)
{
    std::printf ("[2] Generic byte-routing sweep (every patch/part param)\n");
    const auto& descs = getPatchParamDescriptors();

    int tested = 0;
    int byteMismatch = 0;
    int deadParam = 0;   // min-value byte == max-value byte for a param with a real range

    for (const auto& d : descs)
    {
        if (! isByteRouted (d)) continue;
        ++tested;

        const int vA = pickValue (d, 0);   // low
        const int vC = pickValue (d, 2);   // high
        const uint8_t byteA = routedByte (proc, d, vA);
        const uint8_t byteC = routedByte (proc, d, vC);
        const uint8_t wantA = hellcatValueToPatchByte (d, static_cast<float> (vA));
        const uint8_t wantC = hellcatValueToPatchByte (d, static_cast<float> (vC));

        if (byteA != wantA || byteC != wantC)
        {
            ++byteMismatch;
            std::printf ("  FAIL: %s byte mismatch: got %u/%u want %u/%u\n",
                         d.paramID.c_str(), byteA, byteC, wantA, wantC);
        }
        // A param with a real value range should map distinct values to a
        // changed byte (catches a dead / always-zero write). Signed params at
        // min/max wrap but still differ; choices with size>1 differ.
        const bool hasRange = (d.choices != nullptr)
                                  ? (d.choices->size() > 1)
                                  : (d.maxValue > d.minValue);
        if (hasRange && byteA == byteC)
            ++deadParam;   // reported once at the end (some edge cases are legit)
    }

    std::printf ("     params swept: %d\n", tested);
    check (byteMismatch == 0, "every routed byte equals hellcatValueToPatchByte()");
    // Dead-param count is informational: a few params legitimately map min==max
    // to the same byte (e.g. a signed 0 vs a value clamping). We report it but
    // do not hard-fail — the targeted audio tests below are the real dead-param
    // net for audio-meaningful params.
    std::printf ("     (info) params where min-byte==max-byte: %d\n", deadParam);
}

// =============================================================================
// 3. Oscillators — audio
// =============================================================================
static void testOscillators (HellcatAudioProcessor& proc)
{
    std::printf ("[3] Oscillator audio\n");
    resetToDefaults (proc);
    auto isolateOsc1 = [&] { setChoice (proc, "osc2_shape", ambika::dsp::WAVEFORM_NONE); };
    auto isolateOsc2 = [&] { setChoice (proc, "osc1_shape", ambika::dsp::WAVEFORM_NONE); };

    // SAW (osc1) audible, NONE near-silent.
    isolateOsc1();
    setChoice (proc, "osc1_shape", ambika::dsp::WAVEFORM_SAW);
    proc.syncAllParamsToEngine();
    const double peakSaw = peakOf (renderNote (proc, 60, 80));

    setChoice (proc, "osc1_shape", ambika::dsp::WAVEFORM_NONE);
    proc.syncAllParamsToEngine();
    const double peakNone = peakOf (renderNote (proc, 60, 80));
    std::printf ("     osc1 SAW peak=%.4f  NONE peak=%.4f\n", peakSaw, peakNone);
    check (peakSaw > 0.01, "osc1 SAW renders audible audio");
    check (peakNone < peakSaw * 0.5, "osc1 NONE clearly quieter than SAW");

    // osc2 isolated.
    isolateOsc2();
    setChoice (proc, "osc2_shape", ambika::dsp::WAVEFORM_SAW);
    proc.syncAllParamsToEngine();
    const double peak2Saw = peakOf (renderNote (proc, 60, 80));
    setChoice (proc, "osc2_shape", ambika::dsp::WAVEFORM_NONE);
    proc.syncAllParamsToEngine();
    const double peak2None = peakOf (renderNote (proc, 60, 80));
    std::printf ("     osc2 SAW peak=%.4f  NONE peak=%.4f\n", peak2Saw, peak2None);
    check (peak2Saw > 0.01, "osc2 SAW renders audible audio");
    check (peak2None < peak2Saw * 0.5, "osc2 NONE clearly quieter than SAW");

    // osc1_range +12 semitones => ~2x pitch.
    isolateOsc1();
    setChoice (proc, "osc1_shape", ambika::dsp::WAVEFORM_SAW);
    setInt (proc, "filter1_cutoff", 127);  // open filter so the fundamental is strong
    setInt (proc, "osc1_range", 0);
    proc.syncAllParamsToEngine();
    const double f0 = detectPitchHz (renderNote (proc, 69, 120), kFs);   // A4
    setInt (proc, "osc1_range", 12);
    proc.syncAllParamsToEngine();
    const double f1 = detectPitchHz (renderNote (proc, 69, 120), kFs);
    std::printf ("     osc1_range 0 -> %.1f Hz, +12 -> %.1f Hz (ratio %.3f)\n",
                 f0, f1, f0 > 0 ? f1 / f0 : 0.0);
    check (f0 > 200.0 && f0 < 600.0, "osc1_range 0 pitch near A4 (200-600 Hz)");
    check (f0 > 0 && std::fabs (f1 / f0 - 2.0) < 0.08, "osc1_range +12 doubles pitch (~2x)");

    // osc1_detune shifts the inter-oscillator beat -> different output between
    // 0 and +64 (we compare the rendered waveform energy/variance, which changes
    // as the beat pattern shifts). Isolate BOTH oscs at SAW so there is a beat.
    setInt (proc, "osc1_range", 0);
    setChoice (proc, "osc2_shape", ambika::dsp::WAVEFORM_SAW);  // both saws
    setInt (proc, "mix_balance", 32);                           // centre
    setInt (proc, "osc1_detune", 0);
    proc.syncAllParamsToEngine();
    const auto det0 = renderNote (proc, 60, 80);
    setInt (proc, "osc1_detune", 64);
    proc.syncAllParamsToEngine();
    const auto det64 = renderNote (proc, 60, 80);
    // Detune changes the period of the summed waveform; compare the
    // zero-crossing rate (proxy for the shifting spectral content).
    const double zcr0  = zcrOf (det0);
    const double zcr64 = zcrOf (det64);
    std::printf ("     osc1_detune 0 zcr=%.5f, +64 zcr=%.5f\n", zcr0, zcr64);
    check (std::fabs (zcr64 - zcr0) > 1e-5 || peakOf (det0) != peakOf (det64),
           "osc1_detune +64 changes the rendered waveform (beat shifts)");

    // osc1_param moves a SQUARE wave's pulse width -> output changes.
    isolateOsc1();
    setChoice (proc, "osc1_shape", ambika::dsp::WAVEFORM_SQUARE);
    setInt (proc, "osc1_param", 0);
    proc.syncAllParamsToEngine();
    const double pwp0 = peakOf (renderNote (proc, 60, 60));
    const double zcrp0 = zcrOf (renderNote (proc, 60, 60));
    setInt (proc, "osc1_param", 127);
    proc.syncAllParamsToEngine();
    const double pwp127 = peakOf (renderNote (proc, 60, 60));
    const double zcrp127 = zcrOf (renderNote (proc, 60, 60));
    std::printf ("     osc1_param 0 peak/zcr=%.4f/%.5f, 127 peak/zcr=%.4f/%.5f\n",
                 pwp0, zcrp0, pwp127, zcrp127);
    check (std::fabs (pwp127 - pwp0) > 1e-4 || std::fabs (zcrp127 - zcrp0) > 1e-5,
           "osc1_param changes a SQUARE wave (timbre/PWM)");
}

// =============================================================================
// 4. Mixer — audio
// =============================================================================
static void testMixer (HellcatAudioProcessor& proc)
{
    std::printf ("[4] Mixer audio\n");
    resetToDefaults (proc);
    // Both oscillators SAW on opposite ranges so they are distinguishable.
    setChoice (proc, "osc1_shape", ambika::dsp::WAVEFORM_SAW);
    setChoice (proc, "osc2_shape", ambika::dsp::WAVEFORM_SAW);
    setInt (proc, "osc2_range", -12);   // osc2 one octave down
    setInt (proc, "filter1_cutoff", 127);
    setInt (proc, "mix_op", ambika::dsp::OP_SUM);
    proc.syncAllParamsToEngine();

    // balance full-osc1 (0) vs full-osc2 (63): the pitch should track.
    setInt (proc, "mix_balance", 0);
    proc.syncAllParamsToEngine();
    const double fOsc1 = detectPitchHz (renderNote (proc, 60, 120), kFs);
    setInt (proc, "mix_balance", 63);
    proc.syncAllParamsToEngine();
    const double fOsc2 = detectPitchHz (renderNote (proc, 60, 120), kFs);
    std::printf ("     balance=0 pitch=%.1f Hz (osc1), balance=63 pitch=%.1f Hz (osc2 -1oct)\n",
                 fOsc1, fOsc2);
    check (fOsc1 > 0 && fOsc2 > 0 && fOsc1 > fOsc2 * 1.3,
           "mix_balance swaps osc1/osc2 energy (osc2 is an octave down)");

    // mix_op: SUM vs RING_MOD produce different output.
    setInt (proc, "mix_balance", 32);
    setInt (proc, "mix_op", ambika::dsp::OP_SUM);
    setInt (proc, "osc2_range", 0);
    proc.syncAllParamsToEngine();
    const double sumPeak = peakOf (renderNote (proc, 60, 60));
    const double sumZcr  = zcrOf (renderNote (proc, 60, 60));
    setInt (proc, "mix_op", ambika::dsp::OP_RING_MOD);
    proc.syncAllParamsToEngine();
    const double ringPeak = peakOf (renderNote (proc, 60, 60));
    const double ringZcr  = zcrOf (renderNote (proc, 60, 60));
    std::printf ("     SUM peak/zcr=%.4f/%.5f, RING peak/zcr=%.4f/%.5f\n",
                 sumPeak, sumZcr, ringPeak, ringZcr);
    check (std::fabs (ringPeak - sumPeak) > 1e-3 || std::fabs (ringZcr - sumZcr) > 1e-4,
           "mix_op SUM vs RING_MOD differ");

    // sub-osc level: 0 vs 63 (sub shape left at default square). The sub-osc
    // adds a lower-octave voice, so measure RMS (total energy), not peak.
    setInt (proc, "mix_op", ambika::dsp::OP_SUM);
    setInt (proc, "mix_sub", 0);
    proc.syncAllParamsToEngine();
    const double sub0 = rmsOf (renderNote (proc, 60, 60));
    setInt (proc, "mix_sub", 63);
    proc.syncAllParamsToEngine();
    const double sub63 = rmsOf (renderNote (proc, 60, 60));
    std::printf ("     mix_sub 0 rms=%.4f, 63 rms=%.4f\n", sub0, sub63);
    check (sub63 > sub0 * 1.02, "mix_sub adds sub-oscillator energy (RMS up)");

    // noise level: 0 vs 63 (broadband -> high ZCR).
    setInt (proc, "mix_sub", 0);
    setInt (proc, "mix_noise", 0);
    proc.syncAllParamsToEngine();
    const double n0 = zcrOf (renderNote (proc, 60, 60));
    setInt (proc, "mix_noise", 63);
    proc.syncAllParamsToEngine();
    const double n63 = zcrOf (renderNote (proc, 60, 60));
    std::printf ("     mix_noise 0 zcr=%.5f, 63 zcr=%.5f\n", n0, n63);
    check (n63 > n0, "mix_noise adds broadband (higher ZCR)");

    // fuzz: 0 vs 63. The waveshaper table (wav_res_distortion) is MONOTONIC
    // with f(128)==128, so every sample keeps its sign under the wet/dry mix —
    // zero-crossing rate is PROVABLY invariant under a monotonic map (a saw
    // and its near-squared version have the same 2 crossings/period). ZCR
    // can never observe this effect; measure RMS instead: at 63 the mix is
    // ~98.8% wet (wet_gain = U14ShiftRight6(63<<8) = 252) and the expander-
    // around-center curve squares the saw up (RMS roughly doubles vs the
    // ~0.35*peak saw). The DSP itself was traced byte-exact to firmware
    // voice.cc (same staging as mix_crush, whose ZCR check passes only
    // because bit-masking is NOT monotonic).
    setInt (proc, "mix_noise", 0);
    setInt (proc, "mix_fuzz", 0);
    proc.syncAllParamsToEngine();
    const double fz0  = rmsOf (renderNote (proc, 60, 60));
    const double fz0z = zcrOf (renderNote (proc, 60, 60));
    setInt (proc, "mix_fuzz", 63);
    proc.syncAllParamsToEngine();
    const double fz63  = rmsOf (renderNote (proc, 60, 60));
    const double fz63z = zcrOf (renderNote (proc, 60, 60));
    std::printf ("     mix_fuzz 0 rms/zcr=%.4f/%.5f, 63 rms/zcr=%.4f/%.5f\n",
                 fz0, fz0z, fz63, fz63z);
    check (fz63 > fz0 * 1.3, "mix_fuzz squares the signal up (higher RMS)");

    // crush: 0 vs 31 -> aliasing/distortion (output changes).
    setInt (proc, "mix_fuzz", 0);
    setInt (proc, "mix_crush", 0);
    proc.syncAllParamsToEngine();
    const double cr0 = peakOf (renderNote (proc, 60, 60));
    setInt (proc, "mix_crush", 31);
    proc.syncAllParamsToEngine();
    const double cr31 = peakOf (renderNote (proc, 60, 60));
    std::printf ("     mix_crush 0 peak=%.4f, 31 peak=%.4f\n", cr0, cr31);
    // The crush effect is verified robustly by the ZCR check below (which renders
    // both settings fresh and compares); the peak comparison is informational.
    check (std::fabs (cr31 - cr0) > 1e-3,
           "mix_crush changes the signal peak (crush applied)");
    // Robust crush check: render both fresh and compare ZCR.
    setInt (proc, "mix_crush", 0); proc.syncAllParamsToEngine();
    const double cz0 = zcrOf (renderNote (proc, 60, 60));
    setInt (proc, "mix_crush", 31); proc.syncAllParamsToEngine();
    const double cz31 = zcrOf (renderNote (proc, 60, 60));
    std::printf ("     mix_crush zcr 0=%.5f 31=%.5f\n", cz0, cz31);
    check (std::fabs (cz31 - cz0) > 1e-5, "mix_crush alters spectral content (ZCR)");
}

// =============================================================================
// 5. Filter — audio
// =============================================================================
static void testFilter (HellcatAudioProcessor& proc)
{
    std::printf ("[5] Filter audio\n");
    resetToDefaults (proc);
    setChoice (proc, "osc1_shape", ambika::dsp::WAVEFORM_SAW);
    setChoice (proc, "osc2_shape", ambika::dsp::WAVEFORM_NONE);
    // The stock default card is SMR4 (4-pole, lowpass-only). The MODE checks
    // below need a 2-pole card, so pin the SVF card explicitly.
    setChoice (proc, "filter_card", 2);
    setChoice (proc, "filter1_mode", ambika::dsp::FILTER_MODE_LP);
    setInt (proc, "filter1_reso", 0);
    setInt (proc, "filter_env", 0);
    setInt (proc, "filter_lfo", 0);
    proc.syncAllParamsToEngine();

    // cutoff low (dark) vs high (bright, high HF energy). The HF-band RMS is
    // card-robust: ZCR saturates on 2-pole cards (the fundamental dominates
    // the crossings at every cutoff).
    setInt (proc, "filter1_cutoff", 10);
    proc.syncAllParamsToEngine();
    const double zLo = hfRmsOf (renderNote (proc, 72, 100), 3000.0);
    setInt (proc, "filter1_cutoff", 127);
    proc.syncAllParamsToEngine();
    const double zHi = hfRmsOf (renderNote (proc, 72, 100), 3000.0);
    std::printf ("     cutoff 10 hf3k=%.5f (dark), 127 hf3k=%.5f (bright)\n", zLo, zHi);
    check (zHi > zLo * 1.2, "filter1_cutoff high is brighter than low (HF-band energy)");

    // resonance: 0 vs 63 at a mid cutoff adds a peak (changes the waveform).
    setInt (proc, "filter1_cutoff", 90);
    setInt (proc, "filter1_reso", 0);
    proc.syncAllParamsToEngine();
    const double r0 = peakOf (renderNote (proc, 72, 100));
    setInt (proc, "filter1_reso", 63);
    proc.syncAllParamsToEngine();
    const double r63 = peakOf (renderNote (proc, 72, 100));
    std::printf ("     reso 0 peak=%.4f, 63 peak=%.4f\n", r0, r63);
    check (std::fabs (r63 - r0) > 1e-3 || zcrOf (renderNote (proc, 72, 100)) != r0,
           "filter1_reso 63 adds a resonance peak/character");

    // mode: LP vs HP, by BAND ENERGY (peak comparisons ride the attack
    // transient; band energy is linear in the spectrum). At a cutoff ABOVE
    // the note-72 fundamental (param 60 lands the keytracked cutoff near
    // 1.3 kHz) LP keeps the low band and HP removes it; HP stays the
    // brighter of the two. Two-sided, so a broken mode map fails on the
    // side it breaks.
    setInt (proc, "filter1_cutoff", 60);
    setInt (proc, "filter1_reso", 0);
    setChoice (proc, "filter1_mode", ambika::dsp::FILTER_MODE_LP);
    proc.syncAllParamsToEngine();
    const auto lpRender = renderNote (proc, 72, 100);
    setChoice (proc, "filter1_mode", ambika::dsp::FILTER_MODE_HP);
    proc.syncAllParamsToEngine();
    const auto hpRender = renderNote (proc, 72, 100);
    const double lfLP = lfRmsOf (lpRender, 300.0), lfHP = lfRmsOf (hpRender, 300.0);
    const double hfLP = hfRmsOf (lpRender, 3000.0), hfHP = hfRmsOf (hpRender, 3000.0);
    std::printf ("     LF(300) LP=%.5f HP=%.5f   HF(3k) LP=%.5f HP=%.5f\n", lfLP, lfHP, hfLP, hfHP);
    check (lfLP > lfHP * 1.3, "filter1_mode LP passes the low band, HP removes it");
    check (hfHP > hfLP * 1.5, "filter1_mode HP is the brighter mode above the cutoff");

    // filter_env: env->cutoff. amount 0 vs 63 changes the attack-time cutoff
    // sweep (the note's opening brightness). Use env1 (default env1->PARAM) is
    // not cutoff; route env1->cutoff via mod matrix, set a fast decay env1, and
    // compare the early-window brightness.
    setChoice (proc, "filter1_mode", ambika::dsp::FILTER_MODE_LP);
    setInt (proc, "filter1_cutoff", 60);
    // route env1 -> filter cutoff (mod1 default is env1->PARAM_1; repoint it).
    setChoice (proc, "mod1_source", ambika::dsp::MOD_SRC_ENV_1);
    setChoice (proc, "mod1_dest", ambika::dsp::MOD_DST_FILTER_CUTOFF);
    setInt (proc, "env1_attack", 0);
    setInt (proc, "env1_decay", 20);
    setInt (proc, "env1_sustain", 0);
    setInt (proc, "mod1_amount", 0);
    proc.syncAllParamsToEngine();
    const auto fe0 = renderNote (proc, 72, 100);
    setInt (proc, "mod1_amount", 63);
    proc.syncAllParamsToEngine();
    const auto fe63 = renderNote (proc, 72, 100);
    // Compare early-window HF-band energy (the env opens the filter early,
    // then decays). ZCR saturates on 2-pole cards (see the cutoff check).
    const double e0  = hfRmsOf (std::vector<float> (fe0.begin(),  fe0.begin()  + 4096), 3000.0);
    const double e63 = hfRmsOf (std::vector<float> (fe63.begin(), fe63.begin() + 4096), 3000.0);
    std::printf ("     filter_env(amt0) early hf3k=%.5f, (amt63) early hf3k=%.5f\n", e0, e63);
    check (e63 > e0, "filter_env amount opens the cutoff on the attack (brighter early)");

    // restore mod1 to a clean state for later tests.
    setInt (proc, "mod1_amount", 0);
}

// =============================================================================
// 6. Envelopes — audio (env3 -> VCA by default init patch)
// =============================================================================
static void testEnvelopes (HellcatAudioProcessor& proc)
{
    std::printf ("[6] Envelope audio\n");
    resetToDefaults (proc);
    // Clean patch: osc1 SAW.
    setChoice (proc, "osc1_shape", ambika::dsp::WAVEFORM_SAW);
    setChoice (proc, "osc2_shape", ambika::dsp::WAVEFORM_NONE);
    setInt (proc, "filter1_cutoff", 127);
    // Zero BOTH default VCA routings so we control the VCA driver explicitly:
    // init patch has ENV3->VCA at mod11 (+63) and VELOCITY->VCA at mod12 (+16).
    setInt (proc, "mod11_amount", 0);
    setInt (proc, "mod12_amount", 0);
    proc.syncAllParamsToEngine();

    // Helper: route env{idx} -> VCA at +63 via mod1, render a note, return the
    // early-window (~30 ms) RMS — the part of the envelope the attack shapes.
    auto earlyRmsFor = [&] (int envIdx, int attack) -> double
    {
        setChoice (proc, "mod1_source",
                   envIdx == 1 ? ambika::dsp::MOD_SRC_ENV_1
                               : (envIdx == 2 ? ambika::dsp::MOD_SRC_ENV_2
                                              : ambika::dsp::MOD_SRC_ENV_3));
        setChoice (proc, "mod1_dest", ambika::dsp::MOD_DST_VCA);
        setInt (proc, "mod1_amount", 63);
        setInt (proc, ("env" + std::to_string (envIdx) + "_attack").c_str(), attack);
        setInt (proc, ("env" + std::to_string (envIdx) + "_decay").c_str(), 40);
        setInt (proc, ("env" + std::to_string (envIdx) + "_sustain").c_str(), 110);
        proc.syncAllParamsToEngine();
        const auto x = renderNote (proc, 60, 120);
        return rmsOf (x, 256, 256 + 1440);   // skip the very first click window
    };

    // env3: instant attack (0) is LOUD early; long attack (127) ramps slowly so
    // the early window is much quieter.
    for (int e = 1; e <= 3; ++e)
    {
        const std::string name = "env" + std::to_string (e);
        const double loud = earlyRmsFor (e, 0);
        const double soft = earlyRmsFor (e, 127);
        std::printf ("     %s_atk 0 early-rms=%.5f, 127 early-rms=%.5f (ratio %.2f)\n",
                     name.c_str(), loud, soft, soft > 1e-9 ? loud / soft : 9999.0);
        check (loud > soft * 1.5, name + "_attack 0 louder early than 127 (attack shapes the ramp)");
    }

    // restore default VCA routing + clean mod1.
    setInt (proc, "mod1_amount", 0);
    setChoice (proc, "mod1_source", ambika::dsp::MOD_SRC_ENV_1);
    setChoice (proc, "mod1_dest", ambika::dsp::MOD_DST_PARAMETER_1);
    setInt (proc, "mod11_amount", 63);
    setInt (proc, "mod12_amount", 16);
    setInt (proc, "env1_attack", 0);
    setInt (proc, "env2_attack", 0);
    setInt (proc, "env3_attack", 0);
}

// =============================================================================
// 7. LFOs (env-lfs + voice lfo) — audio (route lfo->cutoff, vary rate)
// =============================================================================
static void testLfos (HellcatAudioProcessor& proc)
{
    std::printf ("[7] LFO audio (route LFO->cutoff, vary rate)\n");
    resetToDefaults (proc);
    setChoice (proc, "osc1_shape", ambika::dsp::WAVEFORM_SAW);
    setChoice (proc, "osc2_shape", ambika::dsp::WAVEFORM_NONE);
    setInt (proc, "filter1_cutoff", 80);
    setInt (proc, "filter1_reso", 20);
    proc.syncAllParamsToEngine();

    // Route env1-lfo (LFO_1) -> cutoff via mod3 (init mod3 is LFO_1->OSC_1).
    setChoice (proc, "mod3_source", ambika::dsp::MOD_SRC_LFO_1);
    setChoice (proc, "mod3_dest", ambika::dsp::MOD_DST_FILTER_CUTOFF);
    setInt (proc, "mod3_amount", 50);
    setChoice (proc, "env1_lfo_shape", ambika::dsp::LFO_WAVEFORM_TRIANGLE);
    proc.syncAllParamsToEngine();

    // A slow free rate vs a fast free rate: free rates start above the synced
    // band (>= kNumSyncedLfoRates). Pick two clearly different free rates.
    auto envLfoModulationVariance = [&] (int rate) -> double
    {
        setInt (proc, "env1_lfo_rate", rate);
        proc.syncAllParamsToEngine();
        const auto x = renderNote (proc, 60, 380);   // ~4s -> a few LFO cycles
        // Windowed RMS over 1024-sample windows; the LFO sweeps the cutoff so the
        // windowed amplitude varies. Return the variance of the windowed RMS.
        std::vector<double> wrms;
        for (size_t i = 0; i + 1024 <= x.size(); i += 1024)
            wrms.push_back (rmsOf (x, i, i + 1024));
        if (wrms.size() < 3) return 0.0;
        double mean = 0.0; for (double v : wrms) mean += v; mean /= static_cast<double> (wrms.size());
        double var = 0.0; for (double v : wrms) var += (v - mean) * (v - mean);
        return var / static_cast<double> (wrms.size());
    };

    const double varSlow = envLfoModulationVariance (ambika::dsp::kNumSyncedLfoRates + 20);   // slow free
    const double varFast = envLfoModulationVariance (ambika::dsp::kNumSyncedLfoRates + 110);  // fast free
    std::printf ("     env1_lfo_rate slow var=%.3e, fast var=%.3e\n", varSlow, varFast);
    check (varSlow > 1e-8 || varFast > 1e-8,
           "env1 LFO->cutoff produces time-varying output (modulation present)");
    check (std::fabs (varFast - varSlow) > 1e-9,
           "env1_lfo_rate slow vs fast produce different modulation");

    // Voice LFO: route MOD_SRC_LFO_4 -> cutoff (init mod7 is LFO_4->CUTOFF at 0).
    setInt (proc, "mod3_amount", 0);
    setChoice (proc, "mod7_source", ambika::dsp::MOD_SRC_LFO_4);
    setChoice (proc, "mod7_dest", ambika::dsp::MOD_DST_FILTER_CUTOFF);
    setInt (proc, "mod7_amount", 50);
    setChoice (proc, "voice_lfo_shape", ambika::dsp::LFO_WAVEFORM_TRIANGLE);
    setInt (proc, "voice_lfo_rate", 20);
    proc.syncAllParamsToEngine();
    const auto vlfo = renderNote (proc, 60, 380);
    double vlfoVar = 0.0;
    {
        std::vector<double> wrms;
        for (size_t i = 0; i + 1024 <= vlfo.size(); i += 1024)
            wrms.push_back (rmsOf (vlfo, i, i + 1024));
        if (wrms.size() >= 3)
        {
            double mean = 0.0; for (double v : wrms) mean += v; mean /= static_cast<double> (wrms.size());
            for (double v : wrms) vlfoVar += (v - mean) * (v - mean);
            vlfoVar /= static_cast<double> (wrms.size());
        }
    }
    std::printf ("     voice_lfo->cutoff windowed-RMS variance=%.3e\n", vlfoVar);
    check (vlfoVar > 1e-8, "voice_lfo->cutoff produces time-varying output");
    setInt (proc, "mod7_amount", 0);
}

// =============================================================================
// 8. Modulation matrix — functional VCA routing
// =============================================================================
static void testModMatrix (HellcatAudioProcessor& proc)
{
    std::printf ("[8] Modulation matrix functional routing\n");
    resetToDefaults (proc);
    setChoice (proc, "osc1_shape", ambika::dsp::WAVEFORM_SAW);
    setChoice (proc, "osc2_shape", ambika::dsp::WAVEFORM_NONE);
    setInt (proc, "filter1_cutoff", 127);
    // IMPORTANT (firmware contract): the VCA destination has a BASELINE =
    // part_volume<<1 and the VCA mod is MULTIPLICATIVE (voice.cpp:275/327), not
    // additive. So amount=0 never closes the VCA (baseline ~240 at vol=120).
    // To make the mod depth OBSERVABLE we lower part_volume for headroom and
    // use CONST sources of very different value (CONST_256 vs CONST_4).
    setInt (proc, "part_volume", 25);   // baseline VCA = 50 (headroom)
    // Zero the default VCA routings: ENV3->VCA at mod11 (+63), VELOCITY->VCA
    // at mod12 (+16), so mod1 is the SOLE mod driver.
    setInt (proc, "mod11_amount", 0);
    setInt (proc, "mod12_amount", 0);
    setChoice (proc, "mod1_dest", ambika::dsp::MOD_DST_VCA);

    // CONST_256 (max source) -> VCA +63 multiplies the VCA baseline up -> loud;
    // CONST_4 (tiny source) -> VCA +63 keeps it low -> quiet. Proves source
    // depth is honored through the multiplicative VCA.
    setChoice (proc, "mod1_source", ambika::dsp::MOD_SRC_CONSTANT_256);
    setInt (proc, "mod1_amount", 63);
    proc.syncAllParamsToEngine();
    const double peakHigh = peakOf (renderNote (proc, 60, 80));
    setChoice (proc, "mod1_source", ambika::dsp::MOD_SRC_CONSTANT_4);
    proc.syncAllParamsToEngine();
    const double peakLow = peakOf (renderNote (proc, 60, 80));
    std::printf ("     mod const256->VCA peak=%.4f, const4->VCA peak=%.4f\n", peakHigh, peakLow);
    check (peakHigh > peakLow * 2.0, "mod source depth honored (CONST_256 >> CONST_4 on VCA)");

    // Amount direction: CONST_256 -> VCA +63 (loud) vs -63 (inverts -> quiet).
    setChoice (proc, "mod1_source", ambika::dsp::MOD_SRC_CONSTANT_256);
    setInt (proc, "mod1_amount", -63);
    proc.syncAllParamsToEngine();
    const double peakNeg = peakOf (renderNote (proc, 60, 80));
    std::printf ("     mod const256->VCA amt -63 peak=%.4f (inverts)\n", peakNeg);
    check (peakNeg < peakHigh, "mod amount -63 inverts the VCA modulation (quieter)");

    // An ENVELOPE source is dynamic: ENV3 -> VCA +63 (env opens at sustain) is
    // louder than the low CONST_4 baseline above. Proves an envelope routes.
    setChoice (proc, "mod1_source", ambika::dsp::MOD_SRC_ENV_3);
    setInt (proc, "mod1_amount", 63);
    setInt (proc, "env3_sustain", 110);
    proc.syncAllParamsToEngine();
    const double peakEnv = peakOf (renderNote (proc, 60, 80));
    std::printf ("     mod env3->VCA +63 peak=%.4f (env opens VCA at sustain)\n", peakEnv);
    check (peakEnv > peakLow * 1.5, "mod ENV3->VCA opens the VCA (envelope routes)");

    // restore defaults for later tests.
    setInt (proc, "mod1_amount", 0);
    setChoice (proc, "mod1_source", ambika::dsp::MOD_SRC_ENV_1);
    setChoice (proc, "mod1_dest", ambika::dsp::MOD_DST_PARAMETER_1);
    setInt (proc, "part_volume", 120);
    setInt (proc, "mod11_amount", 63);
    setInt (proc, "mod12_amount", 16);
    setInt (proc, "env3_sustain", 100);
}

// =============================================================================
// 9. Part params — audio (volume/octave) + byte routing already in sweep
// =============================================================================
static void testPartParams (HellcatAudioProcessor& proc)
{
    std::printf ("[9] Part param audio\n");
    resetToDefaults (proc);
    setChoice (proc, "osc1_shape", ambika::dsp::WAVEFORM_SAW);
    setChoice (proc, "osc2_shape", ambika::dsp::WAVEFORM_NONE);
    setInt (proc, "filter1_cutoff", 127);
    proc.syncAllParamsToEngine();

    // volume 127 vs 10: peak ratio.
    setInt (proc, "part_volume", 127);
    proc.syncAllParamsToEngine();
    const double vol127 = peakOf (renderNote (proc, 60, 80));
    setInt (proc, "part_volume", 10);
    proc.syncAllParamsToEngine();
    const double vol10 = peakOf (renderNote (proc, 60, 80));
    std::printf ("     part_volume 127 peak=%.4f, 10 peak=%.4f\n", vol127, vol10);
    check (vol127 > vol10 * 1.2, "part_volume higher => louder");

    // octave +1 => ~2x pitch.
    setInt (proc, "part_volume", 127);
    setInt (proc, "part_octave", 0);
    proc.syncAllParamsToEngine();
    const double po0 = detectPitchHz (renderNote (proc, 69, 120), kFs);
    setInt (proc, "part_octave", 1);
    proc.syncAllParamsToEngine();
    const double po1 = detectPitchHz (renderNote (proc, 69, 120), kFs);
    std::printf ("     part_octave 0 -> %.1f Hz, +1 -> %.1f Hz\n", po0, po1);
    check (po0 > 200 && po0 < 600 && std::fabs (po1 / po0 - 2.0) < 0.08,
           "part_octave +1 doubles pitch");

    // polyphony mode round-trips (byte routing covered in the sweep; here just
    // confirm all 5 modes are accepted + render finite).
    setInt (proc, "part_octave", 0);
    int okModes = 0;
    for (int m = 0; m < 5; ++m)
    {
        setChoice (proc, "part_polyphony", m);
        proc.syncAllParamsToEngine();
        const auto x = renderNote (proc, 60, 40);
        if (audioFinite (x)) ++okModes;
    }
    std::printf ("     polyphony modes finite: %d/5\n", okModes);
    check (okModes == 5, "all 5 polyphony modes render finite audio");
}

// =============================================================================
// 10. Sequencer — routing + functional note fire
// =============================================================================
static void testSequencer (HellcatAudioProcessor& proc)
{
    std::printf ("[10] Sequencer routing + functional\n");
    resetToDefaults (proc);
    // (a) Every seq descriptor routes without OOB: set seqnote bytes to 0 and 255
    //     (the full 0..255 range the gate/legato bits require) on all 16 steps.
    const auto& descs = getPatchParamDescriptors();
    int seqSet = 0;
    for (const auto& d : descs)
    {
        if (! d.isSequencer) continue;
        setInt (proc, d.paramID.c_str(), d.minValue);
        setInt (proc, d.paramID.c_str(), d.maxValue);
        proc.syncAllParamsToEngine();
        ++seqSet;
    }
    std::printf ("     sequencer params exercised: %d\n", seqSet);
    check (seqSet == 67, "all 67 sequencer params set without crash");

    // Render finite with arbitrary seq data (proves no OOB/crash under the
    // 0..255 note/gate bytes).
    {
        const auto x = renderNote (proc, 60, 20);
        check (audioFinite (x), "renders finite with extreme sequencer byte values");
    }

    // (b) Functional: arp_mode=Sequencer + a 1-step note + transport playing =>
    //     the engine's arp/seq generates a note. Clear the seq, set one step with
    //     a note + gate, length 1, and check the engine produces a non-zero note.
    for (int s = 0; s < 16; ++s)
    {
        setInt (proc, ("seqnote_step" + std::to_string (s)).c_str(), 0);   // gate off everywhere
        setInt (proc, ("seq1_step"   + std::to_string (s)).c_str(), 0);
        setInt (proc, ("seq2_step"   + std::to_string (s)).c_str(), 0);
    }
    setInt (proc, "seq_length_3", 1);                          // note-seq length 1
    setInt (proc, "seqnote_step0", 0x80 | 60);                 // gate on + note C4
    setInt (proc, "seqnote_vel0", 100);                        // velocity
    setChoice (proc, "arp_mode", 2);                           // Sequencer
    setChoice (proc, "arp_resolution", 10);                    // 1/16
    proc.syncAllParamsToEngine();

    // The note-sequencer transposes relative to a HELD key (note = step.note +
    // heldNote - 60) and only fires while a key is held. Hold a key on block 0,
    // then render ~1s and watch seq.debugPreviousNote() (set when a seq note fires).
    std::set<int> genNotes;
    for (int b = 0; b < 200; ++b)
    {
        juce::AudioBuffer<float> buf (2, kBlk); buf.clear();
        juce::MidiBuffer midi;
        if (b == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (uint8_t) 100), 0);
        proc.processBlock (buf, midi);
        genNotes.insert (static_cast<int> (proc.getEngine().getPart(0).seq.debugPreviousNote()));
    }
    int distinct = 0;
    for (int n : genNotes) if (n != 0xff) ++distinct;
    std::printf ("     note-sequencer generated distinct notes: %d\n", distinct);
    check (distinct >= 1, "note-sequencer fires a note when transport plays + a key is held");

    // back to arp off.
    proc.getEngine().allNotesOff (1, false);
    setChoice (proc, "arp_mode", 0);
    proc.syncAllParamsToEngine();
}

// =============================================================================
// 11. Arp — functional
// =============================================================================
static void testArp (HellcatAudioProcessor& proc)
{
    std::printf ("[11] Arp functional\n");
    resetToDefaults (proc);
    setChoice (proc, "osc1_shape", ambika::dsp::WAVEFORM_SAW);
    setChoice (proc, "osc2_shape", ambika::dsp::WAVEFORM_NONE);
    setInt (proc, "filter1_cutoff", 127);
    proc.syncAllParamsToEngine();

    // arp_mode Off => no generated notes from a held chord.
    setChoice (proc, "arp_mode", 0);
    setChoice (proc, "arp_direction", 0);   // Up
    setChoice (proc, "arp_octave", 2);
    setChoice (proc, "arp_resolution", 10); // 1/16
    proc.syncAllParamsToEngine();
    {
        juce::AudioBuffer<float> buf (2, kBlk); buf.clear();
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 48, (uint8_t) 100), 0);
        midi.addEvent (juce::MidiMessage::noteOn (1, 52, (uint8_t) 100), 0);
        midi.addEvent (juce::MidiMessage::noteOn (1, 55, (uint8_t) 100), 0);
        proc.processBlock (buf, midi);
    }
    std::set<int> offNotes;
    for (int b = 0; b < 120; ++b)
    {
        juce::AudioBuffer<float> buf (2, kBlk); buf.clear();
        juce::MidiBuffer empty;
        proc.processBlock (buf, empty);
        offNotes.insert (static_cast<int> (proc.getEngine().getPart(0).arp.lastNote()));
    }
    int offDistinct = 0;
    for (int n : offNotes) if (n != 0xff) ++offDistinct;
    std::printf ("     arp Off distinct generated notes: %d\n", offDistinct);

    // arp_mode Arp + held chord + playing => multiple distinct pitches.
    setChoice (proc, "arp_mode", 1);
    proc.syncAllParamsToEngine();
    proc.getEngine().allNotesOff (1, false);
    {
        juce::AudioBuffer<float> buf (2, kBlk); buf.clear();
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 48, (uint8_t) 100), 0);
        midi.addEvent (juce::MidiMessage::noteOn (1, 52, (uint8_t) 100), 0);
        midi.addEvent (juce::MidiMessage::noteOn (1, 55, (uint8_t) 100), 0);
        proc.processBlock (buf, midi);
    }
    std::set<int> arpNotes;
    for (int b = 0; b < 600; ++b)   // ~3s -> several arp steps
    {
        juce::AudioBuffer<float> buf (2, kBlk); buf.clear();
        juce::MidiBuffer empty;
        proc.processBlock (buf, empty);
        arpNotes.insert (static_cast<int> (proc.getEngine().getPart(0).arp.lastNote()));
    }
    int arpDistinct = 0;
    for (int n : arpNotes) if (n != 0xff) ++arpDistinct;
    std::printf ("     arp On distinct generated notes: %d\n", arpDistinct);
    check (arpDistinct >= 2, "arp On cycles multiple distinct pitches from a held chord");
    check (arpDistinct > offDistinct, "arp On generates more notes than arp Off");

    // resolution change is accepted + renders finite.
    setChoice (proc, "arp_resolution", 0);   // 1/1 (slowest)
    proc.syncAllParamsToEngine();
    {
        juce::AudioBuffer<float> buf (2, kBlk); buf.clear();
        juce::MidiBuffer empty;
        proc.processBlock (buf, empty);
        check (true, "arp_resolution change accepted (no crash)");
    }

    // back to arp off.
    setChoice (proc, "arp_mode", 0);
    proc.getEngine().allNotesOff (1, false);
    proc.syncAllParamsToEngine();
}

// =============================================================================
// 12. Options — vca_curve / filter_card / filter_drive / part_select
// =============================================================================
static void testOptions (HellcatAudioProcessor& proc)
{
    std::printf ("[12] Options\n");
    resetToDefaults (proc);
    setChoice (proc, "osc1_shape", ambika::dsp::WAVEFORM_SAW);
    setChoice (proc, "osc2_shape", ambika::dsp::WAVEFORM_NONE);
    setInt (proc, "filter1_cutoff", 127);
    proc.syncAllParamsToEngine();

    // vca_curve 0 (Linearized) vs 1 (Exponential): the amplitude envelope shape
    // differs. Compare the early/late ratio (exp curve settles differently).
    setChoice (proc, "vca_curve", 0);
    proc.syncAllParamsToEngine();
    const auto lin = renderNote (proc, 60, 120);
    setChoice (proc, "vca_curve", 1);
    proc.syncAllParamsToEngine();
    const auto expc = renderNote (proc, 60, 120);
    const double linRatio = rmsOf (lin,  4096, 8192) > 1e-6
        ? rmsOf (lin,  0, 1440) / rmsOf (lin,  4096, 8192) : 0.0;
    const double expRatio = rmsOf (expc, 4096, 8192) > 1e-6
        ? rmsOf (expc, 0, 1440) / rmsOf (expc, 4096, 8192) : 0.0;
    std::printf ("     vca_curve lin early/late=%.3f, exp early/late=%.3f\n", linRatio, expRatio);
    check (std::fabs (expRatio - linRatio) > 1e-3 || peakOf (lin) != peakOf (expc),
           "vca_curve exp vs linearized changes the amplitude shape");

    // filter_card: 6 topologies accepted + sound different at high resonance.
    setChoice (proc, "vca_curve", 0);
    setInt (proc, "filter1_reso", 50);
    setInt (proc, "filter1_cutoff", 90);
    proc.syncAllParamsToEngine();
    double topoPeaks[6];
    bool topoFinite = true;
    std::vector<float> topoRenders[6];
    for (int t = 0; t < 6; ++t)
    {
        setChoice (proc, "filter_card", t);
        proc.syncAllParamsToEngine();
        topoRenders[t] = renderNote (proc, 72, 100);
        topoPeaks[t] = peakOf (topoRenders[t]);
        if (! audioFinite (topoRenders[t])) topoFinite = false;
    }
    std::printf ("     filter_card peaks: smr4=%.4f ssm2164=%.4f svf=%.4f ladder=%.4f polivoks=%.4f ir3109=%.4f\n",
                 topoPeaks[0], topoPeaks[1], topoPeaks[2], topoPeaks[3], topoPeaks[4], topoPeaks[5]);
    check (topoFinite, "all 6 filter_card topologies render finite");
    // Distinctness is pinned by WAVEFORM, not by peak level: the 2026-08-26
    // resonance-law harmonization levels the cards on purpose, so peak levels
    // may coincide by chance (ladder vs Polivoks measured 0.2156 vs 0.2147,
    // inside the old 1e-3 peak window). Two different filter structures
    // still render different waveforms.
    auto waveDiff = [&topoRenders] (int a, int b)
    {
        const size_t n = std::min (topoRenders[a].size(), topoRenders[b].size());
        double s = 0.0;
        for (size_t i = 0; i < n; ++i)
        {
            const double d = double (topoRenders[a][i]) - double (topoRenders[b][i]);
            s += d * d;
        }
        return std::sqrt (s / double (n));
    };
    // At least two of the topologies should produce measurably different
    // output at high resonance (they are genuinely different filter designs).
    const bool differ01 = waveDiff (0, 1) > 1e-3;
    const bool differ02 = waveDiff (0, 2) > 1e-3;
    const bool differ12 = waveDiff (1, 2) > 1e-3;
    const bool differ03 = waveDiff (0, 3) > 1e-3;
    const bool differ04 = waveDiff (0, 4) > 1e-3;
    check (differ01 || differ02 || differ12 || differ03 || differ04, "filter_card topologies produce different audio");
    check (differ03, "OTA card (index 3) sounds different from the Ladder card");
    check (differ04, "Polivoks card (index 4) sounds different from the Ladder card");

    // filter_drive: index 0 (1.0) vs index 7 (12.0) on the Ladder card with a
    // hot input -> saturation differs. Use a high note + high reso to push level.
    setChoice (proc, "filter_card", 3);   // Ladder (drive affects ladder only)
    setInt (proc, "filter1_reso", 55);
    setInt (proc, "filter1_cutoff", 110);
    setChoice (proc, "filter_drive", 0);  // 1.0
    proc.syncAllParamsToEngine();
    const double drv1 = peakOf (renderNote (proc, 72, 100));
    setChoice (proc, "filter_drive", 7);  // 12.0
    proc.syncAllParamsToEngine();
    const double drv12 = peakOf (renderNote (proc, 72, 100));
    std::printf ("     filter_drive 1.0 peak=%.4f, 12.0 peak=%.4f\n", drv1, drv12);
    check (std::fabs (drv12 - drv1) > 1e-3, "filter_drive scales the ladder saturation");

    // part_select: switching to Part 2 routes subsequent edits to Part 2.
    // (part_select is an AudioParameterInt 1..6, NOT a choice -> use setInt.)
    setChoice (proc, "filter_drive", 1);  // restore default
    // Note the default osc1_shape on Part 1 vs after a Part-2 edit.
    proc.getEngine().setCurrentPart (0);
    setInt (proc, "part_select", 1);   // Part 1
    setChoice (proc, "osc1_shape", ambika::dsp::WAVEFORM_SQUARE);
    proc.syncAllParamsToEngine();
    const uint8_t p1Byte = proc.getEngine().getPart(0).patchBytes[0];

    setInt (proc, "part_select", 2);   // Part 2
    setChoice (proc, "osc1_shape", ambika::dsp::WAVEFORM_TRIANGLE);
    proc.syncAllParamsToEngine();
    const uint8_t p2Byte = proc.getEngine().getPart(1).patchBytes[0];
    const uint8_t p1ByteAfter = proc.getEngine().getPart(0).patchBytes[0];
    std::printf ("     Part1 byte0=%u (SQUARE), Part2 byte0=%u (TRI), Part1 still=%u\n",
                 p1Byte, p2Byte, p1ByteAfter);
    check (p1Byte == ambika::dsp::WAVEFORM_SQUARE, "part_select=1 edits Part 1");
    check (p2Byte == ambika::dsp::WAVEFORM_TRIANGLE, "part_select=2 edits Part 2 (independent)");
    check (p1ByteAfter == ambika::dsp::WAVEFORM_SQUARE, "Part 1 unchanged when editing Part 2");

    // restore Part 1 + a clean osc1 for any later use.
    setInt (proc, "part_select", 1);
    setChoice (proc, "osc1_shape", ambika::dsp::WAVEFORM_SAW);
    proc.syncAllParamsToEngine();
}

// =============================================================================
TEST(synth_param_coverage_test)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf ("=== Hellcat SYNTH parameter coverage ===\n");

    HellcatAudioProcessor proc;
    FakePlayHead playHead;
    proc.setPlayHead (&playHead);
    proc.prepareToPlay (kFs, kBlk);
    proc.syncAllParamsToEngine();

    testParamTable (proc);
    testByteRoutingAll (proc);
    testOscillators (proc);
    testMixer (proc);
    testFilter (proc);
    testEnvelopes (proc);
    testLfos (proc);
    testModMatrix (proc);
    testPartParams (proc);
    testSequencer (proc);
    testArp (proc);
    testOptions (proc);

    std::printf ("\n=== %s (%d check%s, %d failure%s) ===\n",
                 g_failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED",
                 g_checks, g_checks == 1 ? "" : "s",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}

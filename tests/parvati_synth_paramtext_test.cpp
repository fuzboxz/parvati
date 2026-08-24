// User-friendly synth readout formatter (SynthParamLabels::paramValueTextSynth).
//
// Pure-function input->output checks (no GUI): every raw-numeric synth knob
// family gets a human readout (semitones / cents / % / Hz / ms / note names /
// divisions / octaves), and the fall-through returns the raw integer. The Hz /
// env-time outputs depend on the firmware LUTs, so those cases assert
// "finite + has-unit" rather than an exact float.
//
// Run: ./build_unified/parvati_unified_tests parvati_synth_paramtext_test

#include <cstdio>
#include "unified_test_runner.h"

#include <juce_core/juce_core.h>

#include "ui/SynthParamLabels.h"

namespace
{
int g_failures = 0;

void checkEqual (const juce::String& got, const juce::String& want, const char* label)
{
    const bool ok = got == want;
    std::printf ("  %s: %s -> \"%s\"%s\n",
                 ok ? "ok  " : "FAIL", label, got.toRawUTF8(),
                 ok ? "" : (juce::String (" (want \"") + want + "\")").toRawUTF8());
    if (! ok) ++g_failures;
}

void checkContains (const juce::String& got, const juce::String& sub, const char* label)
{
    const bool ok = got.contains (sub) && ! got.isEmpty();
    std::printf ("  %s: %s -> \"%s\"%s\n",
                 ok ? "ok  " : "FAIL", label, got.toRawUTF8(),
                 ok ? "" : (juce::String (" (must contain \"") + sub + "\")").toRawUTF8());
    if (! ok) ++g_failures;
}

// Wraps the formatter (value is the denormalized natural unit).
juce::String T (const char* id, double value)
{
    return paramValueTextSynth (juce::String (id), value);
}
}  // namespace

static void testOscillators()
{
    std::printf ("(oscillators)\n");
    checkEqual (T ("osc1_range", 12.0),  "+12st", "osc1_range 12");
    checkEqual (T ("osc1_range", -3.0),  "-3st",  "osc1_range -3");
    checkEqual (T ("osc1_range", 0.0),   "0st",   "osc1_range 0");
    checkEqual (T ("osc1_detune", 64.0), "+50ct", "osc1_detune 64");
    checkEqual (T ("osc1_detune", -64.0),"-50ct", "osc1_detune -64");
    checkEqual (T ("osc1_param", 64.0),  "50%",    "osc1_param 64");
}

static void testMixer()
{
    std::printf ("(mixer)\n");
    checkEqual (T ("mix_balance", 32.0), "Ctr",  "mix_balance 32 (centre)");
    checkEqual (T ("mix_balance", 0.0),  "L100", "mix_balance 0 (full L, % elided)");
    checkEqual (T ("mix_balance", 63.0), "R100", "mix_balance 63 (full R, symmetric, % elided)");
    checkEqual (T ("mix_crush", 0.0),    "Off",    "mix_crush 0");
    checkEqual (T ("mix_fuzz", 63.0),    "100%",   "mix_fuzz 63");
}

static void testFilter()
{
    std::printf ("(filter)\n");
    checkContains (T ("filter1_cutoff", 64.0), "Hz",    "filter1_cutoff 64 (finite Hz)");
    checkContains (T ("filter1_cutoff", 0.0),  "Hz",    "filter1_cutoff 0 (min Hz)");
    checkEqual     (T ("filter1_cutoff", 127.0),"15k6", "filter1_cutoff 127 (top -> kHz NkN notation: 15.6kHz -> 15k6)");
    checkEqual (T ("filter1_reso", 63.0), "100%", "filter1_reso 63");
}

static void testEnvelopes()
{
    std::printf ("(envelopes)\n");
    const juce::String atk = T ("env1_attack", 40.0);
    checkContains (atk, "ms", "env1_attack 40 (finite ms)");   // mid value -> ms range
    checkEqual (T ("env1_sustain", 95.0), "75%", "env1_sustain 95");
}

static void testLfoRates()
{
    std::printf ("(lfo rates)\n");
    checkEqual     (T ("env1_lfo_rate", 10.0), "1/16", "env1_lfo_rate 10 (synced)");
    checkContains  (T ("env1_lfo_rate", 39.0), "Hz",   "env1_lfo_rate 39 (free)");
    checkContains  (T ("voice_lfo_rate", 72.0), "Hz",  "voice_lfo_rate 72 (free)");
}

static void testModSeqArpPart()
{
    std::printf ("(mod / seq / arp / part)\n");
    checkEqual (T ("mod1_amount", 32.0), "+51%", "mod1_amount 32");
    checkEqual (T ("mod1_amount", -32.0),"-51%", "mod1_amount -32");
    checkEqual (T ("seqnote_step0", static_cast<double> (0x80 | 60)), "C4", "seqnote_step0 0x80|60 (gate+note)");
    checkEqual (T ("seqnote_step0", 60.0),
                juce::String (juce::CharPointer_UTF8 ("\xE2\x80\x94")),
                "seqnote_step0 60 (gate off -> em dash)");
    // seqnote_vel: velocity | legato bit. 100/127 = 78.74 -> 79% (L = legato).
    checkEqual (T ("seqnote_vel3", static_cast<double> (0x80 | 100)), "79%L",
                "seqnote_vel3 0x80|100 (velocity + legato bit)");
    checkEqual (T ("seqnote_vel3", 100.0), "79%",
                "seqnote_vel3 100 (velocity only, no legato)");
    checkEqual (T ("seqnote_vel7", static_cast<double> (0x80 | 0)), "0%L",
                "seqnote_vel7 0x80|0 (legato with zero velocity)");
    checkEqual (T ("arp_octave", 2.0),   "2oct",  "arp_octave 2");
    checkEqual (T ("part_octave", -1.0), "-1oct", "part_octave -1");
    checkEqual (T ("part_tuning", 64.0), "+50ct", "part_tuning 64 (1/128-st units -> 50 ct)");
    checkEqual (T ("part_tuning", 127.0), "+99ct", "part_tuning 127 (max -> 99 ct)");
    checkEqual (T ("part_tuning", -127.0), "-99ct", "part_tuning -127 (min -> -99 ct)");
    // part_raga is a choice param (gated out of the runtime display — the
    // combo shows its choice text); the formatter entry is the unit-checkable
    // decode of the raga byte (index 0 = 12-EDO, 1..32 = firmware preset).
    checkEqual (T ("part_raga", 0.0),  "12-EDO",      "part_raga 0 (off)");
    checkEqual (T ("part_raga", 1.0),  "Just",        "part_raga 1 (just)");
    checkEqual (T ("part_raga", 2.0),  "Pythagorean", "part_raga 2 (pythagorean)");
    checkEqual (T ("seq_length_1", 16.0),"16", "seq_length_1 16");
}

static void testUntestedFamilies()
{
    std::printf ("(filter depths / mix_crush / seq steps / part scales / env2-3 dispatch)\n");
    // filter_env / filter_lfo ride the 0..63 depth fall-through of the filter
    // branch (not the cutoff/reso special cases).
    checkEqual (T ("filter_env", 63.0), "100%", "filter_env 63 (max depth)");
    checkEqual (T ("filter_lfo", 63.0), "100%", "filter_lfo 63 (max depth)");
    checkEqual (T ("filter_env", 0.0), "0%", "filter_env 0");
    // mix_crush non-zero: the 0..31 decimator scale (0 alone reads Off).
    checkEqual (T ("mix_crush", 31.0), "100%", "mix_crush 31 (full, /31 scale)");
    checkEqual (T ("mix_crush", 16.0), "52%", "mix_crush 16 (mid, /31 scale)");
    // seq1/2_step modulation values use the 0..127 scale.
    checkEqual (T ("seq1_step5", 127.0), "100%", "seq1_step5 127 (max)");
    checkEqual (T ("seq2_step0", 64.0), "50%", "seq2_step0 64 (mid)");
    // Part params with per-parameter denominators: spread /40, portamento /63,
    // volume /127 — each at its own 100% rail.
    checkEqual (T ("part_spread", 40.0), "100%", "part_spread 40 (max, /40)");
    checkEqual (T ("part_portamento", 63.0), "100%", "part_portamento 63 (max, /63)");
    checkEqual (T ("part_volume", 127.0), "100%", "part_volume 127 (max, /127)");
    // env2_/env3_ prefix dispatch hits the same env branch as env1_: the
    // synced/free LFO-rate boundary (14 = last synced division, 15 = first
    // free-running rate) must hold for EVERY env unit.
    checkEqual (T ("env3_lfo_rate", 14.0), "1/64T", "env3_lfo_rate 14 (last synced division)");
    checkContains (T ("env3_lfo_rate", 15.0), "Hz", "env3_lfo_rate 15 (first free rate)");
    checkEqual (T ("env2_lfo_rate", 0.0), "1/1", "env2_lfo_rate 0 (first synced division)");
    checkEqual (T ("env2_sustain", 127.0), "100%", "env2_sustain 127 (max)");
}

static void testFallback()
{
    std::printf ("(fallback)\n");
    // An unknown/choice-ish id that slips through (defensive) -> raw integer.
    checkEqual (T ("something_unknown", 42.0), "42", "unknown id -> raw int");
}

TEST(parvati_synth_paramtext_test)
{
    std::printf ("=== synth param readout formatter ===\n\n");
    testOscillators();
    testMixer();
    testFilter();
    testEnvelopes();
    testLfoRates();
    testModSeqArpPart();
    testUntestedFamilies();
    testFallback();

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures == 0 ? "ALL PASS" : "FAILURES",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}

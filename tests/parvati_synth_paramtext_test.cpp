// User-friendly synth readout formatter (SynthParamLabels::paramValueTextSynth).
//
// Pure-function input->output checks (no GUI): every raw-numeric synth knob
// family gets a human readout (semitones / cents / % / Hz / ms / note names /
// divisions / octaves), and the fall-through returns the raw integer. The Hz /
// env-time outputs depend on the firmware LUTs, so those cases assert
// "finite + has-unit" rather than an exact float.
//
// Built by default. Run with: ./build/parvati_synth_paramtext_test

#include <cstdio>

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
    checkEqual (T ("osc1_range", 12.0),  "+12 st", "osc1_range 12");
    checkEqual (T ("osc1_range", -3.0),  "-3 st",  "osc1_range -3");
    checkEqual (T ("osc1_range", 0.0),   "0 st",   "osc1_range 0");
    checkEqual (T ("osc1_detune", 64.0), "+50 ct", "osc1_detune 64");
    checkEqual (T ("osc1_detune", -64.0),"-50 ct", "osc1_detune -64");
    checkEqual (T ("osc1_param", 64.0),  "50%",    "osc1_param 64");
}

static void testMixer()
{
    std::printf ("(mixer)\n");
    checkEqual (T ("mix_balance", 32.0), "Ctr",    "mix_balance 32 (centre)");
    checkEqual (T ("mix_balance", 0.0),  "L 100%", "mix_balance 0 (full L)");
    checkEqual (T ("mix_crush", 0.0),    "Off",    "mix_crush 0");
    checkEqual (T ("mix_fuzz", 63.0),    "100%",   "mix_fuzz 63");
}

static void testFilter()
{
    std::printf ("(filter)\n");
    checkContains (T ("filter1_cutoff", 64.0), "Hz", "filter1_cutoff 64 (finite Hz)");
    checkContains (T ("filter1_cutoff", 0.0),  "Hz", "filter1_cutoff 0 (min Hz)");
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
    checkEqual (T ("arp_octave", 2.0),   "2 oct",  "arp_octave 2");
    checkEqual (T ("part_octave", -1.0), "-1 oct", "part_octave -1");
    checkEqual (T ("part_tuning", 64.0), "+50 ct", "part_tuning 64 (1/128-st units -> 50 ct)");
    checkEqual (T ("part_tuning", 127.0), "+99 ct", "part_tuning 127 (max -> 99 ct)");
    checkEqual (T ("part_tuning", -127.0), "-99 ct", "part_tuning -127 (min -> -99 ct)");
    checkEqual (T ("seq_length_1", 16.0),"16 steps", "seq_length_1 16");
}

static void testFallback()
{
    std::printf ("(fallback)\n");
    // An unknown/choice-ish id that slips through (defensive) -> raw integer.
    checkEqual (T ("something_unknown", 42.0), "42", "unknown id -> raw int");
}

int main()
{
    std::printf ("=== synth param readout formatter ===\n\n");
    testOscillators();
    testMixer();
    testFilter();
    testEnvelopes();
    testLfoRates();
    testModSeqArpPart();
    testFallback();

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures == 0 ? "ALL PASS" : "FAILURES",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}

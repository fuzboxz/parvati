// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See SynthParamLabels.h.
//
// Meaningful-unit value readout for the SYNTH-section knob params. Mirrors the
// FX formatter (ui/FxSlotLabels.cpp::paramValueText) for the synth side. The
// dispatch is by paramID prefix; every raw-numeric synth knob gets a human
// readout (Hz / ms / semitones / cents / % / note names / divisions / octaves),
// and any unmatched paramID falls back to the raw integer (the pre-formatter
// behaviour). DISPLAY-ONLY — never touches the stored value.

#include "ui/SynthParamLabels.h"

#include <cmath>

#include "ui/NoteName.h"

#include "dsp/analog_filter.h"            // AnalogFilter::cutoffByteToHz
#include "dsp/constants.h"                // kInternalSampleRate, kAudioBlockSize
#include "dsp/patch.h"                    // kNumSyncedLfoRates
#include "dsp/resources/resources.h"      // lut_res_*_increments
#include "dsp/resources/resources_manager.h"  // ResourcesManager::Lookup

namespace
{
// Synced-LFO rate (0..14) -> musical division, derived from
// midi_clock_tick_per_step (constants.h:59) = cycle length in 24-PPQN ticks.
// Each base note has up to three variants: normal, dotted (x3/4 ticks i.e.
// x1.5 duration), triplet (x2/3 ticks). Sorted by tick descending:
//   96=1/1  72=1/2.  64=1/1T  48=1/2  36=1/4.  32=1/2T  24=1/4  16=1/4T
//   12=1/8   8=1/8T   6=1/16   4=1/16T  3=1/32   2=1/32T  1=1/64T
// NOTE: this is the CORRECT tick-indexed order; ParameterLayout's
// makeArpResolutions() (the arp-resolution dropdown labels) is in a DIFFERENT
// order and is partly mislabeled vs the ticks. Reusing it would show index 10
// (tick 6 = 1/16) as "1/16T". Do NOT reuse makeArpResolutions here.
const char* const kSyncedDivisions[15] = {
    "1/1", "1/2.", "1/1T", "1/2", "1/4.", "1/2T", "1/4", "1/4T",
    "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T", "1/64T" };

// Frequency -> "0.XXHz" / "NHz" / "N.NkHz" (Style X: no space; kHz 1 sig-fig
// so the longest readout "1.2kHz" fits a 36px dial above the painter's 9px floor).
juce::String hzToString (double hz)
{
    if (hz < 1.0)    return juce::String (hz, 2) + "Hz";
    if (hz < 1000.0) return juce::String (juce::roundToInt (hz)) + "Hz";
    return juce::String (hz / 1000.0, 1) + "kHz";
}

// LFO 16-bit phase advanced once per internal block (kAudioBlockSize samples @
// kInternalSampleRate) -> Hz = inc * sr / (block * 65536).
double lfoIncrementToHz (uint16_t inc)
{
    constexpr double kSr    = ambika::dsp::kInternalSampleRate;                          // 39216
    constexpr double kBlock = static_cast<double> (ambika::dsp::kAudioBlockSize);        // 40
    return static_cast<double> (inc) * kSr / (kBlock * 65536.0);
}

// Env/portamento 16-bit phase advanced once per internal block -> time(s) =
// (65536 * block) / (inc * sr). inc==0 (never advances) -> infinity glyph.
juce::String envTimeToString (uint16_t inc)
{
    if (inc == 0) return juce::String::charToString ((juce::juce_wchar) 0x221E);   // ∞
    constexpr double kSr    = ambika::dsp::kInternalSampleRate;
    constexpr double kBlock = static_cast<double> (ambika::dsp::kAudioBlockSize);
    const double t = (65536.0 * kBlock) / (static_cast<double> (inc) * kSr);
    if (t < 0.001) return "<1ms";
    if (t < 1.0)   return juce::String (juce::roundToInt (t * 1000.0)) + "ms";
    return juce::String (t, 1) + "s";
}

// Signed amount -63..+63 -> "+100%" / "0%" / "-50%" (mirrors ModMatrixView::formatPercent).
juce::String amountPercent (double v)
{
    const int pct = juce::roundToInt (v * 100.0 / 63.0);
    return (pct > 0 ? "+" : juce::String()) + juce::String (pct) + "%";
}

// Unsigned 0..max -> "NN%".
juce::String pct (double v, double max)
{
    return juce::String (juce::roundToInt (juce::jlimit (0.0, 100.0, v / max * 100.0))) + "%";
}

juce::String formatSemis (int semis)
{
    return (semis > 0 ? "+" : juce::String()) + juce::String (semis) + "st";
}
}  // namespace

juce::String paramValueTextSynth (const juce::String& id, double value)
{
    using ambika::dsp::ResourcesManager;
    using ambika::dsp::lut_res_env_portamento_increments;
    using ambika::dsp::lut_res_lfo_increments;
    const int iv = juce::roundToInt (value);

    // ---- Oscillators ----
    if (id.startsWith ("osc"))
    {
        if (id.endsWith ("_range"))  return formatSemis (iv);                 // ±24 st
        if (id.endsWith ("_detune"))                                            // ±64 -> cents
        {
            // display fallback; verify against DSP (detune byte is 1/128-st units)
            const int ct = juce::roundToInt (value * 100.0 / 128.0);
            return (ct > 0 ? "+" : juce::String()) + juce::String (ct) + "ct";
        }
        return pct (value, 127.0);   // osc_param (shape-dependent) -> %
    }

    // ---- Mixer ----
    if (id.startsWith ("mix"))
    {
        if (id == "mix_balance")
        {
            // Range 0..63, centre 32 (init). Per-side denominator by EACH side's
            // own max distance (L spans 0..31 -> max dist 32; R spans 33..63 ->
            // max dist 31) so both rails read 100%: L0->"L100%", R63->"R100%".
            if (std::abs (iv - 32) <= 1) return "Ctr";
            const int dist  = std::abs (iv - 32);
            const int denom = (iv < 32) ? 32 : 31;
            const int p = juce::roundToInt (juce::jlimit (0.0, 100.0, dist / (double) denom * 100.0));
            return (iv < 32 ? "L" : "R") + juce::String (p) + "%";
        }
        if (id == "mix_crush")   // sample-rate decimator, not bit-depth -> %
            return iv == 0 ? "Off" : pct (value, 31.0);   // display fallback; verify
        // mix_param / mix_sub / mix_noise / mix_fuzz (0..63)
        return pct (value, 63.0);
    }

    // ---- Filter ----
    if (id.startsWith ("filter"))
    {
        if (id == "filter1_cutoff")
        {
            // Knob 0..127 -> cutoff byte 0..254 -> Hz. Approximate (the real cutoff
            // is key-tracked + mod-matrix shifted); this is the BASE cutoff position.
            const int byte = juce::jlimit (0, 255, juce::roundToInt (value * 2.0));
            return hzToString (ambika::dsp::AnalogFilter::cutoffByteToHz (static_cast<uint8_t> (byte)));
        }
        // filter1_reso / filter_env / filter_lfo are 0..63 depth amounts -> %.
        // (filter_drive is a choice param, gated out before the formatter runs.)
        return pct (value, 63.0);
    }

    // ---- Envelopes + per-env LFO ----
    if (id.startsWith ("env") && id.length() >= 5)
    {
        const bool isLfoRate = id.endsWith ("_lfo_rate");
        if (isLfoRate)
        {
            // 0..(kNumSyncedLfoRates+127): <15 synced, >=15 free-running.
            if (iv < ambika::dsp::kNumSyncedLfoRates)
                return kSyncedDivisions[iv];
            const uint16_t inc = ResourcesManager::Lookup<uint16_t, uint8_t> (
                lut_res_lfo_increments, static_cast<uint8_t> (iv - ambika::dsp::kNumSyncedLfoRates));
            return hzToString (lfoIncrementToHz (inc));
        }
        if (id.endsWith ("_sustain")) return pct (value, 127.0);
        if (id.endsWith ("_attack") || id.endsWith ("_decay") || id.endsWith ("_release"))
        {
            const uint16_t inc = ResourcesManager::Lookup<uint16_t, uint8_t> (
                lut_res_env_portamento_increments, static_cast<uint8_t> (iv));
            return envTimeToString (inc);
        }
        // env_lfo_shape (choice) is gated out; anything else -> raw.
        return juce::String (iv);
    }

    // ---- Voice LFO (MOD_SRC_LFO_4) ----
    if (id == "voice_lfo_rate")
    {
        const uint16_t inc = ResourcesManager::Lookup<uint16_t, uint8_t> (
            lut_res_lfo_increments, static_cast<uint8_t> (iv));
        return hzToString (lfoIncrementToHz (inc));
    }

    // ---- Mod matrix amount (mod{N}_amount; source/dest are choices -> gated) ----
    if (id.startsWith ("mod") && id.endsWith ("_amount"))
        return amountPercent (value);

    // ---- Sequencer ----
    if (id.startsWith ("seq"))
    {
        // 28px SEQ dial can't fit "16 steps"; the "Length" label disambiguates.
        if (id.startsWith ("seq_length_")) return juce::String (iv);
        if (id.startsWith ("seqnote_step"))                                   // note | gate
        {
            const bool gate = (iv & 0x80) != 0;
            if (! gate) return juce::CharPointer_UTF8 ("\xE2\x80\x94");      // em dash —
            return midiNoteName (iv & 0x7f);
        }
        if (id.startsWith ("seqnote_vel"))                                    // vel | legato
        {
            const int vel = iv & 0x7f;
            const bool leg = (iv & 0x80) != 0;
            return pct (static_cast<double> (vel), 127.0) + (leg ? "L" : "");
        }
        // seq1/2_step (modulation value, 0..127) -> %
        return pct (value, 127.0);
    }

    // ---- Arpeggiator (arp_octave; mode/dir/pattern/resolution are choices) ----
    if (id == "arp_octave") return juce::String (iv) + "oct";

    // ---- Global / Part ----
    if (id.startsWith ("part"))
    {
        if (id == "part_octave")
            return (iv > 0 ? "+" : juce::String()) + juce::String (iv) + "oct";   // -2..2 octaves
        if (id == "part_tuning")
        {
            // DSP adds tuning to the 14-bit pitch in 1/128-semitone units
            // (AmbikaVoice startNote: note14 = n*128 + partTuning_), i.e. the
            // same unit as osc detune -> cents = value * 100/128 (range +-99 ct).
            const int ct = juce::roundToInt (value * 100.0 / 128.0);
            return (ct > 0 ? "+" : juce::String()) + juce::String (ct) + "ct";
        }
        if (id == "part_spread")   return pct (value, 40.0);
        if (id == "part_portamento") return pct (value, 63.0);   // display fallback; verify (rate, not time)
        if (id == "part_volume")   return pct (value, 127.0);    // display fallback; verify (dB)
        return juce::String (iv);
    }

    // ---- Default: raw integer (pre-formatter behaviour, future-proof) ----
    return juce::String (iv);
}

// Copyright (c) 2026 805Labs Kft. / Hellcat.  See ParamHelp.h.

#include "ParamHelp.h"

namespace
{
// Build the curated entries. The {n} macros from the plan are expanded
// here at authoring time (env1_..env3_, mod1_..mod14_, modif1_..mod4_) so the
// map holds literal concrete keys that match getPatchParamDescriptors(). The
// FX families (fx{1..3}_*, fxmod{1..16}_*) are generated with the SAME loops
// the descriptor table uses (ParameterLayout's addFx), so the map can never
// drift from those 78 paramIDs.
std::unordered_map<std::string, std::string> buildHelpMap()
{
    std::unordered_map<std::string, std::string> m;

    // ---- Oscillators (8) ----
    m["osc1_shape"]  = "Waveform for oscillator 1 (saw, square, CZ, wavetable, FM...).";
    m["osc1_param"]  = "Shape-specific parameter (pulse width, FM ratio, wavetable position...).";
    m["osc1_range"]  = "Osc 1 octave range in semitones (-24..+24).";
    m["osc1_detune"] = "Osc 1 fine detune (-64..+64).";
    m["osc2_shape"]  = "Waveform for oscillator 2.";
    m["osc2_param"]  = "Shape-specific parameter for oscillator 2.";
    m["osc2_range"]  = "Osc 2 octave range in semitones (-24..+24).";
    m["osc2_detune"] = "Osc 2 fine detune (-64..+64).";

    // ---- Mixer (8) ----
    m["mix_balance"]   = "Balance between oscillator 1 and oscillator 2 (0..63).";
    m["mix_op"]        = "Oscillator mix operation (Sum, Sync, Ring Mod, XOR, Fold, Bits).";
    m["mix_param"]     = "Depth of the selected mix operation (0..63).";
    m["mix_sub_shape"] = "Sub-oscillator waveform.";
    m["mix_sub"]       = "Sub-oscillator level (one octave below the root).";
    m["mix_noise"]     = "White-noise level added to the mix (0..63).";
    m["mix_fuzz"]      = "Wavefolding / fuzz distortion amount (0..63).";
    m["mix_crush"]     = "Bit-crusher resolution (0..31; lower = more crushed).";

    // ---- Filters (8) ----
    m["filter1_cutoff"] = "Filter 1 cutoff frequency (0..127).";
    m["filter1_reso"]   = "Filter 1 resonance / Q (0..63).";
    m["filter1_mode"]   = "Filter 1 response: LP, BP, HP, Notch.";
    m["filter2_cutoff"] = "Filter 2 cutoff frequency (0..127).";
    m["filter2_reso"]   = "Filter 2 resonance / Q (0..63).";
    m["filter2_mode"]   = "Filter 2 response: LP, BP, HP, Notch.";
    m["filter_env"]     = "Envelope 1 amount applied to filter cutoff (0..63).";
    m["filter_lfo"]     = "LFO 2 amount applied to filter cutoff (0..63).";

    // ---- 3 env+lfo units (21 = 7 each) ----
    // Env/LFO 1
    m["env1_attack"]    = "Env 1 attack time (0..127).";
    m["env1_decay"]     = "Env 1 decay time (0..127).";
    m["env1_sustain"]   = "Env 1 sustain level (0..127).";
    m["env1_release"]   = "Env 1 release time (0..127).";
    m["env1_lfo_shape"] = "LFO 1 waveform: Triangle, Square, S&H, Ramp.";
    m["env1_lfo_rate"]  = "LFO 1 rate (0..142; <15 = tempo-synced).";
    m["env1_lfo_sync"]  = "LFO 1 sync mode: Free, Slave, Master.";
    // Env/LFO 2
    m["env2_attack"]    = "Env 2 attack time (0..127).";
    m["env2_decay"]     = "Env 2 decay time (0..127).";
    m["env2_sustain"]   = "Env 2 sustain level (0..127).";
    m["env2_release"]   = "Env 2 release time (0..127).";
    m["env2_lfo_shape"] = "LFO 2 waveform: Triangle, Square, S&H, Ramp.";
    m["env2_lfo_rate"]  = "LFO 2 rate (0..142; <15 = tempo-synced).";
    m["env2_lfo_sync"]  = "LFO 2 sync mode: Free, Slave, Master.";
    // Env/LFO 3
    m["env3_attack"]    = "Env 3 attack time (0..127).";
    m["env3_decay"]     = "Env 3 decay time (0..127).";
    m["env3_sustain"]   = "Env 3 sustain level (0..127).";
    m["env3_release"]   = "Env 3 release time (0..127).";
    m["env3_lfo_shape"] = "LFO 3 waveform: Triangle, Square, S&H, Ramp.";
    m["env3_lfo_rate"]  = "LFO 3 rate (0..142; <15 = tempo-synced).";
    m["env3_lfo_sync"]  = "LFO 3 sync mode: Free, Slave, Master.";

    // ---- Voice LFO (2) ----
    m["voice_lfo_shape"] = "Voice LFO waveform: Triangle, Square, S&H, Ramp.";
    m["voice_lfo_rate"]  = "Voice LFO rate (0..127).";

    // ---- 14 modulation routings (42 = 3 each) ----
    m["mod1_source"]  = "Mod 1 source (env, LFO, seq, velocity, pitch bend...).";
    m["mod1_dest"]    = "Mod 1 destination parameter (Filter Cutoff always gets Env2 x FilterEnv + LFO2 x FilterLFO + key tracking; VCA amounts multiply; matrix slots stack with these pre-routes like the hardware).";
    m["mod1_amount"]  = "Mod 1 depth (-63..+63; bipolar).";
    m["mod2_source"]  = "Mod 2 source (env, LFO, seq, velocity, pitch bend...).";
    m["mod2_dest"]    = "Mod 2 destination parameter.";
    m["mod2_amount"]  = "Mod 2 depth (-63..+63; bipolar).";
    m["mod3_source"]  = "Mod 3 source (env, LFO, seq, velocity, pitch bend...).";
    m["mod3_dest"]    = "Mod 3 destination parameter.";
    m["mod3_amount"]  = "Mod 3 depth (-63..+63; bipolar).";
    m["mod4_source"]  = "Mod 4 source (env, LFO, seq, velocity, pitch bend...).";
    m["mod4_dest"]    = "Mod 4 destination parameter.";
    m["mod4_amount"]  = "Mod 4 depth (-63..+63; bipolar).";
    m["mod5_source"]  = "Mod 5 source (env, LFO, seq, velocity, pitch bend...).";
    m["mod5_dest"]    = "Mod 5 destination parameter.";
    m["mod5_amount"]  = "Mod 5 depth (-63..+63; bipolar).";
    m["mod6_source"]  = "Mod 6 source (env, LFO, seq, velocity, pitch bend...).";
    m["mod6_dest"]    = "Mod 6 destination parameter.";
    m["mod6_amount"]  = "Mod 6 depth (-63..+63; bipolar).";
    m["mod7_source"]  = "Mod 7 source (env, LFO, seq, velocity, pitch bend...).";
    m["mod7_dest"]    = "Mod 7 destination parameter.";
    m["mod7_amount"]  = "Mod 7 depth (-63..+63; bipolar).";
    m["mod8_source"]  = "Mod 8 source (env, LFO, seq, velocity, pitch bend...).";
    m["mod8_dest"]    = "Mod 8 destination parameter.";
    m["mod8_amount"]  = "Mod 8 depth (-63..+63; bipolar).";
    m["mod9_source"]  = "Mod 9 source (env, LFO, seq, velocity, pitch bend...).";
    m["mod9_dest"]    = "Mod 9 destination parameter.";
    m["mod9_amount"]  = "Mod 9 depth (-63..+63; bipolar).";
    m["mod10_source"] = "Mod 10 source (env, LFO, seq, velocity, pitch bend...).";
    m["mod10_dest"]   = "Mod 10 destination parameter.";
    m["mod10_amount"] = "Mod 10 depth (-63..+63; bipolar).";
    m["mod11_source"] = "Mod 11 source (env, LFO, seq, velocity, pitch bend...).";
    m["mod11_dest"]   = "Mod 11 destination parameter.";
    m["mod11_amount"] = "Mod 11 depth (-63..+63; bipolar).";
    m["mod12_source"] = "Mod 12 source (env, LFO, seq, velocity, pitch bend...).";
    m["mod12_dest"]   = "Mod 12 destination parameter.";
    m["mod12_amount"] = "Mod 12 depth (-63..+63; bipolar).";
    m["mod13_source"] = "Mod 13 source (env, LFO, seq, velocity, pitch bend...).";
    m["mod13_dest"]   = "Mod 13 destination parameter.";
    m["mod13_amount"] = "Mod 13 depth (-63..+63; bipolar).";
    m["mod14_source"] = "Mod 14 source (env, LFO, seq, velocity, pitch bend...).";
    m["mod14_dest"]   = "Mod 14 destination parameter.";
    m["mod14_amount"] = "Mod 14 depth (-63..+63; bipolar).";

    // ---- 4 modifiers (12 = 3 each) ----
    m["modif1_in1"] = "Modifier 1 input 1 source.";
    m["modif1_in2"] = "Modifier 1 input 2 source.";
    m["modif1_op"]  = "Modifier 1 operation: Sum, Product, Attenuate, Max, Min, XOR, GE, LE, Quantize, Lag.";
    m["modif2_in1"] = "Modifier 2 input 1 source.";
    m["modif2_in2"] = "Modifier 2 input 2 source.";
    m["modif2_op"]  = "Modifier 2 operation: Sum, Product, Attenuate, Max, Min, XOR, GE, LE, Quantize, Lag.";
    m["modif3_in1"] = "Modifier 3 input 1 source.";
    m["modif3_in2"] = "Modifier 3 input 2 source.";
    m["modif3_op"]  = "Modifier 3 operation: Sum, Product, Attenuate, Max, Min, XOR, GE, LE, Quantize, Lag.";
    m["modif4_in1"] = "Modifier 4 input 1 source.";
    m["modif4_in2"] = "Modifier 4 input 2 source.";
    m["modif4_op"]  = "Modifier 4 operation: Sum, Product, Attenuate, Max, Min, XOR, GE, LE, Quantize, Lag.";

    // ---- Part params (8) ----
    m["part_volume"] = "Part output volume (0..127).";
    m["part_octave"]     = "Part global octave shift (-2..+2).";
    m["part_tuning"] = "Part master tuning in 1/128-semitone steps (-127..+127).";
    m["part_raga"] = "Per-part scale preset (0 = 12-EDO, 1..32 = presets; muted note classes are refused).";
    m["part_spread"] = "Per-voice detune spread for a wider ensemble sound (0..40).";
    m["part_legato"]     = "Legato mode: no re-trigger between legato notes.";
    m["part_portamento"] = "Portamento glide time (0..63).";
    m["part_polyphony"]  = "Voice allocator: Mono, Poly, Unison 2x, Cyclic, Chain.";

    // ---- Sequencer lengths (3) ----
    m["seq_length_1"] = "Sequencer 1 (modulation) step count (1..32).";
    m["seq_length_2"] = "Sequencer 2 (modulation) step count (1..32).";
    m["seq_length_3"] = "Note-sequence step count (1..32).";

    // ---- Arp (5) ----
    m["arp_mode"]       = "Arp / Sequencer engine mode: Off, Arp, Sequencer.";
    m["arp_direction"]  = "Arp note order: Up, Down, Up-Down, As-Played, Random, Chord.";
    m["arp_octave"]     = "Arp octave span (1..4).";
    m["arp_pattern"]    = "Arp note-selection gate pattern (22 stored patterns).";
    m["arp_resolution"] = "Arp rhythmic value synced to host tempo (1/1 .. 1/64T).";

    // ---- Options (3) ----
    m["vca_curve"]   = "VCA response curve: Linearized or Exponential.";
    m["part_select"] = "Selects which Part (1..6) the editor edits.";
    m["filter_card"] = "Filter voicecard board: SMR4, 4P, SVF, Ladder, Polivoks or IR3109; SMR4 is the stock Ambika default.";
    m["filter_drive"] = "Clip and rate-limit drive for the Ladder, SMR4, IR3109, and Polivoks cards; higher values clip lower (1.2 = default).";

    // ---- Per-part FX (78 = 24 slot + 6 chain/master + 48 fxmod) ----
    // Hellcat-exclusive; no Ambika patch byte. Slot/fxmod entries are loop-
    // generated to mirror the descriptor table (see the buildHelpMap comment).
    for (int s = 1; s <= 3; ++s)
    {
        const auto n = std::to_string (s);
        m["fx" + n + "_type"]    = "FX slot " + n + " effect algorithm (None, Diffuser, Pitch Shifter, Clouds Reverb, Echo, ...).";
        m["fx" + n + "_enabled"] = "FX slot " + n + " enable / bypass toggle (0 = bypassed, 1 = active).";
        m["fx" + n + "_drywet"]  = "FX slot " + n + " wet/dry blend (0 = fully dry, 127 = fully wet; a delay before a reverb still sounds in the tail after drying — move it later for an immediate cut).";
        for (int p = 1; p <= 5; ++p)
            m["fx" + n + "_param" + std::to_string (p)] =
                "FX slot " + n + " parameter " + std::to_string (p) +
                " (meaning depends on the selected algorithm; the slot card shows its name).";
    }
    m["fx_topo"]     = "FX chain topology: FX1 -> FX2 -> FX3, FX1 + FX2 -> FX3, or FX1 -> FX2 + FX3.";
    m["fx_order"]    = "Slot order within the FX chain (permutation index 0..5).";
    m["fx_mix"]      = "Global FX wet/dry mix applied after the chain (0 = fully dry, 127 = fully wet).";
    m["fx_eq_low"]   = "Master FX low-cut (high-pass): 0 = Off, otherwise the cutoff frequency.";
    m["fx_eq_mid"]   = "Master FX mid peaking gain (64 = 0 dB; about 0.19 dB per step).";
    m["fx_eq_high"]  = "Master FX high-shelf gain (64 = 0 dB; about 0.19 dB per step).";
    for (int fm = 1; fm <= 16; ++fm)
    {
        const auto q = std::to_string (fm);
        m["fxmod" + q + "_source"] = "FX mod " + q + " source (env, LFO, seq, velocity, pitch bend...).";
        m["fxmod" + q + "_dest"]   = "FX mod " + q + " destination (one FX parameter).";
        m["fxmod" + q + "_amount"] = "FX mod " + q + " depth (-63..+63; bipolar).";
    }

    return m;
}
}  // namespace

const std::unordered_map<std::string, std::string>& hellcatParamHelp()
{
    static const auto map = buildHelpMap();
    return map;
}

juce::String getParamHelp (const juce::String& paramID)
{
    // 1) Curated map hit.
    const auto& map = hellcatParamHelp();
    const auto it = map.find (paramID.toStdString());
    if (it != map.end())
        return juce::String (it->second);

    // 2) Generated help for the 64 step-sequencer entries (seq1_step*, seq2_step*,
    //    seqnote_step*, seqnote_vel*). Parse the trailing digits off a known
    //    prefix; indices are 0-based (descriptor step 0 -> human step 1).
    struct PrefixSpec { const char* prefix; int kind; };
    static const PrefixSpec specs[] = {
        { "seq1_step",    0 },
        { "seq2_step",    1 },
        { "seqnote_step", 2 },
        { "seqnote_vel",  3 },
    };

    for (const auto& s : specs)
    {
        const juce::String prefix (s.prefix);
        if (paramID.startsWith (prefix))
        {
            const juce::String rest = paramID.substring (prefix.length());
            if (rest.isNotEmpty() && rest.containsOnly ("0123456789"))
            {
                const int index = rest.getIntValue();   // 0-based step index
                const juce::String stepNo (index + 1);  // 1-based for the user
                switch (s.kind)
                {
                    case 0: return "Sequencer 1 step "  + stepNo + " value (0..127, modulation source).";
                    case 1: return "Sequencer 2 step "  + stepNo + " value (0..127, modulation source).";
                    case 2: return "Note sequence step " + stepNo + ": MIDI note (0..127) | gate flag (bit 7).";
                    case 3: return "Note sequence step " + stepNo + ": velocity (0..127) | legato flag (bit 7).";
                    default: break;   // unknown kind: fall through to try the next prefix spec
                }
            }
        }
    }

    // 3) Genuinely unknown ID.
    return {};
}

bool hasParamHelp (const juce::String& paramID)
{
    return getParamHelp (paramID).isNotEmpty();
}

// Copyright (c) 2024 805LABS / Parvati.  See ParameterLayout.h.

#include "ParameterLayout.h"

#include "dsp/patch.h"  // enum value counts + the init patch field semantics

namespace
{
// ---- Choice string lists (sizes match the patch.h enum LAST values) ---------

// OscillatorAlgorithm: WAVEFORM_LAST == 38 (NONE..VOWEL, 16 wavetables, wavequence).
juce::StringArray makeOscShapes()
{
    juce::StringArray a;
    a.add ("None");        a.add ("Saw");        a.add ("Square");     a.add ("Triangle");
    a.add ("Sine");
    a.add ("CZ Saw");      a.add ("CZ Saw LP");  a.add ("CZ Saw PK");  a.add ("CZ Saw BP");
    a.add ("CZ Saw HP");
    a.add ("CZ PLS LP");   a.add ("CZ PLS PK");  a.add ("CZ PLS BP");  a.add ("CZ PLS HP");
    a.add ("CZ Tri LP");   a.add ("Quad Saw Pad"); a.add ("FM");       a.add ("8-Bit Land");
    a.add ("Dirty PWM");   a.add ("Filtered Noise"); a.add ("Vowel");
    // Indices 21..36 = wavetables 1..16 (WAVEFORM_WAVETABLE_1 .. WAVEFORM_WAVETABLE_16).
    for (int i = 1; i <= 16; ++i)
        a.add ("Wavetable " + juce::String (i));
    a.add ("Wavequence");  // WAVEFORM_WAVEQUENCE == 37
    jassert (a.size() == ambika::dsp::WAVEFORM_LAST);
    return a;
}

// SubOscillatorAlgorithm: WAVEFORM_SUB_OSC_LAST == 11.
juce::StringArray makeSubOscShapes()
{
    return {
        "Sub Square 1", "Sub Triangle 1", "Sub Pulse 1",
        "Sub Square 2", "Sub Triangle 2", "Sub Pulse 2",
        "Click", "Glitch", "Blow", "Metallic", "Pop"
    };
}

// Operator: OP_LAST == 6.
juce::StringArray makeMixOps()
{
    return { "Sum", "Sync", "Ring Mod", "XOR", "Fold", "Bits" };
}

// FilterMode == 4.
juce::StringArray makeFilterModes()
{
    return { "LP", "BP", "HP", "Notch" };
}

// Voice LFO shapes: limited to 0..LFO_WAVEFORM_RAMP (4) per parameter.cc.
// NOTE: the in-engine LFO 1/2/3 cannot render wavetable shapes 4..19
// (PARVATI_DISABLE_WAVETABLE_LFOS), so both the voice LFO and the 3 env-lfo
// units expose only these 4 shapes (Triangle/Square/S&H/Ramp).
juce::StringArray makeVoiceLfoShapes()
{
    return { "Triangle", "Square", "S&H", "Ramp" };
}

// LfoSyncMode: LFO_SYNC_MODE_LAST == 3.
juce::StringArray makeLfoSyncModes()
{
    return { "Free", "Slave", "Master" };
}

// ModulationSource: MOD_SRC_LAST == 31.
juce::StringArray makeModSources()
{
    return {
        "Env 1", "Env 2", "Env 3",
        "LFO 1", "LFO 2", "LFO 3", "LFO 4",
        "Op 1", "Op 2", "Op 3", "Op 4",
        "Seq 1", "Seq 2", "Arp Step",
        "Velocity", "Aftertouch", "Pitch Bend", "Wheel", "Wheel 2", "Expression",
        "Note", "Gate", "Noise", "Random",
        "Const 256", "Const 128", "Const 64", "Const 32", "Const 16", "Const 8", "Const 4"
    };
}

// ModulationDestination: MOD_DST_LAST == 19.
juce::StringArray makeModDests()
{
    return {
        "Parameter 1", "Parameter 2", "Osc 1", "Osc 2", "Osc 1+2 Coarse", "Osc 1+2 Fine",
        "Mix Balance", "Mix Param", "Mix Noise", "Mix Sub Osc", "Mix Fuzz", "Mix Crush",
        "Filter Cutoff", "Filter Resonance",
        "Attack", "Decay", "Release", "LFO 4", "VCA"
    };
}

// ModifierOp: MODIFIER_LAST == 11.
juce::StringArray makeModifierOps()
{
    return { "None", "Sum", "Product", "Attenuate", "Max", "Min", "XOR", "GE", "LE", "Quantize", "Lag" };
}

const juce::StringArray kOnOff { "Off", "On" };

// PolyphonyMode (firmware part.h:58): Mono/Poly/Unison2x/Cyclic/Chain. Default Poly (1).
const juce::StringArray kPolyModes { "Mono", "Poly", "Unison 2x", "Cyclic", "Chain" };

// ---- Arpeggiator choice lists ----
juce::StringArray makeArpModes()       { return { "Off", "Arp", "Sequencer" }; }
juce::StringArray makeArpDirections()  { return { "Up", "Down", "Up-Down", "As-Played", "Random", "Chord" }; }
juce::StringArray makeArpResolutions() { return { "1/1", "1/2.", "1/2", "1/4.", "1/4T", "1/4", "1/8.", "1/8T", "1/8", "1/16.", "1/16T", "1/16", "1/32.", "1/32", "1/64" }; }
juce::StringArray makeArpPatterns()
{
    juce::StringArray a;
    for (int i = 0; i < 22; ++i)
        a.add ("Pattern " + juce::String (i));
    return a;
}

// ---- The factory init patch (from ambika_reference/controller/part.cc) ----
// This is the controller-side init_patch (part.cc:39-81) — the actual patch a
// user sees on power-up / "Init Patch" (Part::InitPatch(DEFAULT)). It is NOT the
// voicecard silence fallback (voicecard/voice.cc), which is only used when a
// voicecard boots with no patch loaded over SPI and is never heard. Every APVTS
// default is derived from these bytes (see the `add()` lambda below), so this
// single table defines all factory patch-parameter defaults. Indices follow the
// Patch struct byte layout in patch.h exactly.
struct InitPatch
{
    static constexpr int kSize = 112;
    // clang-format off
    static constexpr uint8_t bytes[kSize] = {
        // osc[0] (0..3)
        ambika::dsp::WAVEFORM_SAW, 0, 0, 0,
        // osc[1] (4..7)  -- range -12 stored as its uint8 byte (244)
        ambika::dsp::WAVEFORM_SQUARE, 32, static_cast<uint8_t> (-12), 12,
        // mix (8..15): balance, op, param, sub_shape, sub, noise, fuzz, crush
        32, ambika::dsp::OP_SUM, 31, ambika::dsp::WAVEFORM_SUB_OSC_SQUARE_1, 0, 0, 0, 0,
        // filter[0] (16..18), filter[1] (19..21), filter_env (22), filter_lfo (23)
        96, 0, 0,   0, 0, 0,   24, 0,
        // env_lfo[0] (24..31): A D S R | lfo_shape rate pad retrigger
        0, 40, 20, 60, ambika::dsp::LFO_WAVEFORM_TRIANGLE, ambika::dsp::kNumSyncedLfoRates + 24, 0, 0,
        // env_lfo[1] (32..39)
        0, 40, 0, 40, ambika::dsp::LFO_WAVEFORM_TRIANGLE, ambika::dsp::kNumSyncedLfoRates + 32, 0, 0,
        // env_lfo[2] (40..47)
        0, 40, 100, 40, ambika::dsp::LFO_WAVEFORM_TRIANGLE, ambika::dsp::kNumSyncedLfoRates + 48, 0, 0,
        // voice_lfo (48..49)
        ambika::dsp::LFO_WAVEFORM_TRIANGLE, 72,
        // modulation[0..13] (50..91): source, dest, amount
        ambika::dsp::MOD_SRC_ENV_1,      ambika::dsp::MOD_DST_PARAMETER_1, 0,
        ambika::dsp::MOD_SRC_ENV_1,      ambika::dsp::MOD_DST_PARAMETER_2, 0,
        ambika::dsp::MOD_SRC_LFO_1,      ambika::dsp::MOD_DST_OSC_1, 0,
        ambika::dsp::MOD_SRC_LFO_1,      ambika::dsp::MOD_DST_OSC_2, 0,
        ambika::dsp::MOD_SRC_LFO_2,      ambika::dsp::MOD_DST_PARAMETER_1, 0,
        ambika::dsp::MOD_SRC_LFO_2,      ambika::dsp::MOD_DST_PARAMETER_2, 0,
        ambika::dsp::MOD_SRC_LFO_3,      ambika::dsp::MOD_DST_MIX_BALANCE, 0,
        ambika::dsp::MOD_SRC_LFO_4,      ambika::dsp::MOD_DST_FILTER_CUTOFF, 0,
        ambika::dsp::MOD_SRC_SEQ_1,      ambika::dsp::MOD_DST_FILTER_CUTOFF, 0,
        ambika::dsp::MOD_SRC_SEQ_2,      ambika::dsp::MOD_DST_MIX_BALANCE, 0,
        ambika::dsp::MOD_SRC_ENV_3,      ambika::dsp::MOD_DST_VCA, 63,
        ambika::dsp::MOD_SRC_VELOCITY,   ambika::dsp::MOD_DST_VCA, 16,
        ambika::dsp::MOD_SRC_PITCH_BEND, ambika::dsp::MOD_DST_OSC_1_2_COARSE, 32,
        ambika::dsp::MOD_SRC_LFO_4,      ambika::dsp::MOD_DST_OSC_1_2_COARSE, 16,
        // modifier[0..3] (92..103): in1, in2, op
        ambika::dsp::MOD_SRC_LFO_1, ambika::dsp::MOD_SRC_LFO_2, 0,
        ambika::dsp::MOD_SRC_LFO_2, ambika::dsp::MOD_SRC_LFO_3, 0,
        ambika::dsp::MOD_SRC_LFO_3, ambika::dsp::MOD_SRC_SEQ_1, 0,
        ambika::dsp::MOD_SRC_SEQ_1, ambika::dsp::MOD_SRC_SEQ_2, 0,
        // padding (104..111)
        0, 0, 0, 0, 0, 0, 0, 0
    };
    // clang-format on
};
constexpr uint8_t InitPatch::bytes[InitPatch::kSize];

// Part defaults (controller init_part, part.cc:83-100):
//   volume=120, legato=0, portamento=0 (Part bytes 0,5,6).
constexpr uint8_t kInitPartVolume    = 120;
constexpr uint8_t kInitPartLegato    = 0;
constexpr uint8_t kInitPartPortamento = 0;

uint8_t initPartByte (int offset)
{
    switch (offset)
    {
        case 0:  return kInitPartVolume;
        case 3:  return 0;   // PartData.spread (firmware init = 0)
        case 5:  return kInitPartLegato;
        case 6:  return kInitPartPortamento;
        case 15: return 1;   // PartData.polyphony_mode: POLY (firmware default)
        default: return 0;
    }
}
}  // namespace

// External linkage (outside the anonymous namespace above) so the engine and
// processor can link it. InitPatch::bytes lives in the anonymous namespace but
// is reachable from anywhere in this TU.
const uint8_t* getControllerInitPatchBytes() { return InitPatch::bytes; }

const std::vector<PatchParamDescriptor>& getPatchParamDescriptors()
{
    static const auto kOscShapes      = makeOscShapes();
    static const auto kSubOscShapes  = makeSubOscShapes();
    static const auto kMixOps        = makeMixOps();
    static const auto kFilterModes   = makeFilterModes();
    static const auto kVoiceLfoShapes = makeVoiceLfoShapes();
    static const auto kLfoSyncModes  = makeLfoSyncModes();
    static const auto kModSources    = makeModSources();
    static const auto kModDests      = makeModDests();
    static const auto kModifierOps   = makeModifierOps();

    static const std::vector<PatchParamDescriptor> table = [&]()
    {
        std::vector<PatchParamDescriptor> d;

        const auto add = [&] (std::string id, std::string label, int off, bool part,
                              bool isSigned, const juce::StringArray* choices,
                              int mn, int mx)
        {
            PatchParamDescriptor p;
            p.paramID = std::move (id);
            p.label   = std::move (label);
            p.byteOffset = off;
            p.isPart  = part;
            p.isSigned = isSigned;
            p.choices = choices;
            p.minValue = mn;
            p.maxValue = mx;
            // Default value = the faithful init byte (signed reinterpreted if needed).
            const uint8_t b = part ? initPartByte (off) : InitPatch::bytes[off];
            p.defaultValue = isSigned ? static_cast<int> (static_cast<int8_t> (b))
                                      : static_cast<int> (b);
            d.push_back (std::move (p));
        };

        // ---- Oscillators (patch bytes 0..7) ----
        add ("osc1_shape",  "Osc 1 Shape",   0, false, false, &kOscShapes,    0, 0);
        add ("osc1_param",  "Osc 1 Param",   1, false, false, nullptr,        0, 127);
        add ("osc1_range",  "Osc 1 Range",   2, false, true,  nullptr,      -24, 24);
        add ("osc1_detune", "Osc 1 Detune",  3, false, true,  nullptr,      -64, 64);
        add ("osc2_shape",  "Osc 2 Shape",   4, false, false, &kOscShapes,    0, 0);
        add ("osc2_param",  "Osc 2 Param",   5, false, false, nullptr,        0, 127);
        add ("osc2_range",  "Osc 2 Range",   6, false, true,  nullptr,      -24, 24);
        add ("osc2_detune", "Osc 2 Detune",  7, false, true,  nullptr,      -64, 64);

        // ---- Mixer (patch bytes 8..15) ----
        add ("mix_balance",  "Mix Balance",   8,  false, false, nullptr,        0, 63);
        add ("mix_op",       "Mix Op",        9,  false, false, &kMixOps,       0, 0);
        add ("mix_param",    "Mix Parameter", 10, false, false, nullptr,        0, 63);
        add ("mix_sub_shape","Sub Osc Shape", 11, false, false, &kSubOscShapes, 0, 0);
        add ("mix_sub",      "Sub Osc Level", 12, false, false, nullptr,        0, 63);
        add ("mix_noise",    "Noise Level",   13, false, false, nullptr,        0, 63);
        add ("mix_fuzz",     "Fuzz",          14, false, false, nullptr,        0, 63);
        add ("mix_crush",    "Crush",         15, false, false, nullptr,        0, 31);

        // ---- Filters (patch bytes 16..23) ----
        add ("filter1_cutoff", "Filter 1 Cutoff",     16, false, false, nullptr,       0, 127);
        add ("filter1_reso",   "Filter 1 Resonance",  17, false, false, nullptr,       0, 63);
        add ("filter1_mode",   "Filter 1 Mode",       18, false, false, &kFilterModes, 0, 0);
        // NOTE: filter[1] (patch bytes 19..21) is a reserved/unused slot — the
        // engine only ever reads patch_.filter[0] (voice.cpp cutoff/resonance +
        // voice.h mode()). Exposing it here would show 3 inert "Filter 2"
        // widgets, so it is intentionally NOT added to the APVTS bridge.
        add ("filter_env",     "Filter Env Amount",   22, false, false, nullptr,       0, 63);
        add ("filter_lfo",     "Filter LFO Amount",   23, false, false, nullptr,       0, 63);

        // ---- 3 env+lfo units (patch bytes 24..47; stride 8) ----
        for (int i = 0; i < ambika::dsp::kNumEnvelopes; ++i)
        {
            const int base = 24 + 8 * i;
            const std::string n = std::to_string (i + 1);
            add ("env" + n + "_attack",     "Env " + n + " Attack",     base + 0, false, false, nullptr,       0, 127);
            add ("env" + n + "_decay",      "Env " + n + " Decay",      base + 1, false, false, nullptr,       0, 127);
            add ("env" + n + "_sustain",    "Env " + n + " Sustain",    base + 2, false, false, nullptr,       0, 127);
            add ("env" + n + "_release",    "Env " + n + " Release",    base + 3, false, false, nullptr,       0, 127);
            add ("env" + n + "_lfo_shape",  "Env " + n + " LFO Shape",  base + 4, false, false, &kVoiceLfoShapes, 0, 0);
            add ("env" + n + "_lfo_rate",   "Env " + n + " LFO Rate",   base + 5, false, false, nullptr,       0, ambika::dsp::kNumSyncedLfoRates + 127);
            // byte base+6 is `padding` (not exposed); base+7 is retrigger_mode == LFO sync.
            add ("env" + n + "_lfo_sync",   "Env " + n + " LFO Sync",   base + 7, false, false, &kLfoSyncModes, 0, 0);
        }

        // ---- Voice LFO (patch bytes 48..49) ----
        add ("voice_lfo_shape", "Voice LFO Shape", 48, false, false, &kVoiceLfoShapes, 0, 0);
        add ("voice_lfo_rate",  "Voice LFO Rate",  49, false, false, nullptr,          0, 127);

        // ---- 14 modulation routings (patch bytes 50..91; stride 3) ----
        for (int j = 0; j < ambika::dsp::kNumModulations; ++j)
        {
            const int base = 50 + 3 * j;
            const std::string n = std::to_string (j + 1);
            add ("mod" + n + "_source", "Mod " + n + " Source", base + 0, false, false, &kModSources, 0, 0);
            add ("mod" + n + "_dest",   "Mod " + n + " Dest",   base + 1, false, false, &kModDests,   0, 0);
            add ("mod" + n + "_amount", "Mod " + n + " Amount", base + 2, false, true,  nullptr,    -63, 63);
        }

        // ---- 4 modifiers (patch bytes 92..103; stride 3) ----
        for (int k = 0; k < ambika::dsp::kNumModifiers; ++k)
        {
            const int base = 92 + 3 * k;
            const std::string n = std::to_string (k + 1);
            add ("modif" + n + "_in1", "Modifier " + n + " In 1", base + 0, false, false, &kModSources,  0, 0);
            add ("modif" + n + "_in2", "Modifier " + n + " In 2", base + 1, false, false, &kModSources,  0, 0);
            add ("modif" + n + "_op",  "Modifier " + n + " Op",   base + 2, false, false, &kModifierOps, 0, 0);
        }

        // ---- Part params (Part struct bytes: volume=0, legato=5, portamento=6) ----
        add ("part_volume",     "Volume",     0, true, false, nullptr, 0, 127);
        add ("part_octave",     "Octave",     1, true, true,  nullptr, -2, 2);   // PartData.octave (int8)
        add ("part_tuning",     "Tuning",     2, true, true,  nullptr, -127, 127); // PartData.tuning (int8)
        add ("part_spread",     "Spread",     3, true, false, nullptr,    0, 40);   // PartData.spread (uint8): per-voice detune
        add ("part_legato",     "Legato",     5,  true, false, &kOnOff,    0, 0);
        add ("part_portamento", "Portamento", 6,  true, false, nullptr,    0, 63);
        add ("part_polyphony",  "Polyphony",  15, true, false, &kPolyModes, 0, 0);

        // ---- Step-sequencer (controller PartData; routed to the engine
        // Sequencer, not the voicecard Part). byteOffset = controller PartData. ----
        // sequence_length[3] @ 12/13/14; sequence_data[64] @ 16..79:
        //   0..15 -> seq1 steps, 16..31 -> seq2 steps, 32..63 -> note seq.
        auto addSeq = [&] (std::string id, std::string label, int off, int mn, int mx, int defVal = 0)
        {
            PatchParamDescriptor p;
            p.paramID = std::move (id);
            p.label   = std::move (label);
            p.byteOffset = off;
            p.isSequencer = true;   // controller PartData; routed to engine Sequencer
            p.minValue = mn;
            p.maxValue = mx;
            p.defaultValue = defVal;
            d.push_back (std::move (p));
        };
        addSeq ("seq_length_1", "Seq 1 Length", 12, 1, 32, 16);   // firmware UNIT_UINT8 1..32, default 16
        addSeq ("seq_length_2", "Seq 2 Length", 13, 1, 32, 16);
        addSeq ("seq_length_3", "Seq 3 Length", 14, 1, 32, 16);
        for (int s = 0; s < 16; ++s)
            addSeq ("seq1_step"  + std::to_string (s), "Seq1 Step "  + std::to_string (s + 1), 16 + s,    0, 127);
        for (int s = 0; s < 16; ++s)
            addSeq ("seq2_step"  + std::to_string (s), "Seq2 Step "  + std::to_string (s + 1), 32 + s,    0, 127);
        // Note sequence: 16 steps x 2 bytes (note|gate, velocity|legato). Both
        // bytes are exposed 0..255 so the gate (bit 7 of the note byte) and the
        // legato flag (bit 7 of the velocity byte) survive .MUL load/edit. The
        // Sequencer decodes note=byte&0x7f, gate=byte&0x80 (likewise velocity).
        for (int s = 0; s < 16; ++s)
            addSeq ("seqnote_step" + std::to_string (s), "Note/Gate " + std::to_string (s + 1), 48 + s * 2, 0, 255);
        for (int s = 0; s < 16; ++s)
            addSeq ("seqnote_vel" + std::to_string (s), "Vel/Legato " + std::to_string (s + 1), 49 + s * 2, 0, 255);

        // ---- Arpeggiator (controller-side; no Patch byte) ----
        {
            static const auto kArpModes       = makeArpModes();
            static const auto kArpDirections  = makeArpDirections();
            static const auto kArpPatterns    = makeArpPatterns();
            static const auto kArpResolutions = makeArpResolutions();
            auto addArp = [&] (std::string id, std::string label,
                               const juce::StringArray* choices, int defVal,
                               int mn = 0, int mx = 0)
            {
                PatchParamDescriptor p;
                p.paramID = std::move (id);
                p.label   = std::move (label);
                p.byteOffset = -1;
                p.isArp  = true;
                p.choices = choices;
                p.minValue = mn;
                p.maxValue = mx;
                p.defaultValue = defVal;
                d.push_back (std::move (p));
            };
            addArp ("arp_mode",       "Arp Mode",       &kArpModes,       0);          // Off
            addArp ("arp_direction",  "Arp Direction",  &kArpDirections,  0);          // Up
            addArp ("arp_octave",     "Arp Octave",     nullptr,          1, 1, 4);
            addArp ("arp_pattern",    "Arp Pattern",    &kArpPatterns,    0);          // 0
            addArp ("arp_resolution", "Arp Resolution", &kArpResolutions, 10);         // 1/16 (firmware init_part divider=10)
        }

        // ---- Synth options (no Patch byte; routed specially) ----
        {
            static const auto kVcaCurves = juce::StringArray { "Linearized", "Exponential" };
            PatchParamDescriptor v;
            v.paramID = "vca_curve";
            v.label   = "VCA Curve";
            v.byteOffset = -1;
            v.isOption = true;
            v.choices = &kVcaCurves;
            v.defaultValue = 0;   // Linearized (firmware linearization-table mode)
            d.push_back (std::move (v));
        }

        // ---- Multitimbral Part selector (1..6; no Patch byte) ----
        {
            PatchParamDescriptor ps;
            ps.paramID = "part_select";
            ps.label   = "Part";
            ps.byteOffset = -1;
            ps.isOption = true;
            ps.minValue = 1;
            ps.maxValue = 6;   // kNumParts (hardware: 6 voicecards)
            ps.defaultValue = 1;   // Part 1 (channel 1) by default
            d.push_back (std::move (ps));
        }

        // ---- GLOBAL filter-card topology (one Ambika unit = one filter card) ----
        {
            static const auto kFilterCards = juce::StringArray {
                "4-pole LM13700 (Ladder)",
                "4-pole SSM2164 (Cascade)",
                "2-pole SVF"
            };
            PatchParamDescriptor f;
            f.paramID = "filter_card";
            f.label   = "Filter Card";
            f.byteOffset = -1;
            f.isOption = true;
            f.choices = &kFilterCards;
            f.defaultValue = 0;   // LM13700 (most common Ambika card)
            d.push_back (std::move (f));
        }

        // ---- Ladder saturation drive (Parvati-only; no Ambika patch byte) ----
        // Scales the juce::dsp::LadderFilter tanh saturator. The default entry
        // "1.2" is the JUCE LadderFilter ctor default, so an untouched control
        // reproduces the pre-control sound exactly. Only affects the Ladder
        // card; carried by .parvati (isOption), dropped by .PRO/.MUL (correct:
        // there is no Ambika byte for it).
        {
            static const auto kFilterDrives = juce::StringArray {
                "1.0", "1.2", "1.5", "2.0", "3.0", "5.0", "8.0", "12.0"
            };
            PatchParamDescriptor fd;
            fd.paramID = "filter_drive";
            fd.label   = "Filter Drive";
            fd.byteOffset = -1;
            fd.isOption = true;
            fd.choices = &kFilterDrives;
            fd.defaultValue = 1;   // "1.2" == juce::dsp::LadderFilter default
            d.push_back (std::move (fd));
        }

        return d;
    }();

    return table;
}

juce::AudioProcessorValueTreeState::ParameterLayout createParvatiParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (const auto& d : getPatchParamDescriptors())
    {
        if (d.choices != nullptr)
        {
            layout.add (std::make_unique<juce::AudioParameterChoice> (
                juce::ParameterID { d.paramID, 1 }, d.label, *d.choices, d.defaultValue));
        }
        else
        {
            layout.add (std::make_unique<juce::AudioParameterInt> (
                juce::ParameterID { d.paramID, 1 }, d.label, d.minValue, d.maxValue, d.defaultValue));
        }
    }
    return layout;
}

uint8_t parvatiValueToPatchByte (const PatchParamDescriptor& d, float rawValue)
{
    if (d.isArp || d.isOption || d.isSequencer)
        return 0;  // arp / option / sequencer params have no patch byte

    // APVTS stores integer params as denormalized floats, which can drift by
    // a tiny epsilon (e.g. 63 -> 62.999996). Truncating would chop that to 62;
    // round to the nearest integer so the byte is sound-faithful on load, live
    // edits, and save. (juce::roundToInt is already available via the JUCE headers.)
    int v = juce::roundToInt (rawValue);
    if (d.choices != nullptr)
    {
        jassert (d.choices->size() > 0);
        return static_cast<uint8_t> (juce::jlimit (0, d.choices->size() - 1, v));
    }
    v = juce::jlimit (d.minValue, d.maxValue, v);
    return d.isSigned ? static_cast<uint8_t> (static_cast<int8_t> (v))
                      : static_cast<uint8_t> (v);
}

float parvatiPatchByteToValue (const PatchParamDescriptor& d, uint8_t byte)
{
    // Reverse of parvatiValueToPatchByte: the raw byte back to the APVTS
    // (denormalized) value. Choice params store the enum index; Int params the
    // signed/unsigned value.
    if (d.choices != nullptr)
    {
        jassert (d.choices->size() > 0);
        return static_cast<float> (juce::jlimit (0, d.choices->size() - 1, (int) byte));
    }
    return d.isSigned ? static_cast<float> (static_cast<int8_t> (byte))
                      : static_cast<float> (byte);
}

// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See ParameterLayout.h.

#include "ParameterLayout.h"

#include "TuningTables.h"   // part_raga choice names (firmware raga presets)

#include "dsp/fx/FxTypes.h"   // FxType / FxModDestination + counts (FX descriptors)
#include "dsp/patch.h"  // enum value counts + the init patch field semantics

// Host-visible parameter TEXT (wired below via AudioParameterIntAttributes).
// Both formatter shards are juce_core-only (no juce_gui): pure LUT/math on the
// raw integer value, safe to call from arbitrary host threads. The master-EQ
// readouts (fxEqLowToString/fxEqDbToString) were hoisted here from
// FxRoutingBar.cpp so the host text and the UI knob text are ONE implementation.
#include "ui/FxSlotLabels.h"
#include "ui/FormatHelpers.h"   // signedAmountPercent (fxmod amount host strings)
#include "ui/SynthParamLabels.h"

#include <array>
#include <utility>
#include <vector>

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

// part_raga (PartData byte 4): "Off" + the 32 firmware scale presets. The
// choice INDEX is the raga byte (0 = 12-EDO, 1..32 = presets; files can carry
// 32 = rasia, a superset of the hardware UI's 0..31 — see TuningTables.h).
juce::StringArray makeTuningPresetNames()
{
    juce::StringArray a;
    a.add ("Off");
    for (int id = 1; id <= parvati::kNumTuningPresets; ++id)
        a.add (juce::String (parvati::tuningPresetName (id)));
    jassert (a.size() == parvati::kNumTuningPresets + 1);
    return a;
}

// ModulationSource: MOD_SRC_LAST == 31.
juce::StringArray makeModSources()
{
    return {
        "Env 1", "Env 2", "Env 3",
        "LFO 1", "LFO 2", "LFO 3", "Voice LFO",
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
        "Osc Param 1", "Osc Param 2", "Osc 1", "Osc 2", "Osc 1+2 Coarse", "Osc 1+2 Fine",
        "Mix Balance", "Mix Param", "Mix Noise", "Mix Sub Osc", "Mix Fuzz", "Mix Crush",
        "Filter Cutoff", "Filter Resonance",
        "Attack", "Decay", "Release", "Voice LFO", "VCA"
    };
}

// ModifierOp: MODIFIER_LAST == 11.
juce::StringArray makeModifierOps()
{
    return { "None", "Sum", "Product", "Attenuate", "Max", "Min", "XOR", "GE", "LE", "Quantize", "Lag" };
}

// FxType choice list — hoisted OUT of the anonymous namespace (external
// linkage) so tests can pin the DISPLAY labels; see the definition after the
// namespace close and the declaration in ParameterLayout.h.

// FxTopology choice list (Series / Parallel12to3 / Parallel1to23), shown as
// human signal-FLOW strings in the compact routing bar's FLOW dropdown. Exactly
// 3 entries; the stored index is unchanged, so this is serialization-safe.
juce::StringArray makeFxTopologies()
{
    return { "FX1 -> FX2 -> FX3", "FX1 + FX2 -> FX3", "FX1 -> FX2 + FX3" };
}

// FxModDestination choice list (FX_DST_FX1_DRYWET .. FX_DST_FX3_P5). 18 entries
// (one dry/wet + five generic params per slot); matches the FxModDestination
// enum order. FX_DST_NONE (-1) is NOT a choice entry — an inactive mod slot is
// signalled by amount=0, not a special dest.
juce::StringArray makeFxDests()
{
    return {
        "FX1 Dry/Wet", "FX1 Param 1", "FX1 Param 2", "FX1 Param 3", "FX1 Param 4", "FX1 Param 5",
        "FX2 Dry/Wet", "FX2 Param 1", "FX2 Param 2", "FX2 Param 3", "FX2 Param 4", "FX2 Param 5",
        "FX3 Dry/Wet", "FX3 Param 1", "FX3 Param 2", "FX3 Param 3", "FX3 Param 4", "FX3 Param 5"
    };
}

const juce::StringArray kOnOff { "Off", "On" };

// PolyphonyMode (firmware part.h:58): Mono/Poly/Unison2x/Cyclic/Chain. Default Poly (1).
const juce::StringArray kPolyModes { "Mono", "Poly", "Unison 2x", "Cyclic", "Chain" };

// ---- Arpeggiator choice lists ----
juce::StringArray makeArpModes()       { return { "Off", "Arp", "Sequencer" }; }
juce::StringArray makeArpDirections()  { return { "Up", "Down", "Up-Down", "As-Played", "Random", "Chord" }; }
// Tick-indexed for kMidiClockTickPerStep = {96,72,64,48,36,32,24,16,12,8,6,4,3,2,1}
// (Arpeggiator.h): 96=1/1 72=1/2. 64=1/1T 48=1/2 36=1/4. 32=1/2T 24=1/4 16=1/4T
// 12=1/8 8=1/8T 6=1/16 4=1/16T 3=1/32 2=1/32T 1=1/64T. Firmware display strings
// (resources.cc STR_RES_*) carry the same values as fractions of a whole note
// (72="3/4", 64="2/3", 36="3/8"...); we use dotted/triplet notation, matching
// ui/SynthParamLabels.cpp kSyncedDivisions. The factory default index 10 =
// 6 ticks = straight 1/16 (part.cc init arp bytes {0,1,0,10}).
juce::StringArray makeArpResolutions()
{
    return { "1/1", "1/2.", "1/1T", "1/2", "1/4.", "1/2T", "1/4", "1/4T",
             "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T", "1/64T" };
}
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

// FxType choice list (FxType::None..Spring — the FV-1 family is APPEND-ONLY).
// Matches the enum order. Display-only: the stored value is the choice INDEX
// (APVTS flushToTree serializes the denormalized float, never the text), so
// renaming a label — e.g. "Reverb" -> "CVerb", "Echo" -> "Digital Echo",
// "LUT Distortion" -> "LUT" -> "Wavemangler" — never breaks saved sessions/presets.
// External linkage (outside the anonymous namespace) + declared in
// ParameterLayout.h so tests/fx_param_coverage_test.cpp can pin the labels.
juce::StringArray makeFxTypes()
{
    return { "None", "Diffuser", "Pitch Shifter", "CVerb",
             "Looping Delay", "Pitch Stretch", "Spectral",
             "Wavefolder", "Frequency Shifter", "Ring Modulator", "Resonator",
             "Clocked Delay", "Ensemble", "Plate",
             "Vinyl Compressor", "Phaser",
             "Overdrive", "Wavemangler", "Compressor", "Gate",
             "Chorus", "Flanger", "Digital Echo", "Room", "Spring" };
}

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
    static const auto kTuningPresets = makeTuningPresetNames();

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
        add ("mix_balance",  "Balance",       8,  false, false, nullptr,        0, 63);
        add ("mix_op",       "Mix Op",        9,  false, false, &kMixOps,       0, 0);
        add ("mix_param",    "Mix Param",     10, false, false, nullptr,        0, 63);
        add ("mix_sub_shape","Sub Shape",     11, false, false, &kSubOscShapes, 0, 0);
        add ("mix_sub",      "Sub Level",     12, false, false, nullptr,        0, 63);
        add ("mix_noise",    "Noise",         13, false, false, nullptr,        0, 63);
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
        // PartData.raga (uint8): per-part scale preset, applied at trigger like
        // firmware Part::TuneNote. Choice index == raga byte (0..32). (A 33rd
        // "custom table" mode existed in the custom-tuning subsystem — removed
        // 2026-08-19.) The firmware exposes raga UI-only (NRPN 0xff,
        // MidiParameterMap firmware_parameters row 46), so this param carries NO
        // CC/NRPN mapping — values travel via the editor, presets and files.
        add ("part_raga",       "Scale",      4, true, false, &kTuningPresets, 0, 32);
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
        addSeq ("seq_length_1", "Seq 1 Length", 12, 1, 16, 16);   // Parvati has 16 step cells/seq (firmware byte allows 1..32)
        addSeq ("seq_length_2", "Seq 2 Length", 13, 1, 16, 16);
        addSeq ("seq_length_3", "Seq 3 Length", 14, 1, 16, 16);
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
                               ambika::dsp::ArpSeqField field, const juce::StringArray* choices,
                               int defVal, int mn = 0, int mx = 0)
            {
                PatchParamDescriptor p;
                p.paramID = std::move (id);
                p.label   = std::move (label);
                // The true controller PartData offset (7..11), from the single
                // byte-domain table. Every byteOffset consumer dispatches on
                // isArp FIRST, so the offset is metadata for the NRPN map
                // (112 + byteOffset) — the apply/save paths keep their isArp
                // staging routes.
                p.byteOffset = ambika::dsp::arpSeqDomain (field).partDataOffset;
                p.isArp  = true;
                p.choices = choices;
                p.minValue = mn;
                p.maxValue = mx;
                p.defaultValue = defVal;
                d.push_back (std::move (p));
            };
            addArp ("arp_mode",       "Arp Mode",       ambika::dsp::ArpSeqField::ArpMode,       &kArpModes,       0);          // Off
            addArp ("arp_direction",  "Arp Direction",  ambika::dsp::ArpSeqField::ArpDirection,  &kArpDirections,  0);          // Up
            addArp ("arp_octave",     "Arp Octave",     ambika::dsp::ArpSeqField::ArpOctave,     nullptr,          1, 1, 4);
            addArp ("arp_pattern",    "Arp Pattern",    ambika::dsp::ArpSeqField::ArpPattern,    &kArpPatterns,    0);          // 0
            addArp ("arp_resolution", "Arp Resolution", ambika::dsp::ArpSeqField::ArpResolution, &kArpResolutions, 10);         // 1/16 (firmware init_part divider=10)
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
            ps.nonAutomatable = true;   // part switching is a UI action, not a
                                        // sound parameter: automation would
                                        // drive the part-load machinery
                                        // (loadPartIntoApvts + full re-sync)
                                        // per automation tick -- now also
                                        // deferred off-thread when it does
                                        // arrive off the message thread.
            d.push_back (std::move (ps));
        }

        // ---- GLOBAL filter-card topology (one Ambika unit = one filter card) ----
        {
            static const auto kFilterCards = juce::StringArray {
                "Ladder (24 dB/oct)",
                "4-pole SSM2164 (Cascade)",
                "2-pole SVF",
                "SMR4 (OTA Cascade)",
                "Polivoks (SVF)",
                "IR3109 (Cascade)"
            };
            PatchParamDescriptor f;
            f.paramID = "filter_card";
            f.label   = "Filter Card";
            f.byteOffset = -1;
            f.isOption = true;
            f.choices = &kFilterCards;
            f.defaultValue = 0;   // Ladder (the saturation-rich 4-pole card)
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

        // ---- Per-part FX (Parvati-exclusive; no Ambika patch byte; per-part) ----
        // 78 params: 3 slots x (type/enabled/drywet/param1..5) = 24, + fx_topo +
        // fx_order = 2, + 16 x (fxmod source/dest/amount) = 48, + 4 master section
        // (fx_mix/fx_eq_low/mid/high, engine-state v3). All isFx=true,
        // byteOffset=-1 (never touch patch/part bytes). Routed via
        // applyFxParameter; loaded per-part in loadPartIntoApvts. Reuses the
        // synth mod-source choice list (makeModSources) for fxmod sources.
        {
            static const auto kFxTypes      = makeFxTypes();
            static const auto kFxTopologies = makeFxTopologies();
            static const auto kFxDests      = makeFxDests();
            static const auto kFxModSources = makeModSources();   // reuse synth sources
            jassert (kFxTypes.size()      == (int) FxType::Count);
            jassert (kFxTopologies.size() == 3);
            jassert (kFxDests.size()      == FX_DST_LAST);
            juce::ignoreUnused (kFxTypes, kFxTopologies, kFxDests, kFxModSources);

            auto addFx = [&] (std::string id, std::string label,
                              const juce::StringArray* choices, int defVal,
                              int mn = 0, int mx = 0)
            {
                PatchParamDescriptor p;
                p.paramID = std::move (id);
                p.label   = std::move (label);
                p.byteOffset = -1;   // FX params carry no Ambika patch/part byte
                p.isFx  = true;
                p.choices = choices;
                p.minValue = mn;
                p.maxValue = mx;
                p.defaultValue = defVal;
                d.push_back (std::move (p));
            };

            for (int s = 1; s <= 3; ++s)
            {
                addFx ("fx" + std::to_string (s) + "_type",    "FX" + std::to_string (s) + " Type",    &kFxTypes, 0);  // None
                addFx ("fx" + std::to_string (s) + "_enabled", "FX" + std::to_string (s) + " Enable",  nullptr,   0, 0, 1);
                addFx ("fx" + std::to_string (s) + "_drywet",  "FX" + std::to_string (s) + " Dry/Wet", nullptr,   0, 0, 127);  // 0 = fully dry
                addFx ("fx" + std::to_string (s) + "_param1",  "FX" + std::to_string (s) + " Param 1", nullptr,   0, 0, 127);
                addFx ("fx" + std::to_string (s) + "_param2",  "FX" + std::to_string (s) + " Param 2", nullptr,   0, 0, 127);
                addFx ("fx" + std::to_string (s) + "_param3",  "FX" + std::to_string (s) + " Param 3", nullptr,   0, 0, 127);
                addFx ("fx" + std::to_string (s) + "_param4",  "FX" + std::to_string (s) + " Param 4", nullptr,   0, 0, 127);
                addFx ("fx" + std::to_string (s) + "_param5",  "FX" + std::to_string (s) + " Param 5", nullptr,   0, 0, 127);
            }
            addFx ("fx_topo", "FX Topology", &kFxTopologies, 0);   // Series
            addFx ("fx_order", "FX Order",    nullptr,        0, 0, 5);   // orderIdx 0..5
            // Master section (engine-state v3): global chain wet/dry + 3-band
            // master EQ. Tail retention is now unconditional (no user control).
            // Defaults preserve prior audio (mix fully wet, EQ unity/no-cut) so
            // existing patches unchanged.
            addFx ("fx_mix",     "FX Mix",     nullptr, 127, 0, 127);  // global wet/dry (127 = fully wet)
            addFx ("fx_eq_low",  "FX EQ Low",  nullptr,   0, 0, 127);  // low-cut (high-pass)
            addFx ("fx_eq_mid",  "FX EQ Mid",  nullptr,  64, 0, 127);  // mid peaking gain (64 = 0 dB)
            addFx ("fx_eq_high", "FX EQ High", nullptr,  64, 0, 127);  // high-shelf gain (64 = 0 dB)
            for (int m = 1; m <= 16; ++m)
            {
                addFx ("fxmod" + std::to_string (m) + "_source", "FX Mod " + std::to_string (m) + " Src",    &kFxModSources, 0);
                addFx ("fxmod" + std::to_string (m) + "_dest",   "FX Mod " + std::to_string (m) + " Dest",   &kFxDests,      0);
                addFx ("fxmod" + std::to_string (m) + "_amount", "FX Mod " + std::to_string (m) + " Amount", nullptr,        0, -63, 63);
            }
        }

        return d;
    }();

    return table;
}

//==============================================================================
namespace
{
// ---- Host parameter GROUPING (VST3 Units / AU grouped parameter lists) ------
// Mirrors the editor's Section rules (sectionForId, PluginEditor.cpp) adapted
// to the descriptor table's emission order. Groups are created lazily in
// FIRST-APPEARANCE order, so the flattened host parameter list stays as close
// to the historical descriptor order as the grouping permits (see the note in
// createParvatiParameterLayout for the exact deltas).
enum class HostGroup { Osc, Mix, Filter, Env, Lfo, Mod, Modif, Part, Seq, Arp, Global, Fx, FxMod };

const char* hostGroupId (HostGroup g)
{
    switch (g)
    {
        case HostGroup::Osc:    return "osc";
        case HostGroup::Mix:    return "mix";
        case HostGroup::Filter: return "filter";
        case HostGroup::Env:    return "env";
        case HostGroup::Lfo:    return "lfo";
        case HostGroup::Mod:    return "mod";
        case HostGroup::Modif:  return "modif";
        case HostGroup::Part:   return "part";
        case HostGroup::Seq:    return "seq";
        case HostGroup::Arp:    return "arp";
        case HostGroup::Global: return "global";
        case HostGroup::Fx:     return "fx";
        case HostGroup::FxMod:  return "fxmod";
    }
    return "global";   // unreachable; keeps -Wreturn-type calm
}

const char* hostGroupName (HostGroup g)
{
    switch (g)
    {
        case HostGroup::Osc:    return "Oscillators";
        case HostGroup::Mix:    return "Mixer";
        case HostGroup::Filter: return "Filter";
        case HostGroup::Env:    return "Envelopes";
        case HostGroup::Lfo:    return "LFOs";
        case HostGroup::Mod:    return "Mod Matrix";
        case HostGroup::Modif:  return "Modifiers";
        case HostGroup::Part:   return "Part";
        case HostGroup::Seq:    return "Sequencer";
        case HostGroup::Arp:    return "Arpeggiator";
        case HostGroup::Global: return "Global";
        case HostGroup::Fx:     return "FX";
        case HostGroup::FxMod:  return "FX Mod";
    }
    return "Global";
}

// Prefix rules. ORDER MATTERS, mirroring sectionForId:
//   - the global synth options are exact-id matches BEFORE the "filter" prefix
//     (filter_card / filter_drive are global voice-card options, not Filter);
//   - "fxmod" before "fx" and "modif" before "mod" (those ids share prefixes);
//   - env{i}_lfo_* splits into LFOs, the env ADSR quadruple stays Envelopes.
HostGroup hostGroupForId (const juce::String& id)
{
    if (id == "filter_card" || id == "filter_drive" || id == "vca_curve")
        return HostGroup::Global;
    if (id.startsWith ("fxmod")) return HostGroup::FxMod;
    if (id.startsWith ("fx"))    return HostGroup::Fx;
    if (id.startsWith ("modif")) return HostGroup::Modif;
    if (id.startsWith ("mod"))   return HostGroup::Mod;
    if (id.startsWith ("voice_lfo")) return HostGroup::Lfo;
    if (id.startsWith ("env"))
        return id.contains ("_lfo_") ? HostGroup::Lfo : HostGroup::Env;
    if (id.startsWith ("arp"))    return HostGroup::Arp;
    if (id.startsWith ("seq"))    return HostGroup::Seq;
    if (id.startsWith ("osc"))    return HostGroup::Osc;
    if (id.startsWith ("mix"))    return HostGroup::Mix;
    if (id.startsWith ("filter")) return HostGroup::Filter;
    if (id.startsWith ("part"))   return HostGroup::Part;
    return HostGroup::Global;
}

// ---- Shared percent readouts (mirror the UI knob readouts) ------------------
juce::String unsignedPctOf (double v, double max)
{
    return juce::String (juce::roundToInt (juce::jlimit (0.0, 100.0, v / max * 100.0))) + "%";
}

// Signed mod amount -63..+63 -> "+100%" / "0%" / "-50%": the shared
// ui/FormatHelpers.h formatter (same math the UI's mod-matrix / FX-mod amount
// readouts use — pinned equal by paramhelp_parity_test).

// "fx{1..3}_param{1..5}" -> true + slot/paramIdx (1-based). @p paramIdx is the
// UI's generic param index (paramLabel/paramValueText idx 0..4 = paramIdx-1).
// Thin wrapper over the ONE shared FX id decoder (parseFxParamId).
bool parseFxSlotParam (const juce::String& id, int& slot, int& paramIdx)
{
    const FxParamId fx = parseFxParamId (id);
    if (fx.kind != FxParamId::SlotParam)
        return false;
    slot = fx.slot + 1;
    paramIdx = fx.paramIdx + 1;
    return true;
}
}  // namespace

//==========================================================================
// The ONE FX paramID decoder (see ParameterLayout.h). Splits the id into
// kind + indices. Every caller owns its range clamps.
FxParamId parseFxParamId (const juce::String& id)
{
    FxParamId r;
    // Per-slot ids: fx{1..3}_type/enabled/drywet/param{1..5}. The id[2]
    // digit check keeps fx_topo / fx_order / fx_mix / fx_eq_* / fxmod* out.
    if (id.length() >= 4 && id[0] == 'f' && id[1] == 'x' && id[2] >= '1' && id[2] <= '3' && id[3] == '_')
    {
        const int slot = id[2] - '1';
        const juce::String suffix = id.substring (4);
        if      (suffix == "type")    { r.kind = FxParamId::SlotType;    r.slot = slot; return r; }
        else if (suffix == "enabled") { r.kind = FxParamId::SlotEnabled; r.slot = slot; return r; }
        else if (suffix == "drywet")  { r.kind = FxParamId::SlotDryWet;  r.slot = slot; return r; }
        else if (suffix.startsWith ("param"))
        {
            const int k = suffix.substring (5).getIntValue();
            if (k >= 1 && k <= kNumFxSlotParams)
            {
                r.kind = FxParamId::SlotParam;
                r.slot = slot;
                r.paramIdx = k - 1;
            }
            return r;
        }
        return r;
    }
    // Scalar FX ids. The slot branch above rejects them: id[2] is not a digit.
    if      (id == "fx_topo")    { r.kind = FxParamId::Topology; return r; }
    else if (id == "fx_order")   { r.kind = FxParamId::Order;    return r; }
    else if (id == "fx_mix")     { r.kind = FxParamId::Mix;      return r; }
    else if (id == "fx_eq_low")  { r.kind = FxParamId::EqLow;    return r; }
    else if (id == "fx_eq_mid")  { r.kind = FxParamId::EqMid;    return r; }
    else if (id == "fx_eq_high") { r.kind = FxParamId::EqHigh;   return r; }
    // FX mod matrix ids: fxmod{1..16}_source/_dest/_amount.
    else if (id.startsWith ("fxmod") && id.contains ("_"))
    {
        const int under = id.indexOf ("_");   // first '_' after "fxmod{m}"
        const int m = id.substring (5, under).getIntValue();
        if (m >= 1 && m <= kNumFxMatrixSlots)
        {
            const juce::String field = id.substring (under + 1);
            if      (field == "source") { r.kind = FxParamId::ModSource; r.slot = m - 1; }
            else if (field == "dest")   { r.kind = FxParamId::ModDest;   r.slot = m - 1; }
            else if (field == "amount") { r.kind = FxParamId::ModAmount; r.slot = m - 1; }
        }
    }
    return r;
}

juce::AudioProcessorValueTreeState::ParameterLayout createParvatiParameterLayout()
{
    // ---- Host-facing text + grouping ------------------------------------------
    // (1) Every AudioParameterInt carries a value->text formatter so host
    //     automation lanes / generic editors show the same meaningful-unit
    //     readout as the Parvati UI (Hz / ms / semitones / cents / % / note
    //     names) instead of a raw 0..127 integer, plus a text->value parser so
    //     hosts with typed parameter entry (Cubase / Bitwig) map typed values
    //     through the DISPLAYED unit (typing "100" into Dry/Wet = 100% = 127).
    //     Choice parameters already carry their text via the choice list and
    //     are untouched. ParameterID { d.paramID, 1 } versioning is unchanged.
    // (2) Parameters are wrapped in AudioProcessorParameterGroups (VST3 Units
    //     / AU grouped lists). Within each group the order is EXACTLY the
    //     descriptor-table order. vs the historical FLAT list, only two spans
    //     change: (a) the 23 env+lfo params (the 9 env{i}_lfo_* / 2 voice_lfo
    //     params now follow the 12 env ADSR params) and (b) the 84-param
    //     part..global span (part_select joins the Part params up front;
    //     vca_curve / filter_card / filter_drive close the span in Global).
    //     Both spans permute the SAME members, so absolute indices are
    //     IDENTICAL outside them: everything through modif4_op (index 97) and
    //     everything from fx1_type onward keep their exact historical
    //     positions. Hosts reference parameters by string/hash ID in ALL
    //     shipped formats (VST3 string ids; AU/AUv3 hashCode of the id —
    //     verified in juce_audio_plugin_client), so saved automation and
    //     APVTS state are unaffected either way.
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // fx{s}_type choice params, stashed as created so the SAME slot's
    // param1..5 formatters can resolve the current effect type. Lifetime: both
    // parameters are owned by the same APVTS (created from this layout), so the
    // captured pointer is valid for exactly as long as the formatter that
    // holds it. AudioParameterChoice::getIndex() is an atomic load — reading it
    // from a host thread is safe (the read may trail the audio thread by a
    // tick; a display-only staleness).
    std::array<juce::AudioParameterChoice*, 3> fxTypeParams {};

    auto intAttributesFor = [&fxTypeParams] (const PatchParamDescriptor& d)
    {
        auto attrs = juce::AudioParameterIntAttributes {};
        const juce::String id { d.paramID };   // copy: the descriptor table outlives us, but stay self-contained
        // Default typed-entry parse: plain integer (identical to
        // AudioParameterInt's built-in default). Range clamping is applied by
        // the parameter's normalisable range (getValueForText -> convertTo0to1).
        juce::AudioParameterIntAttributes::ValueFromString parse =
            [] (const juce::String& t) { return t.getIntValue(); };

        if (d.isFx)
        {
            int slot = 0, paramIdx = 0;
            if (parseFxSlotParam (id, slot, paramIdx))
            {
                // Semantic per-FxType text (e.g. "+12.0 st", "1/16", "6x");
                // falls back to "NN%" for dimensionless params (FxSlotLabels).
                juce::AudioParameterChoice* typeParam = fxTypeParams[(size_t) (slot - 1)];
                attrs = attrs.withStringFromValueFunction (
                    [typeParam, paramIdx] (int v, int)
                    {
                        const int typeIdx = (typeParam != nullptr) ? typeParam->getIndex() : 0;
                        return paramValueText (static_cast<FxType> (typeIdx), paramIdx - 1,
                                               static_cast<double> (v));
                    });
                // Semantic strings ("C4", "1/16") are not generally invertible:
                // typed entry stays raw-integer.
            }
            else if (id.endsWith ("_drywet") || id == "fx_mix")
            {
                attrs = attrs.withStringFromValueFunction (
                    [] (int v, int) { return unsignedPctOf (v, 127.0); });
                parse = [] (const juce::String& t) { return juce::roundToInt (t.getIntValue() * 1.27); };
            }
            else if (id == "fx_eq_low")
            {
                attrs = attrs.withStringFromValueFunction (
                    [] (int v, int) { return fxEqLowToString (static_cast<double> (v)); });
                // "off" / "On"-style entry; Hz strings are not invertible.
                parse = [] (const juce::String& t)
                {
                    if (t.equalsIgnoreCase ("off")) return 0;
                    return t.getIntValue();
                };
            }
            else if (id == "fx_eq_mid" || id == "fx_eq_high")
            {
                attrs = attrs.withStringFromValueFunction (
                    [] (int v, int) { return fxEqDbToString (static_cast<double> (v)); });
                // Typed dB entry ("+6" / "-12") maps through the ±12 dB scale
                // around the unity byte 64.
                parse = [] (const juce::String& t)
                {
                    return 64 + juce::roundToInt (t.getIntValue() * 64.0 / 12.0);
                };
            }
            else if (id.endsWith ("_enabled"))
            {
                attrs = attrs.withStringFromValueFunction (
                    [] (int v, int) { return v == 0 ? juce::String ("Off") : juce::String ("On"); });
                parse = [] (const juce::String& t)
                {
                    if (t.equalsIgnoreCase ("on"))  return 1;
                    if (t.equalsIgnoreCase ("off")) return 0;
                    return t.getIntValue();
                };
            }
            else if (id.startsWith ("fxmod") && id.endsWith ("_amount"))
            {
                attrs = attrs.withStringFromValueFunction (
                    [] (int v, int) { return signedAmountPercent (static_cast<double> (v)); });
                parse = [] (const juce::String& t) { return juce::roundToInt (t.getIntValue() * 0.63); };
            }
            // fx_order: an internal chain-permutation index with no meaningful
            // unit — stays raw (no formatter). Noted in audit/work_host_params.md.
        }
        else
        {
            // Synth descriptors: the existing pure formatter. Unmatched ids
            // fall back to the raw integer inside it, so this is always safe.
            attrs = attrs.withStringFromValueFunction (
                [id] (int v, int) { return paramValueTextSynth (id, static_cast<double> (v)); });
        }

        return attrs.withValueFromStringFunction (parse);
    };

    // Lazily-created groups, emitted in FIRST-APPEARANCE order (see the order
    // note above). Children are added while the group is still local (never
    // after the APVTS owns it — the AudioProcessorParameterGroup contract).
    std::array<std::unique_ptr<juce::AudioProcessorParameterGroup>, 13> groups {};
    std::vector<HostGroup> groupOrder;
    groupOrder.reserve (13);

    auto groupFor = [&groups, &groupOrder] (HostGroup g) -> juce::AudioProcessorParameterGroup*
    {
        auto& slot = groups[static_cast<size_t> (g)];
        if (slot == nullptr)
        {
            slot = std::make_unique<juce::AudioProcessorParameterGroup> (
                hostGroupId (g), hostGroupName (g), " - ");
            groupOrder.push_back (g);
        }
        return slot.get();
    };

    for (const auto& d : getPatchParamDescriptors())
    {
        const juce::String id { d.paramID };
        std::unique_ptr<juce::RangedAudioParameter> param;

        if (d.choices != nullptr)
        {
            auto choice = std::make_unique<juce::AudioParameterChoice> (
                juce::ParameterID { d.paramID, 1 }, d.label, *d.choices, d.defaultValue);
            // Stash fx{N}_type for the slot's param1..5 sibling lookup above.
            if (d.isFx && id.endsWith ("_type"))
            {
                const int slot = id.substring (2).upToFirstOccurrenceOf ("_", false, false).getIntValue();
                if (slot >= 1 && slot <= 3)
                    fxTypeParams[static_cast<size_t> (slot - 1)] = choice.get();
            }
            param = std::move (choice);
        }
        else
        {
            auto attrs = intAttributesFor (d);
            if (d.nonAutomatable)
            {
                // UI-action parameters (part_select): excluded from host
                // automation. Everything else about the parameter is unchanged.
                attrs = attrs.withAutomatable (false);
            }
            param = std::make_unique<juce::AudioParameterInt> (
                juce::ParameterID { d.paramID, 1 }, d.label, d.minValue, d.maxValue, d.defaultValue, attrs);
        }

        groupFor (hostGroupForId (id))->addChild (std::move (param));
    }

    for (const auto g : groupOrder)
        layout.add (std::move (groups[static_cast<size_t> (g)]));

    return layout;
}

uint8_t parvatiValueToPatchByte (const PatchParamDescriptor& d, float rawValue)
{
    if (d.isArp || d.isOption || d.isSequencer || d.isFx)
        return 0;  // arp / option / sequencer / FX params have no patch byte

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

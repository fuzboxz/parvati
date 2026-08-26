// patch_sanitizer — normalize raw Patch/PartData bytes to firmware-valid
// domains at the INGESTION boundary (memory-safety wave 2026-08-22).
//
// WHY: two load paths push file/host bytes straight into the engine's
// patchBytes[112] / partBytes[84] with NO validation (the APVTS round-trip
// paths — .PRO via loadProgramFromBytes, .yml via descriptor decode —
// clamp by construction; these two do not):
//   * .MUL multi loads      (PluginProcessor::loadMultiFile)
//   * host-state blob resto (SynthEngine::restoreState — setStateInformation)
//
// The 2026-08-18 bug hunts clamped the KNOWN array-index sinks inside the DSP
// render paths (mod source/destination, modifier operands, LFO rate,
// portamento, wavetable index) — those clamps STAY (defense in depth; the
// firmware's own controller also validated before its voicecards). This
// module is the single normalize-once layer so every sink — including
// yet-undiscovered ones — receives in-domain bytes, exactly like the
// firmware's controller guaranteed for its own UI-generated data.
//
// FIDELITY: real firmware files only ever contain in-domain bytes (the
// hardware's parameter layer clamped the same way), so sanitizing is the
// IDENTITY for legitimate files: saves/loads round-trip byte-exactly. Only
// hand-edited/corrupted/hostile blobs are (deterministically) narrowed.
//
// All bounds derive from the patch.h enums/constants, so they cannot drift
// from the DSP definitions.

#ifndef HELLCAT_DSP_PATCH_SANITIZER_H_
#define HELLCAT_DSP_PATCH_SANITIZER_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "dsp/patch.h"
#include "TuningTables.h"   // kNumTuningPresets (dependency-free)
// ArpSeqField bounds pin against the DSP tables they index (kArpPatterns,
// kMidiClockTickPerStep). Both live in Arpeggiator.h / dsp/constants.h and
// carry no JUCE dependency, so this sanitizer shard stays dependency-light.
#include "Arpeggiator.h"

namespace ambika::dsp {

// ---------------------------------------------------------------------------
// Patch (112 bytes) — field-wise on the struct (no byte offsets to drift).
// Returns the number of bytes narrowed (0 for a valid patch).
// ---------------------------------------------------------------------------

// Valid LFO rate domain: 15 tempo-synced rates + 128 free-running entries in
// lut_res_lfo_increments (see voice.cpp LfoRateToIncrement, which clamps the
// same way at the sink).
constexpr uint8_t kMaxLfoRate = kNumSyncedLfoRates + 127;   // 142

inline uint8_t clampU8(uint8_t v, uint8_t lo, uint8_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline size_t sanitizePatch(Patch& p) {
    size_t fixed = 0;
    auto byteFixed = [&fixed](uint8_t before, uint8_t after) {
        if (before != after) ++fixed;
        return after;
    };

    // osc[2]: shape indexes the 22-entry render dispatch + the wavetable
    // definition region; parameter/range/detune are plain gain/pitch math.
    for (auto& osc : p.osc) {
        osc.shape = byteFixed(osc.shape, clampU8(osc.shape, 0,
                    static_cast<uint8_t>(WAVEFORM_WAVEQUENCE)));
    }

    // mix: op selects the mixer algorithm (switch has a safe default, but
    // normalize to a real op); sub_osc_shape selects the sub-osc / transient
    // generator algorithms (WAVEFORM_SUB_OSC_LAST entries).
    p.mix_op = byteFixed(p.mix_op, clampU8(p.mix_op, 0,
                static_cast<uint8_t>(OP_LAST - 1)));
    p.mix_sub_osc_shape = byteFixed(p.mix_sub_osc_shape, clampU8(
                p.mix_sub_osc_shape, 0,
                static_cast<uint8_t>(WAVEFORM_SUB_OSC_LAST - 1)));

    // filter: mode selects LP/BP/HP/NOTCH (AnalogFilter::setMode also clamps).
    for (auto& f : p.filter) {
        f.mode = byteFixed(f.mode, clampU8(f.mode, 0,
                  static_cast<uint8_t>(FILTER_MODE_NOTCH)));
    }

    // env_lfo[3]: attack/decay/release index the 128-entry portamento/expo
    // LUT via Envelope::Update (voice.cpp clips the MODDED value; the raw
    // byte domain is 0..127); sustain feeds a <<1 target (0..127 => 0..254,
    // no 8-bit truncation); shape indexes Lfo::Render's switch (the wavetable
    // branch is compiled out on the voicecard, so >RAMP hits the safe
    // default — normalize to a real controller shape); rate indexes
    // lut_res_lfo_increments via kMaxLfoRate.
    for (auto& e : p.env_lfo) {
        e.attack  = byteFixed(e.attack,  clampU8(e.attack, 0, 127));
        e.decay   = byteFixed(e.decay,   clampU8(e.decay, 0, 127));
        e.sustain = byteFixed(e.sustain, clampU8(e.sustain, 0, 127));
        e.release = byteFixed(e.release, clampU8(e.release, 0, 127));
        e.shape   = byteFixed(e.shape,   clampU8(e.shape, 0,
                     static_cast<uint8_t>(LFO_WAVEFORM_LAST - 1)));
        e.rate    = byteFixed(e.rate,    clampU8(e.rate, 0, kMaxLfoRate));
    }

    // Voice LFO: the voicecard renders only TRIANGLE/SQUARE/S_H/RAMP
    // (controller parameter.cc clamps the shape to 0..RAMP — see lfo.h).
    p.voice_lfo_shape = byteFixed(p.voice_lfo_shape,
        clampU8(p.voice_lfo_shape, 0, static_cast<uint8_t>(LFO_WAVEFORM_RAMP)));
    p.voice_lfo_rate = byteFixed(p.voice_lfo_rate,
        clampU8(p.voice_lfo_rate, 0, kMaxLfoRate));

    // modulation[14]: source indexes modulation_sources_[31], destination
    // indexes dst_[19] (the 2026-08-18 OOB-WRITE class); amount is signed
    // gain math (free).
    for (auto& m : p.modulation) {
        m.source = byteFixed(m.source, clampU8(m.source, 0,
             static_cast<uint8_t>(MOD_SRC_LAST - 1)));
        m.destination = byteFixed(m.destination, clampU8(m.destination, 0,
             static_cast<uint8_t>(MOD_DST_LAST - 1)));
    }

    // modifier[4]: operands index modulation_sources_[31]; op selects the
    // modifier algorithm (MODIFIER_LAST entries).
    for (auto& mod : p.modifier) {
        mod.operands[0] = byteFixed(mod.operands[0], clampU8(mod.operands[0],
            0, static_cast<uint8_t>(MOD_SRC_LAST - 1)));
        mod.operands[1] = byteFixed(mod.operands[1], clampU8(mod.operands[1],
            0, static_cast<uint8_t>(MOD_SRC_LAST - 1)));
        mod.op = byteFixed(mod.op, clampU8(mod.op, 0,
            static_cast<uint8_t>(MODIFIER_LAST - 1)));
    }

    // Deliberately FREE (plain gain/pitch/flag math, no array sink):
    // osc.parameter, osc.range, osc.detune (int8), mix_balance/parameter/
    // sub_osc/noise/fuzz/crush, filter.cutoff/resonance, filter_env/filter_lfo
    // (int8), env_lfo.padding/retrigger, modulation.amount (int8), padding[8].

    return fixed;
}

// ---------------------------------------------------------------------------
// PartData (84 bytes, controller-side) — byte-wise with named offsets. The
// map (mirrors stageArpSeqFromPartBytes in SynthEngine.cpp, which clamps the
// SAME bytes at consume time, and the polyphony service at SynthEngine.cpp
// ~line 1770):
//   [0]      part volume            (free)
//   [1..3]   padding                (free)
//   [4]      tuning/raga preset id  (0..kNumTuningPresets; TuningTables also
//                                   guards the id at its own sink)
//   [5]      legato                 (free; bool-ish)
//   [6]      portamento_time        (0..127: 128-entry LUT — the
//                                   F-static-2 class)
//   [7]      arp mode               (0..2: Off/Arp/Sequencer)
//   [8]      arp direction          (0..5)
//   [9]      arp octave             (1..4: 0 hung the Random wrap loop)
//   [10]     arp pattern            (0..21: kArpPatterns has 22 entries)
//   [11]     arp resolution         (0..14: kMidiClockTickPerStep has 15)
//   [12..14] seq lengths 1..3       (1..16 each; 0 wedges the wrap logic)
//   [15]     polyphony mode         (0..4)
//   [16..79] sequencer step data    (free: every byte is a valid step value)
//   [80..83] unused/reserved        (free)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Arp/seq PartData byte domains — the ONE source for every clamp of these
// bytes (sanitizePartData here, SynthEngine::stageArpSeqFromPartBytes at the
// consume boundary, and the descriptor byte offsets in ParameterLayout). The
// firmware parameter layer guaranteed the same domains; raw-file loaders and
// this sanitizer re-assert them.
// ---------------------------------------------------------------------------
enum class ArpSeqField : uint8_t
{
    ArpMode = 0,
    ArpDirection,
    ArpOctave,
    ArpPattern,
    ArpResolution,
    SeqLength1,
    SeqLength2,
    SeqLength3
};

struct ArpSeqDomain
{
    ArpSeqField field;          // which pendingConfig_ / PartData field
    uint8_t     partDataOffset; // byte offset inside PartData[84]
    uint8_t     lo;             // firmware-valid lower bound (inclusive)
    uint8_t     hi;             // firmware-valid upper bound (inclusive)
};

inline constexpr ArpSeqDomain kArpSeqDomains[] = {
    { ArpSeqField::ArpMode,        7, 0, 2 },
    { ArpSeqField::ArpDirection,   8, 0, 5 },
    { ArpSeqField::ArpOctave,      9, 1, 4 },
    { ArpSeqField::ArpPattern,    10, 0, 21 },
    { ArpSeqField::ArpResolution, 11, 0, 14 },
    { ArpSeqField::SeqLength1,    12, 1, 16 },
    { ArpSeqField::SeqLength2,    13, 1, 16 },
    { ArpSeqField::SeqLength3,    14, 1, 16 },
};

constexpr const ArpSeqDomain& arpSeqDomain (ArpSeqField f)
{
    for (const ArpSeqDomain& d : kArpSeqDomains)
        if (d.field == f)
            return d;
    return kArpSeqDomains[0];   // unreachable for the enum's real values
}

// The pattern and resolution rows index real DSP tables: pin their bounds
// against the table sizes so a table edit fails at compile time.
static_assert (arpSeqDomain (ArpSeqField::ArpPattern).hi + 1
                   == sizeof (::hellcat::kArpPatterns) / sizeof (::hellcat::kArpPatterns[0]),
               "arp pattern domain drifted from kArpPatterns (22 entries)");
static_assert (arpSeqDomain (ArpSeqField::ArpResolution).hi + 1
                   == sizeof (::hellcat::kMidiClockTickPerStep) / sizeof (::hellcat::kMidiClockTickPerStep[0]),
               "arp resolution domain drifted from kMidiClockTickPerStep (15 entries)");

// Sized so the array type itself enforces the PartData length at compile time.
inline size_t sanitizePartData(std::array<uint8_t, 84>& part) {
    size_t fixed = 0;
    auto clampByte = [&fixed](uint8_t& b, uint8_t lo, uint8_t hi) {
        const uint8_t v = clampU8(b, lo, hi);
        if (v != b) { b = v; ++fixed; }
    };

    // Tuning preset byte ([4]): 0 = 12-EDO, 1..kNumTuningPresets = raga ids
    // (HellcatPreset's legacy tuning_mode mapping writes exactly this domain).
    // Bound derived from the real constant so the two cannot drift; the
    // TuningTables id guard remains the sink-side check either way.
    static_assert(hellcat::kNumTuningPresets == 32,
                  "PartData[4] tuning domain drifted — update sanitizePartData");
    clampByte(part[4], 0, static_cast<uint8_t> (hellcat::kNumTuningPresets));

    // Portamento ([6]): the voicecard's lut_res_env_portamento_increments
    // domain (F-static-2; voice.cpp also clamps at the sink).
    clampByte(part[6], 0, 127);

    // Arp block + sequencer lengths ([7..14]) — bounds come from the single
    // kArpSeqDomains table (the same rows stageArpSeqFromPartBytes applies).
    for (const ArpSeqDomain& dom : kArpSeqDomains)
        clampByte (part[dom.partDataOffset], dom.lo, dom.hi);

    // Polyphony mode ([15]) — 0..4 (audio-thread service also clamps).
    clampByte(part[15], 0, 4);

    // [0..3], [5], [16..83]: free (see the map above).

    return fixed;
}

}  // namespace ambika::dsp

#endif  // HELLCAT_DSP_PATCH_SANITIZER_H_

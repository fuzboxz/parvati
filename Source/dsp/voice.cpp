// Faithful C++17 port of Ambika's `voicecard/voice.cc` (the synthesis engine:
// LoadSources / ProcessModulationMatrix / UpdateDestinations / RenderOscillators
// and the ProcessBlock signal flow).
//
// Original: ambika_reference/voicecard/voice.cc (Emilie Gillet, GPL3).
//
// Everything here is integer arithmetic matching the firmware bit-for-bit.
// The only structural changes are: (1) instance vs static-singleton, and
// (2) the rendered block is written to output_[] instead of a global ring.
//
// LFO_1/2/3 INTEGRATION NOTE:
//   In the firmware, MOD_SRC_LFO_1/2/3 are rendered by the CONTROLLER
//   (part.cc) and streamed to the voicecard over SPI; the voicecard only
//   renders its own voice LFO (MOD_SRC_LFO_4). This plugin has no controller,
//   so each Voice renders LFO_1/2/3 itself from patch_.env_lfo[i].
//
//   Rate mapping (verified against controller/part.cc:264-269):
//     * rate >= kNumSyncedLfoRates (15)  -> FREE-RUNNING:
//           increment = lut_res_lfo_increments[rate - 15]
//       (This covers the common/default case — the init patch and goldencard
//        factory patches all use free-running rates, e.g. rate = 15+24..48.)
//     * rate < 15                          -> MIDI-CLOCK SYNCED: the host BPM
//       drives a 24-PPQN clock; the cycle length is
//       midi_clock_tick_per_step[rate] ticks (LfoRateToIncrement below —
//       the port of controller/part.cc:264-269).
//
//   Shape mapping: shapes TRIANGLE/SQUARE/S&H/RAMP (0..3) are rendered by the
//   ported Lfo (which, like the voicecard firmware, compiles out the 16
//   wavetable-LFO shapes — see lfo.h / spec B.5). Full controller-style
//   wavetable LFO shapes (4..19) are a documented follow-up.

#include "dsp/voice.h"

#include <algorithm>  // std::min (raw-byte LUT clamps)
#include <cstring>  // memset

#include "dsp/fixed_math.h"
#include "dsp/fixed_types.h"
#include "dsp/random.h"
#include "dsp/resources/resources.h"
#include "dsp/resources/resources_manager.h"

namespace ambika::dsp {

// ---------------------------------------------------------------------------
// Init patch — identical to the firmware `init_patch` (voice.cc:58-117).
// ---------------------------------------------------------------------------
static const Patch kInitPatch = {
    // Oscillators (osc[2])
    { { WAVEFORM_NONE, 0, 0, 0 },
      { WAVEFORM_NONE, 0, 0, 0 } },

    // Mixer
    /* mix_balance */ 32,
    /* mix_op */       OP_SUM,
    /* mix_parameter */0,
    /* mix_sub_osc_shape */ WAVEFORM_SUB_OSC_SQUARE_1,
    /* mix_sub_osc */  0,
    /* mix_noise */    0,
    /* mix_fuzz */     0,
    /* mix_crush */    0,

    // Filter (filter[2] + filter_env + filter_lfo)
    { { 127, 0, 0 }, { 0, 0, 0 } },
    /* filter_env */ 63,
    /* filter_lfo */ 0,

    // env_lfo[3] {attack,decay,sustain,release,shape,rate,padding,retrigger}
    { { 0, 40, 20, 60, 0, 0, 0, 0 },
      { 0, 40, 20, 60, 0, 0, 0, 0 },
      { 0, 40, 20, 60, 0, 0, 0, 0 } },

    // voice LFO
    /* voice_lfo_shape */ LFO_WAVEFORM_TRIANGLE,
    /* voice_lfo_rate */  16,

    // modulation[14] {source, destination, amount}
    {
        { MOD_SRC_LFO_1,      MOD_DST_OSC_1,          0 },
        { MOD_SRC_ENV_1,      MOD_DST_OSC_2,          0 },
        { MOD_SRC_LFO_1,      MOD_DST_OSC_1,          0 },
        { MOD_SRC_ENV_1,      MOD_DST_OSC_2,          0 },
        { MOD_SRC_ENV_1,      MOD_DST_PARAMETER_1,    0 },
        { MOD_SRC_LFO_1,      MOD_DST_PARAMETER_2,    0 },
        { MOD_SRC_LFO_2,      MOD_DST_MIX_BALANCE,    0 },
        { MOD_SRC_LFO_4,      MOD_DST_PARAMETER_1,    63 },
        { MOD_SRC_SEQ_1,      MOD_DST_PARAMETER_1,    0 },
        { MOD_SRC_SEQ_2,      MOD_DST_PARAMETER_2,    0 },
        { MOD_SRC_ENV_2,      MOD_DST_VCA,            32 },
        { MOD_SRC_VELOCITY,   MOD_DST_VCA,            0 },
        { MOD_SRC_PITCH_BEND, MOD_DST_OSC_1_2_COARSE, 0 },
        { MOD_SRC_LFO_1,      MOD_DST_OSC_1_2_COARSE, 0 },
    },

    // modifier[4] {operands[2], op}
    { { { 0, 0 }, 0 },
      { { 0, 0 }, 0 },
      { { 0, 0 }, 0 },
      { { 0, 0 }, 0 } },

    // padding[8]
    { 0, 0, 0, 0, 0, 0, 0, 0 },
};

// Index helper for the LFO rate→increment map (see file header note).
// `bpm` only affects the tempo-synced path (rate < kNumSyncedLfoRates).
static inline uint16_t LfoRateToIncrement(uint8_t rate, double bpm) {
    // Raw-file bytes are NOT validated upstream (a .MUL / host-state blob can
    // carry any byte): the free-running branch indexes the 128-entry LUT with
    // rate - kNumSyncedLfoRates, so clamp first (bug hunt 2026-08-18,
    // F-static-1). Valid free-running rates are kNumSyncedLfoRates..
    // kNumSyncedLfoRates+127 (=142); 143..255 clamp to 142 (max rate), which
    // the byte-range APVTS path already guarantees for GUI/host writes.
    rate = std::min (rate, static_cast<uint8_t> (kNumSyncedLfoRates + 127));
    if (rate >= kNumSyncedLfoRates) {
        return lut_res_lfo_increments[rate - kNumSyncedLfoRates];
    }
    // Tempo-synced: cycle length in 24-PPQN MIDI-clock ticks. Each Lfo Render()
    // advances by one kAudioBlockSize (40) of internal audio, so the 16-bit
    // phase advances by freq * 40 / kInternalSampleRate per call.
    const double ticksPerSec = 24.0 * bpm / 60.0;
    const double freqHz = ticksPerSec /
                          static_cast<double>(midi_clock_tick_per_step[rate]);
    const double inc = freqHz * 65536.0 *
                       static_cast<double>(kAudioBlockSize) / kInternalSampleRate;
    return inc >= 65535.0 ? 65535 : static_cast<uint16_t>(inc);
}

void Voice::Init() {
    pitch_value_ = 0;
    // Mix glide resets: the first audible block snaps to the current CVs
    // (exactly the firmware's behaviour), after which CV changes glide.
    mix_glide_ready_ = false;
    for (uint8_t i = 0; i < kNumEnvelopes; ++i) {
        envelope_[i].Init();
    }
    memset(no_sync_, 0, kAudioBlockSize);
    std::memcpy(&patch_, &kInitPatch, sizeof(Patch));
    // Prime the envelope phase increments from the init patch's A/D/S/R. The
    // AmbikaVoice active-state gate skips ProcessBlock for idle voices, so an
    // envelope whose stage_phase_increment_[ATTACK/DECAY/RELEASE] were never set
    // (they default to 0) would freeze on its first Trigger(ATTACK): Trigger
    // reads stage_phase_increment_[ATTACK] at trigger time, and 0 means the
    // attack segment never advances (VCA stuck at 0 => a triggered voice is
    // silent). Update() is normally called every block in UpdateDestinations();
    // this just primes the values at construction so a gated idle voice is
    // trigger-ready. (When a voice later runs ProcessBlock, Update() re-seeds
    // these from the live patch, so this does not affect byte-faithfulness.)
    for (uint8_t i = 0; i < kNumEnvelopes; ++i)
        envelope_[i].Update (patch_.env_lfo[i].attack,
                             patch_.env_lfo[i].decay,
                             patch_.env_lfo[i].sustain,
                             patch_.env_lfo[i].release);
    ResetAllControllers();
    part_.volume = 127;
    part_.portamento_time = 0;
    part_.legato = 0;
    Kill();
}

void Voice::ResetAllControllers() {
    modulation_sources_[MOD_SRC_PITCH_BEND] = 128;
    modulation_sources_[MOD_SRC_AFTERTOUCH] = 0;
    modulation_sources_[MOD_SRC_WHEEL] = 0;
    modulation_sources_[MOD_SRC_WHEEL_2] = 0;
    modulation_sources_[MOD_SRC_EXPRESSION] = 0;
    modulation_sources_[MOD_SRC_CONSTANT_4] = 4;
    modulation_sources_[MOD_SRC_CONSTANT_8] = 8;
    modulation_sources_[MOD_SRC_CONSTANT_16] = 16;
    modulation_sources_[MOD_SRC_CONSTANT_32] = 32;
    modulation_sources_[MOD_SRC_CONSTANT_64] = 64;
    modulation_sources_[MOD_SRC_CONSTANT_128] = 128;
    modulation_sources_[MOD_SRC_CONSTANT_256] = 255;
}

void Voice::TriggerEnvelope(uint8_t stage) {
    for (uint8_t i = 0; i < kNumEnvelopes; ++i) {
        TriggerEnvelope(i, stage);
    }
}

void Voice::TriggerEnvelope(uint8_t index, uint8_t stage) {
    // Clamped like every other fixed-array index (memory-safety migration);
    // Envelope::Trigger itself sinks an invalid stage to DEAD.
    envelope_[index < kNumEnvelopes ? index : kNumEnvelopes - 1].Trigger(stage);
}

void Voice::Trigger(uint16_t note, uint8_t velocity, uint8_t legato) {
    pitch_target_ = static_cast<int16_t>(note);
    if (!part_.legato || !legato) {
        gate_ = 255;
        TriggerEnvelope(ATTACK);
        transient_generator.Trigger();
        modulation_sources_[MOD_SRC_VELOCITY] = velocity;
        modulation_sources_[MOD_SRC_RANDOM] = random().state_msb();
        osc_2.Reset();
        RetriggerLfos();   // SLAVE-mode LFOs reset to phase 0 on a new note.
    }
    if (pitch_value_ == 0 || (part_.legato && !legato)) {
        pitch_value_ = pitch_target_;
    }
    int16_t delta = pitch_target_ - pitch_value_;
    // Same raw-byte clamp as LfoRateToIncrement (bug hunt 2026-08-18,
    // F-static-2): part_.portamento_time comes straight from a PartData byte
    // that is not validated on the .MUL / state-restore path, and the LUT has
    // 128 entries (valid 0..127). 128..255 clamp to 127 (longest glide).
    const uint8_t portamentoClamped = std::min (part_.portamento_time, static_cast<uint8_t> (127));
    int32_t increment = ResourcesManager::Lookup<uint16_t, uint8_t>(
        lut_res_env_portamento_increments, portamentoClamped);
    pitch_increment_ = static_cast<int16_t>((static_cast<int32_t>(delta) * increment) >> 16);
    if (pitch_increment_ == 0) {
        pitch_increment_ = (delta < 0) ? -1 : 1;
    }
}

void Voice::RetriggerLfos() {
    // Mirrors firmware Part::RetriggerLfos: only LFOs whose retrigger_mode is
    // SLAVE reset their phase on note-on. The voice LFO (LFO_4) is NOT touched.
    for (uint8_t i = 0; i < kNumLfos; ++i) {
        if (patch_.env_lfo[i].retrigger_mode == LFO_SYNC_MODE_SLAVE)
            lfo_[i].set_phase(0);
    }
}

void Voice::Release() {
    gate_ = 0;
    TriggerEnvelope(RELEASE);
}

// ---------------------------------------------------------------------------
// Stage 1: render modulation sources (env1/2/3, the 4 LFOs, noise, note, gate)
// and apply the 4 modifiers; seed the modulation-destination baselines.
// ---------------------------------------------------------------------------
void Voice::LoadSources() {
    uint8_t ops[9];  // per-voice scratch (was `static` in firmware)

    // Rescale each modulation source. Envelopes are in 0..254 (>>8 of a 16-bit
    // accumulator); pitch is 14-bit. All scaled to 0..255.
    modulation_sources_[MOD_SRC_NOISE] = random().GetByte();
    modulation_sources_[MOD_SRC_ENV_1] = envelope_[0].Render();
    modulation_sources_[MOD_SRC_ENV_2] = envelope_[1].Render();
    modulation_sources_[MOD_SRC_ENV_3] = envelope_[2].Render();
    modulation_sources_[MOD_SRC_NOTE] = U14ShiftRight6(static_cast<uint16_t>(pitch_value_));
    modulation_sources_[MOD_SRC_GATE] = gate_;

    // LFOs 1/2/3: rendered in-engine from patch_.env_lfo[i] (no controller).
    // phase increment is refreshed in UpdateDestinations(); render with shape.
    for (uint8_t i = 0; i < kNumLfos; ++i) {
        modulation_sources_[MOD_SRC_LFO_1 + i] = lfo_[i].Render(patch_.env_lfo[i].shape);
    }
    // Voice LFO (MOD_SRC_LFO_4).
    modulation_sources_[MOD_SRC_LFO_4] = voice_lfo_.Render(patch_.voice_lfo_shape);

    // Apply the modulation operators (modifiers).
    for (uint8_t i = 0; i < kNumModifiers; ++i) {
        if (!patch_.modifier[i].op) {
            continue;
        }
        uint8_t x = patch_.modifier[i].operands[0];
        uint8_t y = patch_.modifier[i].operands[1];
        // Raw-file bytes are unvalidated (bug hunt 2026-08-18, F-eng-1): the
        // operands index modulation_sources_[31]; clamp like the mod matrix
        // below so a crafted .MUL / host-state blob cannot read OOB.
        x = std::min (x, static_cast<uint8_t> (kNumModulationSources - 1));
        y = std::min (y, static_cast<uint8_t> (kNumModulationSources - 1));
        x = modulation_sources_[x];
        y = modulation_sources_[y];
        uint8_t op = patch_.modifier[i].op;
        if (op <= MODIFIER_LE) {
            if (x > y) {
                ops[4] = x;  ops[7] = 255;
                ops[5] = y;  ops[8] = 0;
            } else {
                ops[4] = y;  ops[7] = 0;
                ops[5] = x;  ops[8] = 255;
            }
            ops[1] = static_cast<uint8_t>((x >> 1) + (y >> 1));                       // SUM
            ops[2] = U8U8MulShift8(x, y);                                             // PRODUCT
            ops[3] = static_cast<uint8_t>(S8U8MulShift8(static_cast<int8_t>(x + 128), y) + 128);  // ATTENUATE
            ops[6] = static_cast<uint8_t>(x ^ y);                                      // XOR
            // MAX=ops[4], MIN=ops[5], GE=ops[7], LE=ops[8] set above.
            modulation_sources_[MOD_SRC_OP_1 + i] = ops[op];
        } else if (op == MODIFIER_QUANTIZE) {
            uint8_t mask = 0;
            while (y >>= 1) {
                mask >>= 1;
                mask |= 0x80;
            }
            modulation_sources_[MOD_SRC_OP_1 + i] = static_cast<uint8_t>(x & mask);
        } else if (op == MODIFIER_LAG_PROCESSOR) {
            y >>= 2;
            ++y;
            uint16_t v = U8U8Mul(static_cast<uint8_t>(256 - y), modulation_sources_[MOD_SRC_OP_1 + i]);
            v += U8U8Mul(y, x);
            modulation_sources_[MOD_SRC_OP_1 + i] = static_cast<uint8_t>(v >> 8);
        }
    }

    modulation_destinations_[MOD_DST_VCA] = static_cast<int8_t>(part_.volume << 1);

    // Load and scale to 0..16383 the initial value of each modulated parameter.
    dst_[MOD_DST_OSC_1] = dst_[MOD_DST_OSC_2] = 8192;
    dst_[MOD_DST_OSC_1_2_COARSE] = dst_[MOD_DST_OSC_1_2_FINE] = 8192;
    dst_[MOD_DST_PARAMETER_1] = static_cast<int16_t>(U8U8Mul(patch_.osc[0].parameter, 128));
    dst_[MOD_DST_PARAMETER_2] = static_cast<int16_t>(U8U8Mul(patch_.osc[1].parameter, 128));

    dst_[MOD_DST_MIX_BALANCE] = static_cast<int16_t>(patch_.mix_balance << 8);
    dst_[MOD_DST_MIX_PARAM]   = static_cast<int16_t>(patch_.mix_parameter << 8);
    dst_[MOD_DST_MIX_FUZZ]    = static_cast<int16_t>(patch_.mix_fuzz << 8);
    dst_[MOD_DST_MIX_CRUSH]   = static_cast<int16_t>(patch_.mix_crush << 8);
    dst_[MOD_DST_MIX_NOISE]   = static_cast<int16_t>(patch_.mix_noise << 8);
    dst_[MOD_DST_MIX_SUB_OSC] = static_cast<int16_t>(patch_.mix_sub_osc << 8);

    uint16_t cutoff = U8U8Mul(patch_.filter[0].cutoff, 128);
    dst_[MOD_DST_FILTER_CUTOFF] =
        S16ClipU14(static_cast<int16_t>(cutoff) + pitch_value_ - 8192);
    dst_[MOD_DST_FILTER_RESONANCE] = static_cast<int16_t>(patch_.filter[0].resonance << 8);

    dst_[MOD_DST_ATTACK]  = 8192;
    dst_[MOD_DST_DECAY]   = 8192;
    dst_[MOD_DST_RELEASE] = 8192;
    dst_[MOD_DST_LFO_4]   = static_cast<int16_t>(U8U8Mul(patch_.voice_lfo_rate, 128));
}

// ---------------------------------------------------------------------------
// Stage 2: the 14 modulation routings (with exact AC/DC coupling rules).
// ---------------------------------------------------------------------------
void Voice::ProcessModulationMatrix() {
    for (uint8_t i = 0; i < kNumModulations; ++i) {
        int8_t amount = patch_.modulation[i].amount;

        // The amount of the LAST modulation is scaled by the wheel.
        if (i == kNumModulations - 1) {
            amount = S8U8MulShift8(amount, modulation_sources_[MOD_SRC_WHEEL]);
        }
        uint8_t source = patch_.modulation[i].source;
        uint8_t destination = patch_.modulation[i].destination;
        // Raw-file bytes are unvalidated (bug hunt 2026-08-18, F-eng-1):
        // .MUL loads and host-state restores push patch bytes straight into
        // the voice (no APVTS round-trip — PluginProcessor.cpp:1046 /
        // SynthEngine.cpp:686). `source` indexes modulation_sources_[31] and
        // `destination` indexes dst_[19] — the latter is an OOB WRITE of up
        // to ~510 bytes past the array on the audio thread. Clamp both.
        // (The .PRO path is already clamped via the APVTS range.)
        source      = std::min (source,      static_cast<uint8_t> (kNumModulationSources - 1));
        destination = std::min (destination, static_cast<uint8_t> (kNumModulationDestinations - 1));
        uint8_t source_value = modulation_sources_[source];
        if (destination != MOD_DST_VCA) {
            int16_t modulation = dst_[destination];
            if ((source >= MOD_SRC_LFO_1 && source <= MOD_SRC_LFO_4) ||
                source == MOD_SRC_PITCH_BEND ||
                source == MOD_SRC_NOTE) {
                // AC-coupled sources (128 = no modulation).
                modulation += S8S8Mul(amount, static_cast<int8_t>(source_value + 128));
            } else {
                modulation += S8U8Mul(amount, source_value);
            }
            dst_[destination] = S16ClipU14(modulation);
        } else {
            // The VCA modulation is multiplicative, not additive.
            if (amount < 0) {
                amount = static_cast<int8_t>(-amount);
                source_value = static_cast<uint8_t>(255 - source_value);
            }
            if (amount != 63) {
                source_value = U8Mix(255, source_value, static_cast<uint8_t>(amount << 2));
            }
            modulation_destinations_[MOD_DST_VCA] = static_cast<int8_t>(U8U8MulShift8(
                static_cast<uint8_t>(modulation_destinations_[MOD_DST_VCA]), source_value));
        }
    }
}

// ---------------------------------------------------------------------------
// Stage 3: fold the modulated destinations into per-block CV (filter cutoff/res,
// crush, osc parameters, envelope timings, LFO rates).
// ---------------------------------------------------------------------------
void Voice::UpdateDestinations() {
    // Hardcoded filter modulations: env2 (filter envelope) + lfo2, both AC/DC
    // matched to the firmware (filter_env is int8 * unsigned env2; filter_lfo is
    // int8 * signed (lfo2+128)).
    uint16_t cutoff = static_cast<uint16_t>(dst_[MOD_DST_FILTER_CUTOFF]);
    cutoff = static_cast<uint16_t>(S16ClipU14(
        static_cast<int16_t>(cutoff) +
        S8U8Mul(patch_.filter_env, modulation_sources_[MOD_SRC_ENV_2])));
    cutoff = static_cast<uint16_t>(S16ClipU14(
        static_cast<int16_t>(cutoff) +
        S8S8Mul(patch_.filter_lfo, static_cast<int8_t>(modulation_sources_[MOD_SRC_LFO_2] + 128))));

    modulation_destinations_[MOD_DST_FILTER_CUTOFF] =
        static_cast<int8_t>(U14ShiftRight6(cutoff));
    modulation_destinations_[MOD_DST_FILTER_RESONANCE] =
        static_cast<int8_t>(U14ShiftRight6(static_cast<uint16_t>(dst_[MOD_DST_FILTER_RESONANCE])));
    modulation_destinations_[MOD_DST_MIX_CRUSH] =
        static_cast<int8_t>((dst_[MOD_DST_MIX_CRUSH] >> 8) + 1);

    osc_1.set_parameter(U15ShiftRight7(static_cast<uint16_t>(dst_[MOD_DST_PARAMETER_1])));
    osc_1.set_fm_parameter(static_cast<uint8_t>(patch_.osc[0].range + 36));
    osc_2.set_parameter(U15ShiftRight7(static_cast<uint16_t>(dst_[MOD_DST_PARAMETER_2])));
    osc_2.set_fm_parameter(static_cast<uint8_t>(patch_.osc[1].range + 36));

    int8_t attack_mod  = static_cast<int8_t>(U15ShiftRight7(static_cast<uint16_t>(dst_[MOD_DST_ATTACK]))  - 64);
    int8_t decay_mod   = static_cast<int8_t>(U15ShiftRight7(static_cast<uint16_t>(dst_[MOD_DST_DECAY]))   - 64);
    int8_t release_mod = static_cast<int8_t>(U15ShiftRight7(static_cast<uint16_t>(dst_[MOD_DST_RELEASE])) - 64);
    for (uint8_t i = 0; i < kNumEnvelopes; ++i) {
        int16_t new_attack = patch_.env_lfo[i].attack;
        new_attack = Clip(static_cast<int16_t>(new_attack + attack_mod), 0, 127);
        int16_t new_decay = patch_.env_lfo[i].decay;
        new_decay = Clip(static_cast<int16_t>(new_decay + decay_mod), 0, 127);
        int16_t new_release = patch_.env_lfo[i].release;
        new_release = Clip(static_cast<int16_t>(new_release + release_mod), 0, 127);
        envelope_[i].Update(
            static_cast<uint8_t>(new_attack),
            static_cast<uint8_t>(new_decay),
            patch_.env_lfo[i].sustain,
            static_cast<uint8_t>(new_release));

        // Refresh the in-engine LFO 1/2/3 increments (free-running if rate>=15,
        // tempo-synced if rate<15, locked to the host BPM set via setTempo()).
        lfo_[i].set_phase_increment(
            LfoRateToIncrement(patch_.env_lfo[i].rate, tempo_bpm_));
    }

    // Voice LFO rate (MOD_DST_LFO_4) — index = (value>>6)>>1 == value>>7.
    voice_lfo_.set_phase_increment(
        ResourcesManager::Lookup<uint16_t, uint8_t>(
            lut_res_lfo_increments,
            static_cast<uint8_t>(U14ShiftRight6(static_cast<uint16_t>(dst_[MOD_DST_LFO_4])) >> 1)));
}

// ---------------------------------------------------------------------------
// Stage 4: pitch glide + per-oscillator pitch→increment + render osc1/sub/osc2.
// ---------------------------------------------------------------------------
void Voice::RenderOscillators() {
    // Apply portamento.
    int16_t base_pitch = pitch_value_ + pitch_increment_;
    if ((pitch_increment_ > 0) ^ (base_pitch < pitch_target_)) {
        base_pitch = pitch_target_;
        pitch_increment_ = 0;
    }
    pitch_value_ = base_pitch;

    // -4/+4 semitones (coarse) and -0.5/+0.5 semitones (fine) from bend/vibrato.
    base_pitch += (dst_[MOD_DST_OSC_1_2_COARSE] - 8192) >> 4;
    base_pitch += (dst_[MOD_DST_OSC_1_2_FINE] - 8192) >> 7;

    // Per-voice pitch bend (host pitch wheel / MPE per-note bend): applied
    // DIRECTLY to the oscillator pitch so the bend is always audible regardless
    // of the mod-matrix routing. pitch_bend_offset_ is in 1/128-semitone units
    // (same scale as pitch_value_), set via set_pitch_bend_offset().
    base_pitch += pitch_bend_offset_;

    for (uint8_t i = 0; i < kNumOscillators; ++i) {
        int16_t pitch = base_pitch;
        // -36/+36 semitones by the range controller (except FM, which uses
        // range as an FM parameter instead of a pitch offset).
        if (patch_.osc[i].shape != WAVEFORM_FM) {
            pitch += S8U8Mul(patch_.osc[i].range, 128);
        }
        // -1/+1 semitones by the detune controller.
        pitch += patch_.osc[i].detune;
        // -16/+16 semitones by the routed modulations.
        pitch += (dst_[MOD_DST_OSC_1 + i] - 8192) >> 2;

        if (pitch >= kHighestNote) {
            pitch = kHighestNote;
        }

        // Extract the phase increment from the pitch table.
        int16_t ref_pitch = pitch - kPitchTableStart;
        uint8_t num_shifts = 0;
        while (ref_pitch < 0) {
            ref_pitch += kOctave;
            ++num_shifts;
        }
        uint24_t increment;
        increment.integral = ResourcesManager::Lookup<uint16_t, uint16_t>(
            lut_res_oscillator_increments, static_cast<uint16_t>(ref_pitch >> 1));
        increment.fractional = 0;
        while (num_shifts--) {
            increment = U24ShiftRight(increment);
        }

        int8_t midi_note = static_cast<int8_t>(U15ShiftRight7(static_cast<uint16_t>(pitch)) - 12);
        if (midi_note < 0) {
            midi_note = 0;
        }
        if (i == 0) {
            sub_osc.set_increment(U24ShiftRight(increment));
            osc_1.Render(
                patch_.osc[0].shape,
                static_cast<uint8_t>(midi_note),
                increment,
                no_sync_,
                sync_state_,
                buffer_);
        } else {
            osc_2.Render(
                patch_.osc[1].shape,
                static_cast<uint8_t>(midi_note),
                increment,
                patch_.mix_op == OP_SYNC ? sync_state_ : no_sync_,
                dummy_sync_state_,
                osc2_buffer_);
        }
    }
}

// ---------------------------------------------------------------------------
// ProcessBlock — the full per-block render (40 samples).
// ---------------------------------------------------------------------------
void Voice::ProcessBlock() {
    write_index_ = 0;

    LoadSources();
    ProcessModulationMatrix();
    UpdateDestinations();

    // Skip the oscillator rendering if the VCA output has converged to ~0.
    if (vca() < 2) {
        for (uint8_t i = 0; i < kAudioBlockSize; i += 2) {
            Overwrite2(128, 128);
        }
        return;
    }

    RenderOscillators();
    uint8_t op = patch_.mix_op;
    uint8_t osc_2_gain = U14ShiftRight6(static_cast<uint16_t>(dst_[MOD_DST_MIX_BALANCE]));
    uint8_t wet_gain = U14ShiftRight6(static_cast<uint16_t>(dst_[MOD_DST_MIX_PARAM]));

    // Gain glide ("mix-gain-glide" divergence — see voice.h): the two
    // values above are the firmware's per-block gain latches (voice.cc:
    // 441-442). Interpolate each gain LINEARLY (8.8 fixed point) from its
    // previously APPLIED value to this block's target — pos(i) = start +
    // (target - start) * i / blockSize — and use the glided values inside
    // the mix loops below, so a CV tick slews over the whole block instead
    // of stepping the waveform once: the analog mixer stage does this
    // smoothing in hardware. The interpolation lands EXACTLY on the target
    // (no accumulator residual — an earlier increment-based version
    // oscillated ±1 sub-LSB around targets and cast acc=-1 to gain 255,
    // flipping the crossfade hard over at the extremes; caught by
    // synth_param_coverage_test's balance=0 pitch check). A steady CV has
    // diff == 0, so every sample equals the firmware latch and static
    // patches stay byte-identical to the oracle (the complement gains
    // ~osc_2 / ~wet are derived per sample, preserving the exact pairing).
    const int32_t balance_target = static_cast<int32_t>(osc_2_gain) << 8;
    const int32_t param_target = static_cast<int32_t>(wet_gain) << 8;
    if (mix_glide_ready_) {
        // Ramp from the previously applied gains (recomputed each block, so
        // the glide converges to the target after ONE block of change).
        ;
    } else {
        // First audible block after Init(): snap (firmware behaviour).
        mix_glide_ready_ = true;
        mix_balance_acc_ = balance_target;
        mix_param_acc_ = param_target;
    }
    const int32_t balance_start = mix_balance_acc_;
    const int32_t param_start = mix_param_acc_;
    const int32_t balance_diff = balance_target - balance_start;
    const int32_t param_diff = param_target - param_start;

    // Mix oscillators. (The glide position per sample i: start + diff*i/N;
    // both bounds hold — pos stays within [min(start,target), max(start,target)]
    // ⊂ [0, 255<<8] — so the >>8 gain is always a valid uint8_t.)
    #define PARVATI_MIX_GLIDE(field, i) \
        static_cast<uint8_t> (((field##_start + (field##_diff * static_cast<int32_t> (i)) \
                               / static_cast<int32_t> (kAudioBlockSize)) >> 8))
    switch (op) {
        case OP_RING_MOD:
            for (uint8_t i = 0; i < kAudioBlockSize; ++i) {
                const uint8_t osc_2_glide = PARVATI_MIX_GLIDE (balance, i);
                const uint8_t osc_1_gain = static_cast<uint8_t>(~osc_2_glide);
                const uint8_t wet_glide = PARVATI_MIX_GLIDE (param, i);
                const uint8_t dry_gain = static_cast<uint8_t>(~wet_glide);
                uint8_t mix = U8Mix(buffer_[i], osc2_buffer_[i], osc_1_gain, osc_2_glide);
                uint8_t ring = static_cast<uint8_t>(S8S8MulShift8(
                    static_cast<int8_t>(buffer_[i] + 128),
                    static_cast<int8_t>(osc2_buffer_[i] + 128)) + 128);
                buffer_[i] = U8Mix(mix, ring, dry_gain, wet_glide);
            }
            break;
        case OP_XOR:
            for (uint8_t i = 0; i < kAudioBlockSize; ++i) {
                const uint8_t osc_2_glide = PARVATI_MIX_GLIDE (balance, i);
                const uint8_t osc_1_gain = static_cast<uint8_t>(~osc_2_glide);
                const uint8_t wet_glide = PARVATI_MIX_GLIDE (param, i);
                const uint8_t dry_gain = static_cast<uint8_t>(~wet_glide);
                uint8_t mix = U8Mix(buffer_[i], osc2_buffer_[i], osc_1_gain, osc_2_glide);
                uint8_t xord = static_cast<uint8_t>(buffer_[i] ^ osc2_buffer_[i]);
                buffer_[i] = U8Mix(mix, xord, dry_gain, wet_glide);
            }
            break;
        case OP_FOLD:
            for (uint8_t i = 0; i < kAudioBlockSize; ++i) {
                const uint8_t osc_2_glide = PARVATI_MIX_GLIDE (balance, i);
                const uint8_t osc_1_gain = static_cast<uint8_t>(~osc_2_glide);
                const uint8_t wet_glide = PARVATI_MIX_GLIDE (param, i);
                const uint8_t dry_gain = static_cast<uint8_t>(~wet_glide);
                uint8_t mix = U8Mix(buffer_[i], osc2_buffer_[i], osc_1_gain, osc_2_glide);
                buffer_[i] = U8Mix(mix, static_cast<uint8_t>(mix + 128), dry_gain, wet_glide);
            }
            break;
        case OP_BITS:
            for (uint8_t i = 0; i < kAudioBlockSize; ++i) {
                const uint8_t osc_2_glide = PARVATI_MIX_GLIDE (balance, i);
                const uint8_t osc_1_gain = static_cast<uint8_t>(~osc_2_glide);
                // Same crush transform as the firmware, derived per sample
                // from the glided wet CV (the >>5 quantization is unchanged).
                const uint8_t wet_glide = PARVATI_MIX_GLIDE (param, i);
                const uint8_t crush_mask = static_cast<uint8_t>(255 - ((1 << (wet_glide >> 5)) - 1));
                buffer_[i] = static_cast<uint8_t>(U8Mix(
                    buffer_[i], osc2_buffer_[i], osc_1_gain, osc_2_glide) & crush_mask);
            }
            break;
        default:
            for (uint8_t i = 0; i < kAudioBlockSize; ++i) {
                const uint8_t osc_2_glide = PARVATI_MIX_GLIDE (balance, i);
                const uint8_t osc_1_gain = static_cast<uint8_t>(~osc_2_glide);
                buffer_[i] = U8Mix(buffer_[i], osc2_buffer_[i], osc_1_gain, osc_2_glide);
            }
            break;
    }
    #undef PARVATI_MIX_GLIDE

    // The block has been rendered on the exact ramp; the glide position now
    // IS the target (converged in one block of change — no residual).
    mix_balance_acc_ = balance_target;
    mix_param_acc_ = param_target;

    // Mix-in sub oscillator or transient generator.
    uint8_t sub_gain = U15ShiftRight7(static_cast<uint16_t>(dst_[MOD_DST_MIX_SUB_OSC]));
    if (patch_.mix_sub_osc_shape < WAVEFORM_SUB_OSC_CLICK) {
        sub_osc.Render(patch_.mix_sub_osc_shape, buffer_, sub_gain);
    } else {
        sub_gain <<= 1;
        transient_generator.Render(patch_.mix_sub_osc_shape, buffer_, sub_gain);
    }

    uint8_t noise = random().state_msb();
    uint8_t noise_gain = U15ShiftRight7(static_cast<uint16_t>(dst_[MOD_DST_MIX_NOISE]));
    uint8_t signal_gain = static_cast<uint8_t>(~noise_gain);
    // Fuzz wet/dry — a separate CV from the mix-op pair above (and a
    // wavetable blend rather than a plain gain multiply; not part of the
    // mix-gain-glide divergence — stays firmware block-latched).
    const uint8_t fuzz_wet_gain = U14ShiftRight6(static_cast<uint16_t>(dst_[MOD_DST_MIX_FUZZ]));
    const uint8_t fuzz_dry_gain = static_cast<uint8_t>(~fuzz_wet_gain);

    // Mix with noise and apply distortion (fuzz). Per-block local LCG
    // `noise = noise*73 + 1` (NOT the global Random), matching the firmware.
    for (uint8_t i = 0; i < kAudioBlockSize;) {
        uint8_t signal_noise_a;
        uint8_t signal_noise_b;
        noise = static_cast<uint8_t>((noise * 73) + 1);
        signal_noise_a = U8Mix(buffer_[i++], noise, signal_gain, noise_gain);
        uint8_t a = U8Mix(
            signal_noise_a,
            ResourcesManager::Lookup<uint8_t, uint8_t>(wav_res_distortion, signal_noise_a),
            fuzz_dry_gain, fuzz_wet_gain);

        noise = static_cast<uint8_t>((noise * 73) + 1);
        signal_noise_b = U8Mix(buffer_[i++], noise, signal_gain, noise_gain);
        uint8_t b = U8Mix(
            signal_noise_b,
            ResourcesManager::Lookup<uint8_t, uint8_t>(wav_res_distortion, signal_noise_b),
            fuzz_dry_gain, fuzz_wet_gain);
        Overwrite2(a, b);
    }
}

}  // namespace ambika::dsp

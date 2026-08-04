// Faithful C++17 port of Ambika's `voicecard/oscillator.cc` (15 algorithms).
//
// Original: ambika_reference/voicecard/oscillator.cc (Emilie Gillet, GPL3).
//
// The firmware's UPDATE_PHASE / BEGIN_SAMPLE_LOOP / END_SAMPLE_LOOP macros are
// reproduced here verbatim (with inert `(void)` casts added to silence unused-
// local warnings under -Wextra — they change no behaviour). Keeping the macros
// rather than hand-rewriting each loop minimises the risk of subtle divergence
// in the 24-bit phase / sync-carry bookkeeping.
//
// Width-sensitivity note: AVR `int` is 16 bits, so a few expressions in the
// firmware rely on 16-bit unsigned promotion / wrap (notably the vowel glottal
// reset comparison and the CZ window `~(...)` terms). Those are written here
// with explicit unsigned 16-bit / 32-bit arithmetic so the compiled result
// bit-matches the AVR firmware regardless of host int width.

#include "dsp/oscillator.h"

#include "dsp/constants.h"               // kAudioBlockSize
#include "dsp/fixed_math.h"              // U8Mix, U24AddC, InterpolateSample, ...
#include "dsp/random.h"                  // random()
#include "dsp/resources/resources.h"     // wav_res_* / waveform_table
#include "dsp/resources/resources_manager.h"

namespace ambika::dsp {

namespace {

// Direct (non-interpolated) table read. Firmware ReadSample =
// ResourcesManager::Lookup<uint8_t,uint8_t>(table, phase>>8) == table[phase>>8].
inline uint8_t ReadSample(const uint8_t* table, uint16_t phase) {
    return table[phase >> 8];
}

// Linear blend of two interpolated table reads.
inline uint8_t InterpolateTwoTables(const uint8_t* table_a, const uint8_t* table_b,
                                    uint16_t phase, uint8_t gain_a, uint8_t gain_b) {
    return U8Mix(InterpolateSample(table_a, phase),
                 InterpolateSample(table_b, phase), gain_a, gain_b);
}

}  // namespace

// ---------------------------------------------------------------------------
// Phase / sample-loop macros — faithful copies of oscillator.cc:18-44.
// (void) casts are inert; they only suppress -Wunused-variable for render fns
// that don't touch every local the macro declares.)
// ---------------------------------------------------------------------------

#define UPDATE_PHASE \
    if (*sync_input_++) { \
        phase.integral = 0; \
        phase.fractional = 0; \
    } \
    phase = U24AddC(phase, phase_increment_int); \
    *sync_output_++ = phase.carry;

// Variant using the local sync_input / sync_output copies (faster on AVR; the
// logic is identical to UPDATE_PHASE).
#define UPDATE_PHASE_MORE_REGISTERS \
    if (*sync_input++) { \
        phase.integral = 0; \
        phase.fractional = 0; \
    } \
    phase = U24AddC(phase, phase_increment_int); \
    *sync_output++ = phase.carry;

#define BEGIN_SAMPLE_LOOP \
    uint24c_t phase; \
    uint24_t phase_increment_int; \
    phase_increment_int.integral = phase_increment_.integral; \
    phase_increment_int.fractional = phase_increment_.fractional; \
    phase.integral = phase_.integral; \
    phase.fractional = phase_.fractional; \
    uint8_t size = kAudioBlockSize; \
    uint8_t* sync_input = sync_input_; \
    uint8_t* sync_output = sync_output_; \
    (void) phase_increment_int; (void) sync_input; (void) sync_output; \
    while (size--) {

#define END_SAMPLE_LOOP \
    } \
    phase_.integral = phase.integral; \
    phase_.fractional = phase.fractional;

// ---------------------------------------------------------------------------
// Silence
// ---------------------------------------------------------------------------
void Oscillator::RenderSilence(uint8_t* buffer) {
    uint8_t size = kAudioBlockSize;
    while (size--) {
        *buffer++ = 128;
    }
}

// ---------------------------------------------------------------------------
// Band-limited PWM (writes 2 samples per iteration + manual size--)
// ---------------------------------------------------------------------------
void Oscillator::RenderBandlimitedPwm(uint8_t* buffer) {
    uint8_t balance_index = U8Swap4(note_ /* - 12 play safe with Aliasing */);
    uint8_t gain_2 = balance_index & 0xf0;
    uint8_t gain_1 = static_cast<uint8_t>(~gain_2);

    uint8_t wave_index = balance_index & 0xf;
    const uint8_t* wave_1 = waveform_table[WAV_RES_BANDLIMITED_SAW_1 + wave_index];
    wave_index = U8AddClip(wave_index, 1, kNumZonesHalfSampleRate);
    const uint8_t* wave_2 = waveform_table[WAV_RES_BANDLIMITED_SAW_1 + wave_index];

    uint16_t shift = static_cast<uint16_t>((parameter_ + 128) << 8);
    // For higher pitched notes, simply use 128.
    uint8_t scale = 192 - (parameter_ >> 1);
    if (note_ > 52) {
        scale = U8Mix(scale, 102, static_cast<uint8_t>((note_ - 52) << 2));
        scale = U8Mix(scale, 102, static_cast<uint8_t>((note_ - 52) << 2));
    }
    phase_increment_ = U24ShiftLeft(phase_increment_);
    BEGIN_SAMPLE_LOOP
        phase = U24AddC(phase, phase_increment_int);
        *sync_output_++ = phase.carry;
        *sync_output_++ = 0;
        if (sync_input_[0] || sync_input_[1]) {
            phase.integral = 0;
            phase.fractional = 0;
        }
        sync_input_ += 2;

        uint8_t a = InterpolateTwoTables(wave_1, wave_2, phase.integral, gain_1, gain_2);
        a = U8U8MulShift8(a, scale);
        uint8_t b = InterpolateTwoTables(wave_1, wave_2, phase.integral + shift, gain_1, gain_2);
        b = U8U8MulShift8(b, scale);
        a = a - b + 128;
        *buffer++ = a;
        *buffer++ = a;
        size--;
    END_SAMPLE_LOOP
}

// ---------------------------------------------------------------------------
// Interpolation between two bandlimited waveforms (position from pitch)
// ---------------------------------------------------------------------------
void Oscillator::RenderSimpleWavetable(uint8_t* buffer) {
    uint8_t balance_index = U8Swap4(note_);
    uint8_t gain_2 = balance_index & 0xf0;
    uint8_t gain_1 = static_cast<uint8_t>(~gain_2);
    uint8_t wave_1_index, wave_2_index;
    if (shape_ != WAVEFORM_SINE) {
        uint8_t wave_index = balance_index & 0xf;
        uint8_t base_resource_id = shape_ == WAVEFORM_SAW
            ? WAV_RES_BANDLIMITED_SAW_0
            : (shape_ == WAVEFORM_SQUARE ? WAV_RES_BANDLIMITED_SQUARE_0
                                         : WAV_RES_BANDLIMITED_TRIANGLE_0);
        wave_1_index = base_resource_id + wave_index;
        wave_index = U8AddClip(wave_index, 1, kNumZonesFullSampleRate);
        wave_2_index = base_resource_id + wave_index;
    } else {
        wave_1_index = WAV_RES_SINE;
        wave_2_index = WAV_RES_SINE;
    }
    const uint8_t* wave_1 = waveform_table[wave_1_index];
    const uint8_t* wave_2 = waveform_table[wave_2_index];

    if (shape_ != WAVEFORM_TRIANGLE) {
        BEGIN_SAMPLE_LOOP
            UPDATE_PHASE_MORE_REGISTERS
            uint8_t sample = InterpolateTwoTables(wave_1, wave_2, phase.integral, gain_1, gain_2);
            if (sample < parameter_) {
                sample += parameter_ >> 1;
            }
            *buffer++ = sample;
        END_SAMPLE_LOOP
    } else {
        // The waveshaper for the triangle is different.
        BEGIN_SAMPLE_LOOP
            UPDATE_PHASE_MORE_REGISTERS
            uint8_t sample = InterpolateTwoTables(wave_1, wave_2, phase.integral, gain_1, gain_2);
            if (sample < parameter_) {
                sample = parameter_;
            }
            *buffer++ = sample;
        END_SAMPLE_LOOP
    }
}

// ---------------------------------------------------------------------------
// Casio CZ-like synthesis
// ---------------------------------------------------------------------------
void Oscillator::RenderCzSaw(uint8_t* buffer) {
    BEGIN_SAMPLE_LOOP
        UPDATE_PHASE
        uint8_t phi = phase.integral >> 8;
        uint8_t clipped_phi = phi < 0x20 ? static_cast<uint8_t>(phi << 3) : 0xff;
        // Interpolation causes more aliasing here.
        *buffer++ = ReadSample(wav_res_sine, U8MixU16(phi, clipped_phi, static_cast<uint8_t>(parameter_ << 1)));
    END_SAMPLE_LOOP
}

void Oscillator::RenderCzResoSaw(uint8_t* buffer) {
    uint16_t increment = static_cast<uint16_t>(phase_increment_.integral +
        ((phase_increment_.integral * static_cast<uint32_t>(parameter_)) >> 2));
    uint8_t type = shape_ - WAVEFORM_CZ_SAW_LP;
    uint16_t phase_2 = data_.secondary_phase;
    BEGIN_SAMPLE_LOOP
        UPDATE_PHASE
        if (phase.carry) {
            phase_2 = lut_res_cz_phase_reset[type & 0x03];
        }
        phase_2 += increment;
        uint8_t carrier = ReadSample(wav_res_sine, phase_2);
        uint8_t window = static_cast<uint8_t>(~static_cast<uint8_t>(phase.integral >> 8));
        if (type & 2) {
            *buffer++ = static_cast<uint8_t>(S8U8MulShift8(static_cast<int8_t>(carrier + 128), window) + 128);
        } else {
            *buffer++ = U8U8MulShift8(carrier, window);
        }
    END_SAMPLE_LOOP
    data_.secondary_phase = phase_2;
}

void Oscillator::RenderCzResoPulse(uint8_t* buffer) {
    uint16_t increment = static_cast<uint16_t>(phase_increment_.integral +
        ((phase_increment_.integral * static_cast<uint32_t>(parameter_)) >> 2));
    uint8_t type = shape_ - WAVEFORM_CZ_SAW_LP;
    uint16_t phase_2 = data_.secondary_phase;
    BEGIN_SAMPLE_LOOP
        UPDATE_PHASE
        if (phase.carry) {
            phase_2 = lut_res_cz_phase_reset[type & 0x03];
        }
        phase_2 += increment;
        uint8_t carrier = ReadSample(wav_res_sine, phase_2);
        // window term: faithfully replicated AVR 16-bit-unsigned result.
        // (~(phase.integral - 0x4000) >> 6) on AVR is unsigned-16 then truncated
        // to uint8; the uint32 unsigned computation below yields the same byte.
        uint8_t window = 0;
        if (phase.integral < 0x4000) {
            window = 255;
        } else if (phase.integral < 0x8000) {
            uint32_t d = static_cast<uint32_t>(phase.integral) - 0x4000u;
            window = static_cast<uint8_t>((~d) >> 6);
        }
        if (type == 5) {
            carrier >>= 1;
            carrier += 128;
        }
        if (type & 2) {
            *buffer++ = static_cast<uint8_t>(S8U8MulShift8(static_cast<int8_t>(carrier + 128), window) + 128);
        } else {
            *buffer++ = U8U8MulShift8(carrier, window);
        }
    END_SAMPLE_LOOP
    data_.secondary_phase = phase_2;
}

void Oscillator::RenderCzResoTri(uint8_t* buffer) {
    uint16_t increment = static_cast<uint16_t>(phase_increment_.integral +
        ((phase_increment_.integral * static_cast<uint32_t>(parameter_)) >> 2));
    uint8_t type = shape_ - WAVEFORM_CZ_SAW_LP;
    uint16_t phase_2 = data_.secondary_phase;
    BEGIN_SAMPLE_LOOP
        UPDATE_PHASE
        if (phase.carry) {
            phase_2 = lut_res_cz_phase_reset[type & 0x03];
        }
        phase_2 += increment;
        uint8_t carrier = ReadSample(wav_res_sine, phase_2);
        uint8_t window = (phase.integral & 0x8000)
            ? static_cast<uint8_t>(~static_cast<uint8_t>(phase.integral >> 7))
            : static_cast<uint8_t>(phase.integral >> 7);
        if (type & 2) {
            *buffer++ = static_cast<uint8_t>(S8U8MulShift8(static_cast<int8_t>(carrier + 128), window) + 128);
        } else {
            *buffer++ = U8U8MulShift8(carrier, window);
        }
    END_SAMPLE_LOOP
    data_.secondary_phase = phase_2;
}

// ---------------------------------------------------------------------------
// FM
// ---------------------------------------------------------------------------
void Oscillator::RenderFm(uint8_t* buffer) {
    uint8_t offset = fm_parameter_;
    if (offset < 24) {
        offset = 0;
    } else if (offset > 48) {
        offset = 24;
    } else {
        offset = offset - 24;
    }
    uint16_t multiplier = lut_res_fm_frequency_ratios[offset];
    uint16_t increment =
        static_cast<uint16_t>((static_cast<int32_t>(phase_increment_.integral) * multiplier) >> 8);
    parameter_ <<= 1;

    uint16_t phase_2 = data_.secondary_phase;
    BEGIN_SAMPLE_LOOP
        UPDATE_PHASE
        phase_2 += increment;
        uint8_t modulator = InterpolateSample(wav_res_sine, phase_2);
        uint16_t modulation = static_cast<uint16_t>(modulator * parameter_);
        *buffer++ = InterpolateSample(wav_res_sine, phase.integral + modulation);
    END_SAMPLE_LOOP
    data_.secondary_phase = phase_2;
}

// ---------------------------------------------------------------------------
// 8-bit land
// ---------------------------------------------------------------------------
void Oscillator::Render8BitLand(uint8_t* buffer) {
    BEGIN_SAMPLE_LOOP
        UPDATE_PHASE
        uint8_t x = parameter_;
        *buffer++ = static_cast<uint8_t>(
            (((phase.integral >> 8) ^ (x << 1)) & (~x)) + (x >> 1));
    END_SAMPLE_LOOP
}

// ---------------------------------------------------------------------------
// Vowel (Cantarino formant synth) — writes 2 samples per iter + size--
// ---------------------------------------------------------------------------
void Oscillator::RenderVowel(uint8_t* buffer) {
    data_.vw.update = (data_.vw.update + 1) & 3;
    if (!data_.vw.update) {
        uint8_t offset_1 = U8ShiftRight4(parameter_);
        offset_1 = static_cast<uint8_t>(U8U8Mul(offset_1, 7));
        uint8_t offset_2 = offset_1 + 7;
        uint8_t balance = parameter_ & 15;

        // Interpolate formant frequencies.
        for (uint8_t i = 0; i < 3; ++i) {
            data_.vw.formant_increment[i] = U8U4MixU12(
                wav_res_vowel_data[offset_1 + i],
                wav_res_vowel_data[offset_2 + i], balance);
            data_.vw.formant_increment[i] <<= 3;
        }

        // Interpolate formant amplitudes.
        // formant_amplitude[3] aliases noise_modulation in the firmware (they are
        // adjacent struct bytes there); the C++ port keeps them as separate fields,
        // so route the 4th write explicitly to avoid an out-of-bounds write.
        for (uint8_t i = 0; i < 4; ++i) {
            uint8_t amplitude_a = wav_res_vowel_data[offset_1 + 3 + i];
            uint8_t amplitude_b = wav_res_vowel_data[offset_2 + 3 + i];
            const uint8_t v = U8U4MixU8(amplitude_a, amplitude_b, balance);
            if (i < 3) data_.vw.formant_amplitude[i] = v;
            else       data_.vw.noise_modulation    = v;
        }
    }

    int16_t phase_noise = S8S8Mul(
        static_cast<int8_t>(random().state_msb()), static_cast<int8_t>(data_.vw.noise_modulation));
    BEGIN_SAMPLE_LOOP
        int8_t result = 0;
        uint8_t phaselet;

        data_.vw.formant_phase[0] += data_.vw.formant_increment[0];
        phaselet = (data_.vw.formant_phase[0] >> 8) & 0xf0;
        result = static_cast<int8_t>(wav_res_formant_sine[phaselet | data_.vw.formant_amplitude[0]]);

        data_.vw.formant_phase[1] += data_.vw.formant_increment[1];
        phaselet = (data_.vw.formant_phase[1] >> 8) & 0xf0;
        // int8_t += uint8_t: the RHS is added as its full unsigned value
        // (firmware `result += Lookup<uint8_t,...>()`), NOT reinterpreted.
        result = static_cast<int8_t>(result + wav_res_formant_sine[phaselet | data_.vw.formant_amplitude[1]]);

        data_.vw.formant_phase[2] += data_.vw.formant_increment[2];
        phaselet = (data_.vw.formant_phase[2] >> 8) & 0xf0;
        result = static_cast<int8_t>(result + wav_res_formant_square[phaselet | data_.vw.formant_amplitude[2]]);

        result = S8U8MulShift8(result, phase.integral >> 8);
        phase.integral -= phase_increment_int.integral;
        // Glottal reset. The firmware comparison relies on AVR 16-bit-unsigned
        // promotion: replicate it with an explicit uint16 truncation.
        uint16_t lhs = static_cast<uint16_t>(
            static_cast<int32_t>(phase.integral) + phase_noise);
        if (lhs < phase_increment_int.integral) {
            data_.vw.formant_phase[0] = 0;
            data_.vw.formant_phase[1] = 0;
            data_.vw.formant_phase[2] = 0;
        }
        uint8_t x = static_cast<uint8_t>(S16ClipS8(4 * result) + 128);
        *buffer++ = x;
        *buffer++ = x;
        size--;
    END_SAMPLE_LOOP
}

// ---------------------------------------------------------------------------
// Dirty PWM
// ---------------------------------------------------------------------------
void Oscillator::RenderDirtyPwm(uint8_t* buffer) {
    BEGIN_SAMPLE_LOOP
        UPDATE_PHASE
        *buffer++ = (phase.integral >> 8) < 127 + parameter_ ? 0 : 255;
    END_SAMPLE_LOOP
}

// ---------------------------------------------------------------------------
// Quad saw pad (with aliasing)
// ---------------------------------------------------------------------------
void Oscillator::RenderQuadSawPad(uint8_t* buffer) {
    uint16_t phase_spread =
        static_cast<uint16_t>((static_cast<uint32_t>(phase_increment_.integral) * parameter_) >> 13);
    ++phase_spread;
    uint16_t phase_increment = phase_increment_.integral;
    uint16_t increments[3];
    for (uint8_t i = 0; i < 3; ++i) {
        phase_increment += phase_spread;
        increments[i] = phase_increment;
    }

    BEGIN_SAMPLE_LOOP
        UPDATE_PHASE
        data_.qs.phase[0] += increments[0];
        data_.qs.phase[1] += increments[1];
        data_.qs.phase[2] += increments[2];
        uint8_t value = static_cast<uint8_t>(phase.integral >> 10);
        value = static_cast<uint8_t>(value + (data_.qs.phase[0] >> 10));
        value = static_cast<uint8_t>(value + (data_.qs.phase[1] >> 10));
        value = static_cast<uint8_t>(value + (data_.qs.phase[2] >> 10));
        *buffer++ = value;
    END_SAMPLE_LOOP
}

// ---------------------------------------------------------------------------
// Low-passed then high-passed white noise (own LFSR)
// ---------------------------------------------------------------------------
void Oscillator::RenderFilteredNoise(uint8_t* buffer) {
    uint16_t rng_state = data_.no.rng_state;
    if (rng_state == 0) {
        ++rng_state;
    }
    uint8_t filter_coefficient = static_cast<uint8_t>(parameter_ << 2);
    if (filter_coefficient <= 4) {
        filter_coefficient = 4;
    }
    BEGIN_SAMPLE_LOOP
        if (*sync_input_++) {
            rng_state = data_.no.rng_reset_value;
        }
        rng_state = (rng_state >> 1) ^ (-(rng_state & 1) & 0xb400);
        uint8_t noise_sample = static_cast<uint8_t>(rng_state >> 8);
        // Avoid a DC component at the parameter extremes.
        data_.no.lp_noise_sample = U8Mix(data_.no.lp_noise_sample, noise_sample, filter_coefficient);
        if (parameter_ >= 64) {
            *buffer++ = static_cast<uint8_t>(noise_sample - data_.no.lp_noise_sample - 128);
        } else {
            *buffer++ = data_.no.lp_noise_sample;
        }
    END_SAMPLE_LOOP
    data_.no.rng_state = rng_state;
}

// ---------------------------------------------------------------------------
// Interpolated wavetable (position from parameter)
// ---------------------------------------------------------------------------
void Oscillator::RenderInterpolatedWavetable(uint8_t* buffer) {
    // Which wavetable should we play?
    const uint8_t* wavetable_definition =
        wav_res_wavetables + U8U8Mul(shape_ - WAVEFORM_WAVETABLE_1, 18);
    // Get a 8:8 value with the wave index in the first byte, and the
    // balance amount in the second byte.
    uint8_t num_steps = wavetable_definition[0];
    uint16_t pointer = U8U8Mul(static_cast<uint8_t>(parameter_ << 1), num_steps);
    uint16_t wave_index_1 = wavetable_definition[1 + (pointer >> 8)];
    uint16_t wave_index_2 = wavetable_definition[2 + (pointer >> 8)];
    uint8_t gain = pointer & 0xff;
    const uint8_t* wave_1 = wav_res_waves + U8U8Mul(static_cast<uint8_t>(wave_index_1), 129);
    const uint8_t* wave_2 = wav_res_waves + U8U8Mul(static_cast<uint8_t>(wave_index_2), 129);
    BEGIN_SAMPLE_LOOP
        UPDATE_PHASE_MORE_REGISTERS
        *buffer++ = InterpolateTwoTables(wave_1, wave_2, phase.integral >> 1, static_cast<uint8_t>(~gain), gain);
    END_SAMPLE_LOOP
}

// ---------------------------------------------------------------------------
// Wavequence (single wave, position from parameter)
// ---------------------------------------------------------------------------
void Oscillator::RenderWavequence(uint8_t* buffer) {
    // wav_res_waves holds 80 single-cycle waves (WAV_RES_WAVES_SIZE = 80*129).
    // The firmware indexes it directly with parameter_ (0..127) and reads
    // adjacent PROGMEM past wave 79; in C++ that is an out-of-bounds read, so
    // clamp the index to the table.
    constexpr int kNumWaves = static_cast<int> (WAV_RES_WAVES_SIZE) / 129;
    const uint8_t wave_index = (parameter_ < kNumWaves)
        ? parameter_
        : static_cast<uint8_t> (kNumWaves - 1);
    const uint8_t* wave = wav_res_waves + U8U8Mul(wave_index, 129);
    BEGIN_SAMPLE_LOOP
        UPDATE_PHASE
        *buffer++ = InterpolateSample(wave, phase.integral >> 1);
    END_SAMPLE_LOOP
}

// ---------------------------------------------------------------------------
// Render dispatch table — faithful copy of oscillator.cc:481-504.
// Indexed by WAVEFORM_* (shapes >= WAVEFORM_WAVETABLE_1 all map to entry 21;
// WAVEFORM_WAVEQUENCE is special-cased in Render()).
// ---------------------------------------------------------------------------
const Oscillator::RenderFn Oscillator::fn_table_[] = {
    &Oscillator::RenderSilence,

    &Oscillator::RenderSimpleWavetable,
    &Oscillator::RenderBandlimitedPwm,
    &Oscillator::RenderSimpleWavetable,
    &Oscillator::RenderSimpleWavetable,

    &Oscillator::RenderCzSaw,
    &Oscillator::RenderCzResoSaw,
    &Oscillator::RenderCzResoSaw,
    &Oscillator::RenderCzResoSaw,
    &Oscillator::RenderCzResoSaw,
    &Oscillator::RenderCzResoPulse,
    &Oscillator::RenderCzResoPulse,
    &Oscillator::RenderCzResoPulse,
    &Oscillator::RenderCzResoPulse,
    &Oscillator::RenderCzResoTri,

    &Oscillator::RenderQuadSawPad,

    &Oscillator::RenderFm,

    &Oscillator::Render8BitLand,
    &Oscillator::RenderDirtyPwm,
    &Oscillator::RenderFilteredNoise,
    &Oscillator::RenderVowel,

    &Oscillator::RenderInterpolatedWavetable,
};

}  // namespace ambika::dsp

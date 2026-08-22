// Faithful C++17 port of Ambika's `voicecard/oscillator.{h,cc}` (15 waveform
// algorithms).
//
// Original: ambika_reference/voicecard/oscillator.{h,cc} (Emilie Gillet, GPL3).
//
// The Oscillator is an INSTANCE class (the firmware uses two instances,
// osc_1 / osc_2, per voicecard). Per-instance state (phase_, phase_increment_,
// shape_, parameter_, note_, the algorithm state union, sync pointers) mirrors
// the firmware member layout. Globals in firmware are zero-initialised (BSS);
// here every member is value-initialised via `{}` default member initialisers
// so a freshly constructed Oscillator matches a power-up voicecard.
//
// ALL arithmetic stays integer (8-bit audio centred at 128, 24-bit phase
// accumulators). No float anywhere. The output buffer is uint8_t[kAudioBlockSize].

#ifndef AMBIKA_DSP_OSCILLATOR_H_
#define AMBIKA_DSP_OSCILLATOR_H_

#include <array>
#include <cstdint>

#include "dsp/constants.h"    // kAudioBlockSize (render-buffer bound)
#include "dsp/fixed_types.h"
#include "dsp/patch.h"         // OscillatorAlgorithm enum (WAVEFORM_*).
#include "dsp/random.h"        // Reset() draws from the global LFSR.

namespace ambika::dsp {

// Bandlimited multi-zone sample-rate selection counts (also in constants.h;
// kept here to mirror the firmware header).
static const uint8_t kNumZonesFullSampleRateOsc = 6;
static const uint8_t kNumZonesHalfSampleRateOsc = 5;

// ---- Per-algorithm state (union; same layout as firmware) ----------------

struct VowelSynthesizerState {
    uint16_t formant_increment[3];
    uint16_t formant_phase[3];
    uint8_t  formant_amplitude[3];
    uint8_t  noise_modulation;   // aliases formant_amplitude[3] in the firmware
    uint8_t  update;             // updated only every 4th call (control decimation)
};

struct FilteredNoiseState {
    uint8_t  lp_noise_sample;
    uint16_t rng_state;          // the algorithm's OWN Galois LFSR (not global Random)
    uint16_t rng_reset_value;
};

struct QuadSawPadState {
    uint16_t phase[3];
};

union OscillatorState {
    VowelSynthesizerState vw;
    FilteredNoiseState    no;
    QuadSawPadState       qs;
    uint16_t              secondary_phase;   // CZ / FM resonator phase
};

class Oscillator {
 public:
    // Render-buffer type: a reference to the fixed 40-sample block. Passing the
    // buffer as a sized reference (not uint8_t*) makes the kAudioBlockSize
    // contract un-checkable-by-accident: callers cannot hand a mis-sized or
    // heap buffer without a compile error, and the render loops cannot walk
    // past the end (memory-safety migration; zero runtime cost).
    using RenderBuffer = uint8_t (&)[kAudioBlockSize];

    // Pointer-to-member-function type for the render dispatch table.
    typedef void (Oscillator::*RenderFn)(RenderBuffer buffer);

    Oscillator() = default;

    // Re-seeds the filtered-noise LFSR reset value from the global RNG.
    // (Firmware: data_.no.rng_reset_value = Random::GetByte() + 1.)
    inline void Reset() {
        data_.no.rng_reset_value = static_cast<uint16_t>(random().GetByte()) + 1;
    }

    // Render one kAudioBlockSize buffer. `increment` is the 24-bit phase
    // increment; `sync_input`/`sync_output` are per-sample sync arrays of
    // length kAudioBlockSize (a non-zero sync input sample resets the phase to
    // 0 before the increment; the sync output records phase wraps / carries).
    // Render one kAudioBlockSize buffer. `increment` is the 24-bit phase
    // increment; `sync_input`/`sync_output` are per-sample sync arrays of
    // length kAudioBlockSize (a non-zero sync input sample resets the phase
    // to 0 before the increment; the sync output records phase wraps / carries).
    // All three arrays are passed as sized references — see RenderBuffer.
    inline void Render(uint8_t shape,
                       uint8_t note,
                       uint24_t increment,
                       RenderBuffer sync_input,
                       RenderBuffer sync_output,
                       RenderBuffer buffer) {
        shape_ = shape;
        note_ = note;
        phase_increment_ = increment;
        sync_input_ = sync_input;
        sync_output_ = sync_output;

        // Hack: when pulse width is 0, use a plain bandlimited wavetable.
        if (shape_ == WAVEFORM_SQUARE) {
            if (parameter_ == 0) {
                RenderSimpleWavetable(buffer);
            } else {
                RenderBandlimitedPwm(buffer);
            }
        } else {
            RenderFn fn;
            uint8_t index = shape_ >= WAVEFORM_WAVETABLE_1
                                ? WAVEFORM_WAVETABLE_1
                                : shape_;
            fn = fn_table_[index];
            if (shape_ == WAVEFORM_WAVEQUENCE) {
                fn = &Oscillator::RenderWavequence;
            }
            (this->*fn)(buffer);
        }
    }

    inline void set_parameter(uint8_t parameter) { parameter_ = parameter; }
    inline void set_fm_parameter(uint8_t fm_parameter) { fm_parameter_ = fm_parameter; }

 private:
    void RenderSilence(RenderBuffer buffer);
    void RenderBandlimitedPwm(RenderBuffer buffer);
    void RenderSimpleWavetable(RenderBuffer buffer);
    void RenderCzSaw(RenderBuffer buffer);
    void RenderCzResoSaw(RenderBuffer buffer);
    void RenderCzResoPulse(RenderBuffer buffer);
    void RenderCzResoTri(RenderBuffer buffer);
    void RenderFm(RenderBuffer buffer);
    void Render8BitLand(RenderBuffer buffer);
    void RenderVowel(RenderBuffer buffer);
    void RenderDirtyPwm(RenderBuffer buffer);
    void RenderQuadSawPad(RenderBuffer buffer);
    void RenderFilteredNoise(RenderBuffer buffer);
    void RenderInterpolatedWavetable(RenderBuffer buffer);
    void RenderWavequence(RenderBuffer buffer);

    // Current phase (24-bit fixed {integral:16, fractional:8}).
    uint24_t phase_ {};

    // Phase increment (and, for some algos, phase increment x 2).
    uint24_t phase_increment_ {};

    uint8_t shape_ {};
    uint8_t parameter_ {};
    uint8_t fm_parameter_ {};
    uint8_t note_ {};

    // Union of state used by each algorithm.
    OscillatorState data_ {};

    // Render dispatch table (indexed by WAVEFORM_*; see oscillator.cpp).
    // std::array gives a compile-checked size (static_assert below) and a
    // bounds-checked operator[] in debug builds.
    static const std::array<RenderFn, WAVEFORM_WAVETABLE_1 + 1> fn_table_;
    static_assert(sizeof (fn_table_) / sizeof (RenderFn) == WAVEFORM_WAVETABLE_1 + 1,
                  "dispatch table must cover every WAVEFORM_* below WAVEFORM_WAVETABLE_1");

    // Per-sample sync arrays. A non-zero byte at sync_input_[i] resets the
    // phase to 0; sync_output_[i] records the 24-bit overflow (carry).
    uint8_t* sync_input_ {};
    uint8_t* sync_output_ {};
};

}  // namespace ambika::dsp

#endif  // AMBIKA_DSP_OSCILLATOR_H_

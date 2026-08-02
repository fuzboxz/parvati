// Copyright 2009 Emilie Gillet.
//
// Faithful C++17 port of Ambika's `common/lfo.h` (cheap LFO / modulation
// oscillator).
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// -----------------------------------------------------------------------------
//
// RESOLVED — LFO WAVETABLE ADDRESSING (spec flag B.5 / cross-cutting risk #2):
//
//   The firmware `Render()` default case reads `wav_res_lfo_waveforms + offset`
//   with `offset = (shape - LFO_WAVEFORM_WAVE_1) * 257`, but `wav_res_lfo_waveforms`
//   is ONLY 2 BYTES ({1, 254}). This is NOT a real addressing scheme on the
//   voicecard: the voicecard makefile defines `-DDISABLE_WAVETABLE_LFOS`, so the
//   whole `default:` wavetable branch is COMPILED OUT (see common/lfo.h:71).
//   `wav_res_lfo_waveforms` is dead data that the voicecard never reads.
//
//   Confirming the shapes are unreachable on the voicecard: the voice-LFO shape
//   parameter `PRM_PATCH_VOICE_LFO_SHAPE` is clamped to `0..LFO_WAVEFORM_RAMP`
//   (controller/parameter.cc:590) — i.e. only TRIANGLE/SQUARE/S&H/RAMP.
//
//   The 16 wavetable LFO shapes (WAVE_1..WAVE_16) exist only in the LfoWave enum
//   for the CONTROLLER, which renders LFOs 1/2/3 itself (controller-side, where
//   DISABLE_WAVETABLE_LFOS is NOT defined) and streams their 8-bit values to the
//   voicecard over SPI (`COMMAND_WRITE_LFO` → set_modulation_source). The
//   voicecard renders only its own voice LFO (MOD_SRC_LFO_4), shapes 0..3.
//
//   => This port faithfully compiles OUT the wavetable-LFO branch, exactly as
//      the voicecard firmware does. Only RAMP/S&H/TRIANGLE/SQUARE are reachable.
//
// Instance-based. Phase is a 16-bit accumulator; output is uint8_t (0..255).

#ifndef AMBIKA_DSP_LFO_H_
#define AMBIKA_DSP_LFO_H_

#include <cstdint>

#include "dsp/fixed_math.h"
#include "dsp/patch.h"
#include "dsp/random.h"
#include "dsp/resources/resources.h"

namespace ambika::dsp {

// Mirrors the firmware voicecard build flag. Defined → wavetable-LFO branch is
// omitted (matches voicecard/makefile EXTRA_DEFINES).
#define PARVATI_DISABLE_WAVETABLE_LFOS

class Lfo {
 public:
    Lfo() = default;

    uint8_t Render(uint8_t shape) {
        phase_ += phase_increment_;
        looped_ = phase_ < phase_increment_;

        // Compute the LFO value.
        uint8_t value = value_;  // S&H persists; also a safe default below.
        switch (shape) {
            case LFO_WAVEFORM_RAMP:
                value = static_cast<uint8_t>(phase_ >> 8);
                break;

            case LFO_WAVEFORM_S_H:
                if (looped_) {
                    value_ = random().GetByte();
                }
                value = value_;
                break;

            case LFO_WAVEFORM_TRIANGLE:
                // Second half (phase & 0x8000): rising 0..255.
                // First half: falling 255..0 (~(phase>>7), 8-bit).
                value = (phase_ & 0x8000u)
                            ? static_cast<uint8_t>(phase_ >> 7)
                            : static_cast<uint8_t>(~static_cast<uint8_t>(phase_ >> 7));
                break;

            case LFO_WAVEFORM_SQUARE:
                value = (phase_ & 0x8000u) ? 255 : 0;
                break;

#ifndef PARVATI_DISABLE_WAVETABLE_LFOS
            default: {
                uint8_t shape_offset = shape - LFO_WAVEFORM_WAVE_1;
                uint16_t offset = static_cast<uint16_t>(shape_offset) << 8;
                offset += shape_offset;
                value = InterpolateSample(wav_res_lfo_waveforms + offset, phase_);
                break;
            }
#endif  // PARVATI_DISABLE_WAVETABLE_LFOS

            default:
                // Unreachable on the voicecard (shape clamped to 0..3); keep
                // the persisted S&H value for determinism (firmware UB here).
                break;
        }
        return value;
    }

    void set_phase(uint16_t phase) {
        looped_ = phase <= phase_;
        phase_ = phase;
    }

    void set_phase_increment(uint16_t phase_increment) {
        phase_increment_ = phase_increment;
    }

    uint8_t looped() const { return looped_; }

 private:
    // Phase increment.
    uint16_t phase_increment_ = 0;

    // Current phase of the LFO.
    uint16_t phase_ = 0;
    uint8_t looped_ = 0;

    // Current value of S&H.
    uint8_t value_ = 0;
};

}  // namespace ambika::dsp

#endif  // AMBIKA_DSP_LFO_H_

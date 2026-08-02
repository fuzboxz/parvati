// Copyright 2011 Emilie Gillet.
//
// Faithful C++17 port of Ambika's `voicecard/envelope.h` (ADSR + DEAD
// envelope generator).
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
// All arithmetic stays INTEGER and bit-matches the firmware:
//   * `value_` is a 16-bit accumulator (0..65025); Render() returns value_>>8
//     (0..254 — note the ÷256 truncation, so the peak is 254 not 255).
//   * segment interpolation uses the `wav_res_env_expo` exponential curve
//     via InterpolateSample (linear interp over the 257-entry table).
//   * phase is a 16-bit accumulator; a segment ends on 16-bit wraparound.
//
// Instance-based: the firmware already used one Envelope per slot; the voice
// owns three (kNumEnvelopes == 3). No static-singleton conversion needed.

#ifndef AMBIKA_DSP_ENVELOPE_H_
#define AMBIKA_DSP_ENVELOPE_H_

#include <cstdint>

#include "dsp/fixed_math.h"
#include "dsp/resources/resources.h"

namespace ambika::dsp {

enum EnvelopeStage {
    ATTACK = 0,
    DECAY = 1,
    SUSTAIN = 2,
    RELEASE = 3,
    DEAD = 4,
    NUM_SEGMENTS,
};

class Envelope {
 public:
    Envelope() = default;

    void Init() {
        stage_target_[ATTACK] = 255;
        stage_target_[RELEASE] = 0;
        stage_target_[DEAD] = 0;
        stage_phase_increment_[SUSTAIN] = 0;
        stage_phase_increment_[DEAD] = 0;
    }

    uint8_t stage() const { return stage_; }

    // Begin a segment. DEAD forces the accumulator to 0 (silent). `a_` is
    // seeded from the current output value so segments chain seamlessly.
    void Trigger(uint8_t stage) {
        if (stage == DEAD) {
            value_ = 0;
        }
        a_ = static_cast<uint8_t>(value_ >> 8);
        b_ = stage_target_[stage];
        stage_ = stage;
        phase_ = 0;
        phase_increment_ = stage_phase_increment_[stage];
    }

    // Refresh the per-stage phase increments (from the portamento/expo LUT)
    // and the sustain target. attack/decay/release are 0..127 patch values.
    inline void Update(uint8_t attack, uint8_t decay, uint8_t sustain, uint8_t release) {
        stage_phase_increment_[ATTACK] =
            lut_res_env_portamento_increments[attack];
        stage_phase_increment_[DECAY] =
            lut_res_env_portamento_increments[decay];
        stage_phase_increment_[RELEASE] =
            lut_res_env_portamento_increments[release];
        stage_target_[DECAY] = static_cast<uint8_t>(sustain << 1);
        stage_target_[SUSTAIN] = stage_target_[DECAY];
    }

    // Advance the phase by one control step and return the 8-bit output.
    uint8_t Render() {
        phase_ += phase_increment_;
        if (phase_ < phase_increment_) {
            // 16-bit phase wrapped: the segment is complete. Snap to the end
            // target (U8MixU16(a,b,255) == b*255) and advance to next stage.
            value_ = U8MixU16(a_, b_, 255);
            Trigger(++stage_);
        }
        if (phase_increment_) {
            uint8_t step = InterpolateSample(wav_res_env_expo, phase_);
            value_ = U8MixU16(a_, b_, step);
        }
        return static_cast<uint8_t>(value_ >> 8);
    }

 private:
    // Phase increments for each stage (16-bit accumulators; SUSTAIN/DEAD = 0).
    uint16_t stage_phase_increment_[NUM_SEGMENTS] = {};
    // Value that needs to be reached at the end of each stage.
    uint8_t stage_target_[NUM_SEGMENTS] = {};
    // Current stage.
    uint8_t stage_ = 0;

    // Start and end value of the current segment (8-bit targets).
    uint8_t a_ = 0;
    uint8_t b_ = 0;

    // Phase and phase increment of the current segment.
    uint16_t phase_increment_ = 0;
    uint16_t phase_ = 0;

    // Current value of the envelope (16-bit accumulator; output = value_>>8).
    uint16_t value_ = 0;
};

}  // namespace ambika::dsp

#endif  // AMBIKA_DSP_ENVELOPE_H_

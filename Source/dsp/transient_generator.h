// Faithful C++17 port of Ambika's `voicecard/transient_generator.h`.
//
// Original: ambika_reference/voicecard/transient_generator.h
//           (Emilie Gillet, GPL3).
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// CONVERSION NOTE: the firmware TransientGenerator is a STATIC singleton.
// Here it is an INSTANCE class (per-voice): counter_ / rng_state_ / decimate_
// / gain_ are instance members. The firmware's static `fn_table_[]` dispatch
// is reproduced with an inline switch (behaviourally identical).
//
// CRITICAL ORDERING: each of the 5 generator functions sets `gain_` as a SIDE
// EFFECT before returning its sample; Render() then multiplies gain_ by
// `amount` to get the mix amplitude. That set-then-use ordering is preserved
// exactly.
//
// NOTE: the generators use their own per-instance 8-bit LCG (rng_state_), NOT
// the global Random. This matches the firmware, which never calls Random::*
// from the transient generator.

#ifndef PARVATI_DSP_TRANSIENT_GENERATOR_H
#define PARVATI_DSP_TRANSIENT_GENERATOR_H

#include <cstdint>

#include "dsp/constants.h"  // kAudioBlockSize
#include "dsp/fixed_math.h"  // U8Mix, U8U8MulShift8
#include "dsp/patch.h"       // WAVEFORM_SUB_OSC_* enums

namespace ambika::dsp {

class TransientGenerator {
 public:
    TransientGenerator() = default;

    // Arms the transient: counter_ counts down over a single 255-sample decay.
    void Trigger() { counter_ = 255; }

    // Renders into `buffer` (mixing over existing content) for the transient
    // shapes CLICK/GLITCH/BLOW/METALLIC/POP (`shape >= WAVEFORM_SUB_OSC_CLICK`).
    // Other shapes are left untouched (the sub-oscillator handles them).
    void Render(uint8_t shape, uint8_t* buffer, uint8_t amount) {
        if (shape < WAVEFORM_SUB_OSC_CLICK) {
            return;  // Not my business -- handled by the sub oscillator.
        }
        if (shape > WAVEFORM_SUB_OSC_POP) {
            shape = WAVEFORM_SUB_OSC_POP;
        }
        uint8_t size = kAudioBlockSize;
        while (counter_ && size--) {
            uint8_t value = RenderGenerator(shape);
            uint8_t amplitude = U8U8MulShift8(gain_, amount);
            *buffer = U8Mix(*buffer, value, amplitude);
            ++buffer;
        }
    }

 private:
    // Dispatch equivalent to the firmware fn_table_[shape - CLICK].
    uint8_t RenderGenerator(uint8_t shape) {
        switch (shape) {
            case WAVEFORM_SUB_OSC_CLICK:    return RenderClick();
            case WAVEFORM_SUB_OSC_GLITCH:   return RenderGlitch();
            case WAVEFORM_SUB_OSC_BLOW:     return RenderBlow();
            case WAVEFORM_SUB_OSC_METALLIC: return RenderMetallic();
            default:                        return RenderPop();  // WAVEFORM_SUB_OSC_POP
        }
    }

    uint8_t RenderClick() {
        gain_ = counter_;
        --counter_;
        return counter_ < 32 ? 255 : 0;
    }

    uint8_t RenderGlitch() {
        gain_ = counter_;
        --counter_;
        rng_state_ = static_cast<uint8_t>(rng_state_ * 73 + counter_);
        return rng_state_;
    }

    uint8_t RenderBlow() {
        decimate_ = static_cast<uint8_t>(decimate_ + 2);
        if (decimate_ >= 16) {
            decimate_ = static_cast<uint8_t>(decimate_ - 17);
            rng_state_ = static_cast<uint8_t>(rng_state_ * 73 + counter_);
            if (decimate_ == 0) {
                --counter_;
                gain_ = (counter_ & 0x80) ? static_cast<uint8_t>(~counter_)
                                          : counter_;
            }
        }
        return rng_state_;
    }

    uint8_t RenderMetallic() {
        --counter_;
        gain_ = counter_ >= 64 ? 255 : static_cast<uint8_t>(counter_ << 2);
        return static_cast<uint8_t>(counter_ * 57);
    }

    uint8_t RenderPop() {
        --counter_;
        gain_ = counter_ > 0 ? 255 : 0;
        return 0;
    }

    // Per-instance transient state (all are instance members, not static).
    uint8_t rng_state_ = 0;   // private 8-bit LCG state
    uint8_t decimate_ = 0;    // Blow decimation counter
    uint8_t gain_ = 0;        // set inside each generator, read by Render()
    uint8_t counter_ = 0;     // decay counter (255 -> 0 across the transient)

    TransientGenerator(const TransientGenerator&) = delete;
    TransientGenerator& operator=(const TransientGenerator&) = delete;
};

}  // namespace ambika::dsp

#endif  // PARVATI_DSP_TRANSIENT_GENERATOR_H

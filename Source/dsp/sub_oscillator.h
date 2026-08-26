// Faithful C++17 port of Ambika's `voicecard/sub_oscillator.h`.
//
// Original: ambika_reference/voicecard/sub_oscillator.h (Emilie Gillet, GPL3).
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// CONVERSION NOTE: in the firmware SubOscillator is a STATIC singleton (one
// instance shared across the voicecard). For 16-voice polyphony it is an
// INSTANCE class here: the `static` phase accumulators are now per-instance
// members. The synthesis algorithm is otherwise byte-for-byte identical
// (integer 8-bit, centred at 128).

#ifndef HELLCAT_DSP_SUB_OSCILLATOR_H
#define HELLCAT_DSP_SUB_OSCILLATOR_H

#include <cstdint>

#include "dsp/constants.h"    // kAudioBlockSize
#include "dsp/fixed_math.h"   // U24Add, U24ShiftRight, U8Mix
#include "dsp/fixed_types.h"  // uint24_t

namespace ambika::dsp {

class SubOscillator {
 public:
    SubOscillator() = default;

    // Stores the phase increment received from the voice (voice.cc passes
    // U24ShiftRight(osc1_increment), i.e. one octave below oscillator 1).
    void set_increment(uint24_t increment) { phase_increment_ = increment; }

    // Renders a kAudioBlockSize-sample block of sub-oscillator into `buffer`,
    // mixing it over the existing content with `amount` (0..255). The buffer
    // is a sized reference (not uint8_t*) so a mis-sized buffer cannot be
    // passed (memory-safety migration; zero runtime cost).
    void Render(uint8_t shape, uint8_t (&buffer)[kAudioBlockSize], uint8_t amount) {
        uint24_t increment = phase_increment_;
        if (shape >= 3) {
            // Shapes 3..5 are one octave below shapes 0..2.
            increment = U24ShiftRight(increment);
            shape = static_cast<uint8_t>(shape - 3);
        }
        uint8_t size = kAudioBlockSize;
        uint8_t pulse_width = shape == 0 ? 0x80 : 0x40;
        uint8_t sub_gain = amount;
        uint8_t mix_gain = static_cast<uint8_t>(~sub_gain);  // 255 - sub_gain
        uint8_t* out = buffer;   // sized-array param -> local write cursor
        while (size--) {
            phase_ = U24Add(phase_, increment);
            uint8_t v;
            if (shape != 1) {
                // Square (shape 0) / pulse (shape 2): compare top 8 phase bits
                // against the pulse width.
                v = static_cast<uint8_t>(phase_.integral >> 8) < pulse_width
                        ? 0
                        : 255;
            } else {
                // Triangle (shape 1): folded top bits of the phase.
                uint8_t tri = static_cast<uint8_t>(phase_.integral >> 7);
                v = (phase_.integral & 0x8000u) ? tri
                                                : static_cast<uint8_t>(~tri);
            }
            *out = U8Mix(*out, v, mix_gain, sub_gain);
            ++out;
        }
    }

 private:
    // Per-instance 24-bit phase accumulator and increment.
    uint24_t phase_ {};
    uint24_t phase_increment_ {};

    SubOscillator(const SubOscillator&) = delete;
    SubOscillator& operator=(const SubOscillator&) = delete;
};

}  // namespace ambika::dsp

#endif  // HELLCAT_DSP_SUB_OSCILLATOR_H

// Port of avrlib/random.h — a 16-bit Galois LFSR pseudo-random generator.
//
// Original: ambika_reference/avrlib/random.h (Emilie Gillet, GPL3).
// Feedback polynomial x^16 + x^14 + x^13 + x^11, period 65535.
//
// IMPORTANT distinction the DSP relies on (voice.cc):
//   * GetByte()    ADVANCES the state, then returns its top byte.  (MOD_SRC_NOISE)
//   * state_msb()  returns the top byte WITHOUT advancing.          (MOD_SRC_RANDOM,
//                                                                   vowel glottal noise)
// A single global instance is shared across the whole synth; call random().

#ifndef PARVATI_DSP_RANDOM_H
#define PARVATI_DSP_RANDOM_H

#include <cstdint>

namespace ambika::dsp {

class Random {
 public:
    Random() = default;

    void Seed(uint16_t seed) { state_ = seed; }

    // Advance one LFSR step.
    void Update() {
        // Original: state_ = (state_ >> 1) ^ (-(state_ & 1) & 0xb400);
        // Equivalent 16-bit form (avoids implementation-defined negation):
        uint16_t lsb = static_cast<uint16_t>(state_ & 1u);
        state_ = static_cast<uint16_t>((state_ >> 1) ^ (lsb ? 0xB400u : 0u));
    }

    uint16_t state() const      { return state_; }
    uint8_t  state_msb() const  { return static_cast<uint8_t>(state_ >> 8); }  // no advance

    uint8_t  GetByte()  { Update(); return state_msb(); }   // advances
    uint16_t GetWord()  { Update(); return state_; }

 private:
    uint16_t state_ = 0x21u;  // firmware boot seed (avrlib/random.cc); 0 would lock the LFSR
};

// Global shared instance (defined in random.cpp).
Random& random();

}  // namespace ambika::dsp

#endif  // PARVATI_DSP_RANDOM_H

// Port of avrlib/base.h fixed-width types for the Ambika DSP.
//
// Original: ambika_reference/avrlib/base.h (Emilie Gillet, GPL3).
//
// These mirror the EXACT field layout the firmware uses. The 24-bit
// phase accumulators are stored as { integral:16, fractional:8 } and the
// "carry" variant { carry:8, integral:16, fractional:8 } records a 24-bit
// overflow (used as the oscillator sync/wrap pulse, see oscillator.cc
// UPDATE_PHASE / BEGIN_SAMPLE_LOOP).
//
// Field order of uint24c_t is deliberately {carry, integral, fractional}
// to match base.h.

#ifndef HELLCAT_DSP_FIXED_TYPES_H
#define HELLCAT_DSP_FIXED_TYPES_H

#include <cstdint>

namespace ambika::dsp {

// 24-bit unsigned fixed value: high 16 bits + low 8 bits.
struct uint24_t {
    uint16_t integral;
    uint8_t  fractional;
};

// 24-bit value plus a 1-bit carry flag (set when a 24-bit add overflows).
// carry is the FIRST field, matching avrlib/base.h so any aggregate
// initialisation stays compatible.
struct uint24c_t {
    uint8_t  carry;
    uint16_t integral;
    uint8_t  fractional;
};

}  // namespace ambika::dsp

#endif  // HELLCAT_DSP_FIXED_TYPES_H

// Desktop shim: avrlib/op.h — plain-C++ replacements for the firmware's
// optimized AVR inline-asm fixed-point helpers (tests/firmware_parity_test).
//
// The real header unconditionally #defines USE_OPTIMIZED_OP, selecting AVR
// asm variants that cannot compile off-device. The formulas below are taken
// from the real header's own portable C++ branch (the #else of every
// USE_OPTIMIZED_OP block); only the subset reached by the parity oracle's
// firmware closure (controller/part.cc, controller/multi.cc,
// controller/resources.cc, common/lfo.h, note_stack.h, voice_allocator.cc,
// avrlib/random.cc) is provided. Semantics are bit-identical to the C++
// branch the firmware itself uses when the optimizer is disabled.
#pragma once

#include <cstdint>

#include "avrlib/base.h"
#include "avr/pgmspace.h"   // desktop shim: prog_* typedefs + pgm_read_*

namespace avrlib
{

static inline int16_t Clip (int16_t value, int16_t min, int16_t max)
{
    return value < min ? min : (value > max ? max : value);
}

static inline int16_t S16ClipU14 (int16_t value)
{
    uint8_t msb = static_cast<uint16_t> (value) >> 8;
    if (msb & 0x80) return 0;
    if (msb & 0x40) return 16383;
    return value;
}

static inline uint8_t U8AddClip (uint8_t value, uint8_t increment, uint8_t max)
{
    value += increment;
    if (value > max) value = max;
    return value;
}

static inline uint8_t S16ShiftRight8 (int16_t value)
{
    return static_cast<uint8_t> (static_cast<uint16_t> (value) >> 8);
}

static inline uint8_t S16ClipU8 (int16_t value)
{
    return static_cast<uint8_t> (Clip (value, 0, 255));
}

static inline int8_t S16ClipS8 (int16_t value)
{
    return static_cast<int8_t> (Clip (value, -128, 127));
}

static inline uint8_t U8U8MulShift8 (uint8_t a, uint8_t b)
{
    return static_cast<uint8_t> ((static_cast<uint16_t> (a) * b) >> 8);
}

static inline int8_t S8U8MulShift8 (int8_t a, uint8_t b)
{
    return static_cast<int8_t> ((static_cast<int16_t> (a) * b) >> 8);
}

static inline int16_t S8U8Mul (int8_t a, uint8_t b)
{
    return static_cast<int16_t> (a) * b;
}

static inline int16_t S8S8Mul (int8_t a, int8_t b)
{
    return static_cast<int16_t> (a) * b;
}

static inline uint16_t U8U8Mul (uint8_t a, uint8_t b)
{
    return static_cast<uint16_t> (a) * b;
}

static inline int16_t S16U8MulShift8 (int16_t a, uint8_t b)
{
    return static_cast<int16_t> ((static_cast<int32_t> (a) * b) >> 8);
}

static inline uint16_t U16U8MulShift8 (uint16_t a, uint8_t b)
{
    return static_cast<uint16_t> ((static_cast<uint32_t> (a) * b) >> 8);
}

static inline int16_t S16S8MulShift8 (int16_t a, int8_t b)
{
    return static_cast<int16_t> ((static_cast<int32_t> (a) * b) >> 8);
}

static inline uint8_t U8Mix (uint8_t a, uint8_t b, uint8_t balance)
{
    return static_cast<uint8_t> (U8U8MulShift8 (a, static_cast<uint8_t> (255 - balance))
                               + U8U8MulShift8 (b, balance));
}

static inline uint8_t U8Mix (uint8_t a, uint8_t b, uint8_t gain_a, uint8_t gain_b)
{
    return static_cast<uint8_t> (U8U8MulShift8 (a, gain_a) + U8U8MulShift8 (b, gain_b));
}

// 14-bit value >> 6 (portable form of the real header's C++ branch).
static inline uint8_t U14ShiftRight6 (uint16_t value)
{
    return static_cast<uint8_t> (value >> 6);
}

static inline uint8_t U15ShiftRight7 (uint16_t value)
{
    return static_cast<uint8_t> (value >> 7);
}

static inline uint8_t U8ShiftRight4 (uint8_t a) { return static_cast<uint8_t> (a >> 4); }
static inline uint8_t U8ShiftLeft4 (uint8_t a)  { return static_cast<uint8_t> (a << 4); }
static inline uint8_t U8Swap4 (uint8_t a)       { return static_cast<uint8_t> ((a << 4) | (a >> 4)); }

// Waveform interpolation (the real header's portable branch): linear blend of
// two adjacent table samples by the fractional phase byte.
static inline uint8_t InterpolateSample (const prog_uint8_t* table, uint16_t phase)
{
    return U8Mix (pgm_read_byte (table + (phase >> 8)),
                  pgm_read_byte (1 + table + (phase >> 8)),
                  static_cast<uint8_t> (phase & 0xff));
}

}  // namespace avrlib

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

static inline int8_t S8S8MulShift8 (int8_t a, int8_t b)
{
    return static_cast<int8_t> ((static_cast<int32_t> (a) * b) >> 8);
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
    // Real asm: full 16-bit sum a*(255-balance) + b*balance, result = sum >> 8
    // (single shift — NOT two >>8 multiplies summed; that double truncation
    // was off-by-one vs the firmware and surfaced as ±1 LSB per sample in the
    // voicecard audio oracle's block compares). Bits 8..15 of the (possibly
    // 17-bit) sum are what bytes[1] returns; the int arithmetic reproduces
    // them exactly.
    return static_cast<uint8_t> ((static_cast<int> (a) * static_cast<uint8_t> (255 - balance)
                                  + static_cast<int> (b) * balance) >> 8);
}

static inline uint8_t U8Mix (uint8_t a, uint8_t b, uint8_t gain_a, uint8_t gain_b)
{
    // Real asm: full sum a*gain_a + b*gain_b (can reach 17 bits), result =
    // sum >> 8 — single shift, same reasoning as the balance overload above.
    return static_cast<uint8_t> ((static_cast<int> (a) * gain_a
                                  + static_cast<int> (b) * gain_b) >> 8);
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

static inline uint16_t U8MixU16 (uint8_t a, uint8_t b, uint8_t balance)
{
    // The real header has only the AVR-asm form; per its multiply comments the
    // value is the FULL 16-bit sum a*(255-balance) + b*balance (no >>8) —
    // identical to Hellcat's fixed_math.h U8MixU16 (used by voicecard/
    // envelope.h in the audio oracle).
    return static_cast<uint16_t> (a) * static_cast<uint8_t> (255 - balance)
           + static_cast<uint16_t> (b) * balance;
}

// --- 24-bit fixed-point helpers -------------------------------------------
// uint24_t / uint24c_t (integral:16 + fractional:8 [+ carry]) come from the
// REAL avrlib/base.h included above. The real op.h has AVR-asm-only forms;
// these portable equivalents use a 32-bit intermediate — semantics identical
// to the asm (documented shifts/adds), and matching how Hellcat's port
// translated the same asm (Source/dsp/fixed_math.h — independently written,
// cross-validating the translation).
static inline uint32_t U24To32 (uint24_t a)
{
    return (static_cast<uint32_t> (a.integral) << 8) | a.fractional;
}

static inline uint24_t U24From32 (uint32_t v)
{
    uint24_t r;
    v &= 0xFFFFFF;
    r.integral = static_cast<uint16_t> (v >> 8);
    r.fractional = static_cast<uint8_t> (v);
    return r;
}

static inline uint24c_t U24AddC (uint24c_t a, uint24_t b)
{
    const uint32_t lhs = (static_cast<uint32_t> (a.integral) << 8) | a.fractional;
    const uint32_t rhs = (static_cast<uint32_t> (b.integral) << 8) | b.fractional;
    const uint32_t sum = lhs + rhs;
    uint24c_t r;
    r.integral = static_cast<uint16_t> ((sum >> 8) & 0xFFFF);
    r.fractional = static_cast<uint8_t> (sum & 0xFF);
    r.carry = static_cast<uint8_t> ((sum >> 24) & 1);   // 24-bit overflow flag
    return r;
}

static inline uint24_t U24Add (uint24_t a, uint24_t b)
{
    return U24From32 (U24To32 (a) + U24To32 (b));
}

static inline uint24_t U24Sub (uint24_t a, uint24_t b)
{
    return U24From32 (U24To32 (a) - U24To32 (b));
}

static inline uint24_t U24ShiftRight (uint24_t a)
{
    return U24From32 (U24To32 (a) >> 1);   // integral bit 0 → fractional bit 7
}

static inline uint24_t U24ShiftLeft (uint24_t a)
{
    return U24From32 (U24To32 (a) << 1);  // fractional bit 7 → integral bit 0
}

// 4-bit-balance blends (waveform mip level crossfades in oscillator.cc).
// Decoded from the real header's asm comments/final masks (and matching
// Hellcat's independent translation in Source/dsp/fixed_math.h).
static inline uint8_t U8U4MixU8 (uint8_t a, uint8_t b, uint8_t balance)
{
    return static_cast<uint8_t> (
        (static_cast<int> (a) * (15 - balance) + static_cast<int> (b) * balance) >> 4);
}

static inline uint16_t U8U4MixU12 (uint8_t a, uint8_t b, uint8_t balance)
{
    return static_cast<uint16_t> (
        static_cast<int> (a) * (15 - balance) + static_cast<int> (b) * balance);
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

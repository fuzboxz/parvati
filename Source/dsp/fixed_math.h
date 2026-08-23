// Faithful integer port of avrlib/op.h fixed-point helpers for the Ambika DSP.
//
// Original: ambika_reference/avrlib/op.h (Emilie Gillet, GPL3).
//
// These are the portable C fallbacks (the #else branch of op.h), which are
// mathematically identical to the AVR ASM for every operator here. ALL
// arithmetic stays integer — this is a faithful port, do not float-ify.
//
// Semantics preserved from op.h:
//  * U8Mix uses a >>8 (÷256), NOT ÷255. U8Mix(255,x,0) == 254, not 255.
//    This 8-bit quantisation is intentional and must be kept.
//  * S8U8Mul*/S8S8Mul* : the int8_t operand is sign-extended; muls/mulsu.
//  * U24 ops use a 32-bit intermediate; U24AddC.carry = 24-bit overflow flag.
//  * U14ShiftRight6 / U15ShiftRight7 are plain >>6 / >>7 (C fallback).
//
// The port is complete: every op.h operator is carried, also the ones with
// no current caller. Unused entries stay for firmware parity and for tests.

#ifndef PARVATI_DSP_FIXED_MATH_H
#define PARVATI_DSP_FIXED_MATH_H

#include <cstdint>

#include "dsp/fixed_types.h"

namespace ambika::dsp {

// ---------------------------------------------------------------------------
// Clipping
// ---------------------------------------------------------------------------

// op.h: Clip(value, min, max)
inline int16_t Clip(int16_t value, int16_t lo, int16_t hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

// op.h: S16ClipU8 — clamp to [0,255].
inline uint8_t S16ClipU8(int16_t value) {
    return value < 0 ? uint8_t { 0 }
                     : (value > 255 ? uint8_t { 255 }
                                    : static_cast<uint8_t>(value));
}

// op.h: S16ClipS8 — clamp to [-128,127].
// (The ASM form S16ClipU8(value+128)+128 truncates back to the same range.)
inline int8_t S16ClipS8(int16_t value) {
    return value < -128 ? int8_t { -128 }
                        : (value > 127 ? int8_t { 127 }
                                       : static_cast<int8_t>(value));
}

// op.h: S16ClipU14 — clamp a signed value to [0,16383].
// Bit14 of the unsigned value => saturate high; bit15 (sign) => 0.
inline int16_t S16ClipU14(int16_t value) {
    uint8_t msb = static_cast<uint16_t>(value) >> 8;
    if (msb & 0x80) return 0;        // negative -> 0
    if (msb & 0x40) return 16383;    // > 16383 -> 16383
    return value;
}

// op.h: U8AddClip(value, increment, max) — saturating add.
inline uint8_t U8AddClip(uint8_t value, uint8_t increment, uint8_t max) {
    value = static_cast<uint8_t>(value + increment);
    return value > max ? max : value;
}

// ---------------------------------------------------------------------------
// Bit fiddling
// ---------------------------------------------------------------------------

inline uint8_t U8ShiftRight4(uint8_t a) { return static_cast<uint8_t>(a >> 4); }
inline uint8_t U8ShiftLeft4 (uint8_t a) { return static_cast<uint8_t>(a << 4); }
inline uint8_t U8Swap4      (uint8_t a) { return static_cast<uint8_t>((a << 4) | (a >> 4)); }
inline uint16_t U16ShiftRight4(uint16_t a) { return static_cast<uint16_t>(a >> 4); }

// ---------------------------------------------------------------------------
// Mixers (>>8 unless noted). Keep the ÷256 truncation exactly.
// ---------------------------------------------------------------------------

// result = (a*(255-balance) + b*balance) >> 8   (weight of b = balance)
inline uint8_t U8Mix(uint8_t a, uint8_t b, uint8_t balance) {
    return static_cast<uint8_t>(
        (static_cast<uint16_t>(a) * static_cast<uint8_t>(255 - balance)
         + static_cast<uint16_t>(b) * balance) >> 8);
}

// result = (a*gain_a + b*gain_b) >> 8
inline uint8_t U8Mix(uint8_t a, uint8_t b, uint8_t gain_a, uint8_t gain_b) {
    return static_cast<uint8_t>(
        (static_cast<uint16_t>(a) * gain_a
         + static_cast<uint16_t>(b) * gain_b) >> 8);
}

// Full 16-bit sum, NO >>8.  (a*(255-balance) + b*balance)
inline uint16_t U8MixU16(uint8_t a, uint8_t b, uint8_t balance) {
    return static_cast<uint16_t>(a) * static_cast<uint8_t>(255 - balance)
         + static_cast<uint16_t>(b) * balance;
}

// signed mix: (a*gain_a + b*gain_b) >> 8, a/b signed (mulsu semantics)
inline int8_t S8Mix(int8_t a, int8_t b, uint8_t gain_a, uint8_t gain_b) {
    return static_cast<int8_t>(
        (static_cast<int>(a) * gain_a + static_cast<int>(b) * gain_b) >> 8);
}

// 4-bit balance mixers (used by the vowel synthesiser).
// U8U4MixU8 = (a*(15-balance) + b*balance) >> 4, truncated to 8 bits.
inline uint8_t U8U4MixU8(uint8_t a, uint8_t b, uint8_t balance) {
    return static_cast<uint8_t>(
        (static_cast<int>(a) * (15 - balance) + static_cast<int>(b) * balance) >> 4);
}
// U8U4MixU12 = a*(15-balance) + b*balance  (no shift; ~12 useful bits)
inline uint16_t U8U4MixU12(uint8_t a, uint8_t b, uint8_t balance) {
    return static_cast<uint16_t>(
        static_cast<int>(a) * (15 - balance) + static_cast<int>(b) * balance);
}

// ---------------------------------------------------------------------------
// Fixed-point multiplies
// ---------------------------------------------------------------------------

inline uint8_t  U8U8MulShift8 (uint8_t a, uint8_t b)  { return static_cast<uint8_t> ((static_cast<uint16_t>(a) * b) >> 8); }
inline int8_t   S8U8MulShift8 (int8_t a, uint8_t b)   { return static_cast<int8_t>  ((static_cast<int>(a) * static_cast<int>(b)) >> 8); }  // a signed
inline int16_t  S8U8Mul       (int8_t a, uint8_t b)   { return static_cast<int16_t> (static_cast<int>(a) * static_cast<int>(b)); }            // a signed
inline int16_t  S8S8Mul       (int8_t a, int8_t b)    { return static_cast<int16_t> (static_cast<int>(a) * static_cast<int>(b)); }            // both signed
inline uint16_t U8U8Mul       (uint8_t a, uint8_t b)  { return static_cast<uint16_t>(static_cast<uint16_t>(a) * b); }
inline int8_t   S8S8MulShift8 (int8_t a, int8_t b)    { return static_cast<int8_t>  ((static_cast<int>(a) * static_cast<int>(b)) >> 8); }

// 16x16 scaled by 1/256.
inline uint16_t Mul16Scale8(uint16_t a, uint16_t b) {
    return static_cast<uint16_t>((static_cast<uint32_t>(a) * b) >> 8);
}

// int16 x (u16/u8/s8) >> 8 or >> 16.
inline int16_t S16U8MulShift8 (int16_t a, uint8_t b) {
    return static_cast<int16_t>((static_cast<int32_t>(a) * static_cast<int32_t>(b)) >> 8);
}
inline int16_t S16S8MulShift8 (int16_t a, int8_t b) {
    return static_cast<int16_t>((static_cast<int32_t>(a) * static_cast<int32_t>(b)) >> 8);
}
inline uint16_t U16U8MulShift8(uint16_t a, uint8_t b) {
    return static_cast<uint16_t>((static_cast<uint32_t>(a) * b) >> 8);
}
inline uint16_t U16U16MulShift16(uint16_t a, uint16_t b) {
    return static_cast<uint16_t>((static_cast<uint32_t>(a) * b) >> 16);
}

// ---------------------------------------------------------------------------
// Right-shifts used to reduce wider accumulators to 8 bits.
// ---------------------------------------------------------------------------

inline uint8_t U14ShiftRight6(uint16_t value) { return static_cast<uint8_t>(value >> 6); }  // 14 -> 8
inline uint8_t U15ShiftRight7(uint16_t value) { return static_cast<uint8_t>(value >> 7); }  // 15 -> 8

// ---------------------------------------------------------------------------
// 24-bit fixed ops (32-bit intermediate). carry = 24-bit overflow.
// ---------------------------------------------------------------------------

inline uint24c_t U24AddC(uint24c_t a, uint24_t b) {
    uint32_t av = (static_cast<uint32_t>(a.integral) << 8) | a.fractional;
    uint32_t bv = (static_cast<uint32_t>(b.integral) << 8) | b.fractional;
    uint32_t sum = av + bv;
    uint24c_t r;
    r.carry      = static_cast<uint8_t>(sum >> 24);             // 0 or 1
    r.integral   = static_cast<uint16_t>((sum >> 8) & 0xFFFFu);
    r.fractional = static_cast<uint8_t>(sum & 0xFFu);
    return r;
}

inline uint24_t U24Add(uint24_t a, uint24_t b) {
    uint32_t av = (static_cast<uint32_t>(a.integral) << 8) | a.fractional;
    uint32_t bv = (static_cast<uint32_t>(b.integral) << 8) | b.fractional;
    uint32_t sum = av + bv;
    uint24_t r;
    r.integral   = static_cast<uint16_t>((sum >> 8) & 0xFFFFu);
    r.fractional = static_cast<uint8_t>(sum & 0xFFu);
    return r;
}

// NOTE: the op.h C-fallback has a typo here (references an undefined `sum`);
// implemented as the obvious a-b, wrapping at 24 bits.
inline uint24_t U24Sub(uint24_t a, uint24_t b) {
    uint32_t av = (static_cast<uint32_t>(a.integral) << 8) | a.fractional;
    uint32_t bv = (static_cast<uint32_t>(b.integral) << 8) | b.fractional;
    uint32_t diff = (av - bv) & 0xFFFFFFu;
    uint24_t r;
    r.integral   = static_cast<uint16_t>((diff >> 8) & 0xFFFFu);
    r.fractional = static_cast<uint8_t>(diff & 0xFFu);
    return r;
}

inline uint24_t U24ShiftRight(uint24_t a) {
    uint32_t v = (((static_cast<uint32_t>(a.integral) << 8) | a.fractional) >> 1) & 0xFFFFFFu;
    uint24_t r;
    r.integral   = static_cast<uint16_t>((v >> 8) & 0xFFFFu);
    r.fractional = static_cast<uint8_t>(v & 0xFFu);
    return r;
}

inline uint24_t U24ShiftLeft(uint24_t a) {
    uint32_t v = ((((static_cast<uint32_t>(a.integral) << 8) | a.fractional) << 1)) & 0xFFFFFFu;
    uint24_t r;
    r.integral   = static_cast<uint16_t>((v >> 8) & 0xFFFFu);
    r.fractional = static_cast<uint8_t>(v & 0xFFu);
    return r;
}

// ---------------------------------------------------------------------------
// Linear interpolation into a table that has one extra sample for wrapping
// (the 257-entry bandlimited / sine / env tables). phase in [0,0xFFFF].
// ---------------------------------------------------------------------------

inline uint8_t InterpolateSample(const uint8_t* table, uint16_t phase) {
    uint16_t i = phase >> 8;
    return U8Mix(table[i], table[i + 1], static_cast<uint8_t>(phase & 0xFF));
}

}  // namespace ambika::dsp

#endif  // PARVATI_DSP_FIXED_MATH_H

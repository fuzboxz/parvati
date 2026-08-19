// Unit pins for dsp/fixed_math.h + dsp/random.h (the integer core every
// waveform rides on). All expected values derived from the documented
// semantics: mixers use >>8 (÷256 truncation — U8Mix(255,x,0)==254, NOT 255),
// U24 ops use a 32-bit intermediate with 24-bit wrap, U8AddClip wraps BEFORE
// the saturating compare (faithful avrlib op.h:46-52 port), and InterpolateSample
// reads the extra 257th table entry at the 0xFFFF endpoint.

#include "dsp/fixed_math.h"
#include "dsp/random.h"

#include <cstdio>
#include <cstring>

using namespace ambika::dsp;

static int g_failures = 0;

#define CHECK(cond, msg)                                            \
    do {                                                            \
        if (cond) { printf("  ok  : %s\n", msg); }                  \
        else { printf("  FAIL: %s\n", msg); ++g_failures; }         \
    } while (0)

static uint24_t u24 (uint32_t v)
{
    uint24_t r;
    r.integral   = static_cast<uint16_t>((v >> 8) & 0xFFFFu);
    r.fractional = static_cast<uint8_t>(v & 0xFFu);
    return r;
}

static uint24c_t u24c (uint32_t v)
{
    uint24c_t r;
    r.carry      = 0;
    r.integral   = static_cast<uint16_t>((v >> 8) & 0xFFFFu);
    r.fractional = static_cast<uint8_t>(v & 0xFFu);
    return r;
}

static uint32_t as24 (uint24_t a)
{
    return (static_cast<uint32_t>(a.integral) << 8) | a.fractional;
}

static uint32_t as24 (uint24c_t a)
{
    return (static_cast<uint32_t>(a.integral) << 8) | a.fractional;
}

int main()
{
    printf("[1] Mixers (>>8 division-by-256 truncation)\n");
    CHECK(U8Mix(255, 0, 0) == 254, "U8Mix(255,x,0)==254 (documented /256, not /255)");
    CHECK(U8Mix(0, 255, 255) == 254, "U8Mix balance 255 still truncates to 254");
    CHECK(U8Mix(128, 128, 128) == 127, "U8Mix midpoint 128/128@128 -> 127");
    CHECK(U8Mix(255, 0, 255, 0) == 254, "4-arg U8Mix gain_a=255 truncates to 254");
    CHECK(U8Mix(200, 100, 128, 128) == 150, "4-arg U8Mix (200,100,128,128)==150");
    CHECK(U8MixU16(10, 20, 128) == 3830, "U8MixU16 full 16-bit sum (no >>8)");
    CHECK(S8Mix(-100, 100, 128, 128) == 0, "S8Mix sign-extended cancellation");
    CHECK(U8U4MixU8(200, 100, 8) == 137, "U8U4MixU8 (200,100,8)==137 (>>4)");
    CHECK(U8U4MixU12(200, 100, 8) == 2200, "U8U4MixU12 no-shift sum");

    printf("[2] Clipping\n");
    CHECK(S16ClipU14(16383) == 16383, "S16ClipU14 16383 passthrough");
    CHECK(S16ClipU14(16384) == 16383, "S16ClipU14 16384 saturates to 16383");
    CHECK(S16ClipU14(16385) == 16383, "S16ClipU14 16385 saturates to 16383");
    CHECK(S16ClipU14(-1) == 0, "S16ClipU14 negative -> 0");
    CHECK(S16ClipU14(0) == 0, "S16ClipU14 0 passthrough");
    CHECK(S16ClipU8(-5) == 0 && S16ClipU8(300) == 255 && S16ClipU8(100) == 100,
          "S16ClipU8 range");
    CHECK(S16ClipS8(-200) == -128 && S16ClipS8(200) == 127 && S16ClipS8(-5) == -5,
          "S16ClipS8 range");
    CHECK(Clip(int16_t(5), int16_t(10), int16_t(20)) == 10, "Clip(value,lo,hi)");

    printf("[3] U8AddClip (faithful wrap-before-compare)\n");
    // The avrlib original (op.h:46-52) does `value += increment` on a uint8_t
    // BEFORE comparing to max, so a large increment WRAPS instead of
    // saturating when max==255. The port keeps this byte-for-byte.
    CHECK(U8AddClip(250, 2, 255) == 252, "in-range add");
    CHECK(U8AddClip(251, 2, 252) == 252, "saturates at max");
    CHECK(U8AddClip(250, 10, 255) == 4, "250+10 WRAPS to 4 (uint8 add precedes the compare — faithful)");
    CHECK(U8AddClip(100, 0, 255) == 100, "zero increment passthrough");

    printf("[4] Bit fiddling\n");
    CHECK(U8Swap4(0x12) == 0x21, "U8Swap4 nibble swap");
    CHECK(U8ShiftLeft4(0x0F) == 0xF0, "U8ShiftLeft4");
    CHECK(U8ShiftRight4(0xF0) == 0x0F, "U8ShiftRight4");
    CHECK(U16ShiftRight4(0x1230) == 0x123, "U16ShiftRight4");

    printf("[5] Fixed-point multiplies\n");
    CHECK(U8U8MulShift8(255, 255) == 254, "U8U8MulShift8 255*255>>8 == 254");
    CHECK(S8U8MulShift8(-128, 255) == -128, "S8U8MulShift8 arithmetic shift floors");
    CHECK(S8U8Mul(-100, 3) == -300, "S8U8Mul full product");
    CHECK(S8S8Mul(-100, 3) == -300, "S8S8Mul full product");
    CHECK(S8S8MulShift8(-100, 2) == -1, "S8S8MulShift8 -200>>8 == -1 (floor)");
    CHECK(U8U8Mul(255, 255) == 65025, "U8U8Mul full product");
    CHECK(Mul16Scale8(256, 256) == 256, "Mul16Scale8 exact when low byte 0");
    CHECK(U16U16MulShift16(65535, 65535) == 65534, "U16U16MulShift16 (65535^2)>>65536 == 65534");
    CHECK(U16U8MulShift8(256, 255) == 255, "U16U8MulShift8");
    CHECK(U14ShiftRight6(0x3FC0) == 0xFF, "U14ShiftRight6 (14->8 bits)");
    CHECK(U14ShiftRight6(0x4000) == 0, "U14ShiftRight6 truncates to uint8 (0x4000>>6 == 0x100 -> 0)");
    CHECK(U15ShiftRight7(0x7F80) == 0xFF, "U15ShiftRight7 (15->8 bits)");

    printf("[6] 24-bit accumulator ops\n");
    {
        const auto s = U24AddC(u24c(0xFFFFFFu), u24(1));
        CHECK(s.carry == 1 && as24(s) == 0,
              "U24AddC 0xFFFFFF+1 carries, wraps to 0");
        const auto n = U24AddC(u24c(0x10000u), u24(0x20000u));
        CHECK(n.carry == 0 && as24(n) == 0x30000u, "U24AddC in-range sum");
        const auto sub = U24Sub(u24(0), u24(1));
        CHECK(as24(sub) == 0xFFFFFFu, "U24Sub wraps at 24 bits (never negative)");
        CHECK(as24(U24Sub(u24(5), u24(2))) == 3, "U24Sub in-range");
        CHECK(as24(U24ShiftLeft(u24(0x800000u))) == 0, "U24ShiftLeft drops bit 23");
        CHECK(as24(U24ShiftLeft(u24(0x400000u))) == 0x800000u, "U24ShiftLeft in-range");
        CHECK(as24(U24ShiftRight(u24(0x800000u))) == 0x400000u, "U24ShiftRight");
    }

    printf("[7] InterpolateSample (257-entry tables, 0xFFFF endpoint)\n");
    {
        uint8_t table[257];
        for (int i = 0; i < 256; ++i) table[i] = static_cast<uint8_t>(i);
        table[256] = 255;   // wrap entry (uint8_t: 256 would not fit)
        CHECK(InterpolateSample(table, 0) == 0, "phase 0 -> table[0]");
        CHECK(InterpolateSample(table, 0x8000) == 127,
              "phase 0x8000 balance 0 -> (128*255)>>8 == 127 (>>8 truncation)");
        CHECK(InterpolateSample(table, 0xFFFF) == 254,
              "phase 0xFFFF uses the 257th entry: (255*255)>>8 == 254");
    }

    printf("[8] Random (16-bit Galois LFSR, poly x^16+x^14+x^13+x^11)\n");
    {
        Random r;   // default seed 0x21 (firmware boot seed)
        CHECK(r.state() == 0x21, "default state is the firmware boot seed 0x21");
        CHECK(r.state_msb() == 0x00, "state_msb of 0x21 is 0x00");
        CHECK(r.state() == 0x21, "state_msb() does NOT advance the state");
        r.Update();
        CHECK(r.state() == 0xB410, "seed 0x21 first Update -> 0xB410");
        r.Update();
        CHECK(r.state() == 0x5A08, "second Update (lsb 0) -> 0x5A08");

        Random g;
        const uint8_t b = g.GetByte();
        CHECK(b == 0xB4, "GetByte advances THEN returns msb (0xB4)");
        CHECK(g.state() == 0xB410, "GetByte advanced the state exactly once");

        Random s1;
        s1.Seed(1);
        s1.Update();
        CHECK(s1.state() == 0xB400 && s1.state_msb() == 0xB4, "Seed(1) update -> 0xB400");

        // The shared global singleton advances the same sequence.
        random().Seed(0x21);
        Random ref;
        ref.Seed(0x21);
        bool same = true;
        for (int i = 0; i < 16; ++i)
            same = same && random().GetByte() == ref.GetByte();
        CHECK(same, "global random() tracks a same-seeded instance for 16 draws");
    }

    printf("\n%s (%d failures)\n",
           g_failures ? "FIXED MATH TEST: FAILURES" : "FIXED MATH TEST: ALL CHECKS PASSED",
           g_failures);
    return g_failures ? 1 : 0;
}

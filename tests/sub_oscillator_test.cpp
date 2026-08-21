// Unit pins for dsp/sub_oscillator.h. Uses a 24-bit increment of 0x80000
// (member integral 0x800, fractional 0) so the phase accumulator's top byte
// advances by exactly 8 per sample and WRAPS every 32 samples (32*0x80000 ==
// 2^24) — exercising the square/pulse compare, the triangle fold, the
// shape>=3 octave halving, and the 24-bit wrap inside one 40-sample block.
//
// Per-sample math: v24 = ((k+1)*0x80000) mod 2^24; top byte = 8*(k+1) mod 256.
// Mix: out = (prefill*(255-amount) + v*amount)>>8 (U8Mix, /256 truncation —
// v=255 at amount=255 renders 254, and amount=0 STILL attenuates the dry
// signal by 1/256: 128 -> 127).

#include "dsp/sub_oscillator.h"
#include "unified_test_runner.h"

#include <cstdio>
#include <cstring>

using namespace ambika::dsp;

static int g_failures = 0;

#define CHECK(cond, msg)                                            \
    do {                                                            \
        if (cond) { printf("  ok  : %s\n", msg); }                  \
        else { printf("  FAIL: %s\n", msg); ++g_failures; }         \
    } while (0)

static uint24_t inc8PerSample()
{
    uint24_t inc;
    inc.integral   = 0x800;   // 24-bit value 0x80000 -> +8/sample in the top byte
    inc.fractional = 0;
    return inc;
}

TEST(sub_oscillator_test)
{
    printf("[1] Shape 0 (SQUARE_1, pulse 0x80): compare + 24-bit phase wrap\n");
    {
        SubOscillator sub;
        sub.set_increment (inc8PerSample());
        uint8_t buf[40];
        std::memset (buf, 128, sizeof (buf));
        sub.Render (WAVEFORM_SUB_OSC_SQUARE_1, buf, 255);
        // top byte 8*(k+1): <128 for k<=14, >=128 for k=15..30, wrapped to 8..72 for k=31..39
        CHECK (buf[14] == 0, "sample 14 still low (8*15 == 120 < 128)");
        CHECK (buf[15] == 254, "sample 15 goes high (128 >= 128) -> (255*255)>>8 == 254");
        CHECK (buf[30] == 254, "sample 30 still high (248)");
        CHECK (buf[31] == 0, "sample 31 WRAPS the 24-bit accumulator back to 0 (top byte 8)");
        CHECK (buf[39] == 0, "sample 39 low (top byte 72)");
    }

    printf("[2] Shape 2 (PULSE_1, pulse 0x40): narrower duty + wrap edge\n");
    {
        SubOscillator sub;
        sub.set_increment (inc8PerSample());
        uint8_t buf[40];
        std::memset (buf, 128, sizeof (buf));
        sub.Render (WAVEFORM_SUB_OSC_PULSE_1, buf, 255);
        CHECK (buf[6] == 0, "pulse 0x40: sample 6 low (56 < 64)");
        CHECK (buf[7] == 254, "pulse 0x40: sample 7 high (64 >= 64)");
        CHECK (buf[38] == 0, "sample 38 back low after wrap (56)");
        CHECK (buf[39] == 254, "sample 39 rises again (wrap top byte == 64 >= 64)");
    }

    printf("[3] Shape 1 (TRIANGLE_1): second-half fold\n");
    {
        SubOscillator sub;
        sub.set_increment (inc8PerSample());
        uint8_t buf[40];
        std::memset (buf, 128, sizeof (buf));
        sub.Render (WAVEFORM_SUB_OSC_TRIANGLE_1, buf, 255);
        CHECK (buf[0] == 238, "sample 0: ~tri (tri 16) -> (239*255)>>8 == 238");
        CHECK (buf[14] == 14, "sample 14: ~tri (tri 240) -> (15*255)>>8 == 14");
        CHECK (buf[15] == 0, "sample 15: bit 15 set, tri 256 truncates to 0");
        CHECK (buf[16] == 15, "sample 16: folded rising branch (tri 16) -> 15");
        CHECK (buf[31] == 254, "sample 31: 16-bit integral wrapped to 0 -> bit clear -> ~0 == 255");
    }

    printf("[4] Shapes 3..5 halve the increment (one octave down)\n");
    {
        SubOscillator sub;
        sub.set_increment (inc8PerSample());
        uint8_t buf[40];
        std::memset (buf, 128, sizeof (buf));
        sub.Render (WAVEFORM_SUB_OSC_SQUARE_2, buf, 255);
        // halved inc: top byte 4*(k+1) — <128 for k<=30, >=128 for k=31..39
        CHECK (buf[30] == 0, "SQUARE_2 sample 30 still low (124 < 128)");
        CHECK (buf[31] == 254, "SQUARE_2 crosses 16 samples later than SQUARE_1 (octave down)");
        CHECK (buf[39] == 254, "SQUARE_2 stays high to the end (no wrap at half rate)");
    }

    printf("[5] amount == 0 STILL attenuates the dry signal by 1/256\n");
    {
        SubOscillator sub;
        sub.set_increment (inc8PerSample());
        uint8_t buf[40];
        std::memset (buf, 128, sizeof (buf));
        sub.Render (WAVEFORM_SUB_OSC_SQUARE_1, buf, 0);
        bool all127 = true;
        for (int i = 0; i < 40; ++i) all127 = all127 && buf[i] == 127;
        CHECK (all127, "every sample 128 -> 127 (U8Mix x*(255), v*0 >> 8)");
    }

    printf("[6] Partial amount mixes dry+sub\n");
    {
        SubOscillator sub;
        sub.set_increment (inc8PerSample());
        uint8_t buf[40];
        std::memset (buf, 128, sizeof (buf));
        sub.Render (WAVEFORM_SUB_OSC_SQUARE_1, buf, 64);
        CHECK (buf[0] == 95, "amount 64, v=0: (128*191)>>8 == 95");
        CHECK (buf[15] == 159, "amount 64, v=255: (128*191 + 255*64)>>8 == 159");
    }

    printf("[7] Phase persists across Render calls\n");
    {
        SubOscillator sub;
        sub.set_increment (inc8PerSample());
        uint8_t buf[40];
        std::memset (buf, 128, sizeof (buf));
        sub.Render (WAVEFORM_SUB_OSC_SQUARE_1, buf, 255);   // 40 adds
        std::memset (buf, 128, sizeof (buf));
        sub.Render (WAVEFORM_SUB_OSC_SQUARE_1, buf, 255);   // 80 adds total
        // 80*0x80000 mod 2^24 == 16*0x80000 -> top byte 128 -> high
        CHECK (buf[39] == 254, "block-2 last sample continues the phase (80 adds -> top byte 128)");
    }

    printf("\n%s (%d failures)\n",
           g_failures ? "SUB OSCILLATOR TEST: FAILURES"
                      : "SUB OSCILLATOR TEST: ALL CHECKS PASSED",
           g_failures);
    return g_failures == 0;
}

// Unit pins for dsp/transient_generator.h — the 5 transient (noise-burst)
// generators mixed into the sub-oscillator slot. kAudioBlockSize == 40, so a
// Trigger()'d 255-sample decay spans 7 Render calls (6x40 + 15). All expected
// values hand-derived from the integer math:
//   amplitude = U8U8MulShift8(gain_, amount)  (i.e. (gain*amount)>>8)
//   output    = U8Mix(prefill, value, amplitude) = (p*(255-a) + v*a)>>8

#include "dsp/transient_generator.h"
#include "unified_test_runner.h"

#include <cstdio>
#include <cstring>

using namespace ambika::dsp;
using namespace ambika::dsp;  // enum constants

static int g_failures = 0;

#define CHECK(cond, msg)                                            \
    do {                                                            \
        if (cond) { printf("  ok  : %s\n", msg); }                  \
        else { printf("  FAIL: %s\n", msg); ++g_failures; }         \
    } while (0)

// Render `calls` successive block-slices of a Trigger()'d generator into a
// 280-byte arena (fresh TransientGenerator per call to keep cases isolated),
// returning the number of samples that differ from the prefill sentinel.
static int renderAndCountChanged (uint8_t shape, uint8_t amount, uint8_t prefill, int calls)
{
    uint8_t arena[280];
    std::memset (arena, prefill, sizeof (arena));
    TransientGenerator tg;
    tg.Trigger();
    for (int c = 0; c < calls; ++c)
        tg.Render (shape, arena + 40 * c, amount);
    int changed = 0;
    for (int i = 0; i < 280; ++i)
        if (arena[i] != prefill) ++changed;
    return changed;
}

TEST(transient_generator_test)
{
    printf("[1] Guard clauses\n");
    {
        uint8_t arena[40];
        std::memset (arena, 0xEE, sizeof (arena));
        TransientGenerator tg;
        tg.Trigger();
        tg.Render (WAVEFORM_SUB_OSC_SQUARE_1, arena, 255);   // shape < CLICK: sub-osc's job
        bool untouched = true;
        for (int i = 0; i < 40; ++i) untouched = untouched && arena[i] == 0xEE;
        CHECK (untouched, "shape < WAVEFORM_SUB_OSC_CLICK leaves the buffer untouched");

        std::memset (arena, 0xEE, sizeof (arena));
        TransientGenerator idle;   // never Trigger()ed
        idle.Render (WAVEFORM_SUB_OSC_POP, arena, 255);
        untouched = true;
        for (int i = 0; i < 40; ++i) untouched = untouched && arena[i] == 0xEE;
        CHECK (untouched, "un-Trigger()ed generator is a no-op (counter_ == 0)");
    }

    printf("[2] Trigger arms exactly a 255-sample decay (shape > POP clamps to POP)\n");
    {
        CHECK (renderAndCountChanged (WAVEFORM_SUB_OSC_POP, 255, 200, 1) == 40,
               "call 1 mixes 40 samples");
        CHECK (renderAndCountChanged (WAVEFORM_SUB_OSC_POP, 255, 200, 6) == 240,
               "6 calls mix 240 samples");
        CHECK (renderAndCountChanged (WAVEFORM_SUB_OSC_POP, 255, 200, 7) == 255,
               "7 calls mix EXACTLY 255 samples (240 + 15)");
        CHECK (renderAndCountChanged (WAVEFORM_SUB_OSC_POP, 255, 200, 8) == 255,
               "an 8th call adds nothing (decay finished)");
        CHECK (renderAndCountChanged (99, 255, 200, 7) == 255,
               "hostile shape 99 clamps to POP and behaves identically");
    }

    printf("[3] CLICK: 255-valued burst over the LAST 32 samples, gain ramp 255->0\n");
    {
        uint8_t arena[280];
        std::memset (arena, 0, sizeof (arena));
        TransientGenerator tg;
        tg.Trigger();
        for (int c = 0; c < 7; ++c)
            tg.Render (WAVEFORM_SUB_OSC_CLICK, arena + 40 * c, 255);
        // value == 255 only while the post-decrement counter < 32 (samples 223..254);
        // before that the generator returns 0, which keeps a zero prefill at 0.
        CHECK (arena[0] == 0 && arena[222] == 0, "samples 0..222 are value 0 (pre-dec counter >= 32)");
        CHECK (arena[223] == 30, "sample 223: amplitude 31 x value 255 -> (255*31)>>8 == 30");
        CHECK (arena[250] == 3, "sample 250: amplitude 4 -> (255*4)>>8 == 3");
        CHECK (arena[254] == 0, "sample 254: gain 1 -> amplitude 0 -> 0");
        bool rampOk = true;
        for (int k = 0; k <= 29; ++k)
            rampOk = rampOk && arena[223 + k] == (uint8_t) ((255 * (31 - k)) >> 8);
        CHECK (rampOk, "samples 223..252 ramp 30,29,...,1 (ampl 31-k, (255*ampl)>>8)");
        CHECK (arena[255] == 0 && arena[279] == 0, "samples past the decay stay 0");
    }

    printf("[4] CLICK amount scaling (amplitude = (gain*amount)>>8)\n");
    {
        uint8_t arena[280];
        std::memset (arena, 0, sizeof (arena));
        TransientGenerator tg;
        tg.Trigger();
        for (int c = 0; c < 7; ++c)
            tg.Render (WAVEFORM_SUB_OSC_CLICK, arena + 40 * c, 128);
        // sample 223: gain 32, amount 128 -> amplitude (32*128)>>8 == 16
        CHECK (arena[223] == (uint8_t) ((255 * 16) >> 8), "amount 128 halves the amplitude (16 -> 15)");
    }

    printf("[5] GLITCH: deterministic private 8-bit LCG (rng = rng*73 + counter)\n");
    {
        uint8_t arena[280];
        std::memset (arena, 0, sizeof (arena));
        TransientGenerator tg;   // rng_state_ == 0 at birth
        tg.Trigger();
        tg.Render (WAVEFORM_SUB_OSC_GLITCH, arena, 255);
        // sample 0: rng = 0*73 + 254 == 254, amplitude (255*255)>>8 == 254
        CHECK (arena[0] == 252, "glitch sample 0 == (254*254)>>8 == 252 (rng 0*73+254)");
        // sample 1: rng = 254*73 + 253 mod 256 == 107, amplitude (254*255)>>8 == 253
        CHECK (arena[1] == 105, "glitch sample 1 == (107*253)>>8 == 105");
    }

    printf("[6] METALLIC: gain floor 255 above counter 64, value counter*57\n");
    {
        uint8_t arena[40];
        std::memset (arena, 0, sizeof (arena));
        TransientGenerator tg;
        tg.Trigger();
        tg.Render (WAVEFORM_SUB_OSC_METALLIC, arena, 255);
        // sample 0: counter 255->254, gain 255, value (254*57)&0xFF == 142,
        // amplitude (255*255)>>8 == 254 -> (142*254)>>8 == 140
        CHECK (arena[0] == 140, "metallic sample 0 == (142*254)>>8 == 140");
    }

    printf("\n%s (%d failures)\n",
           g_failures ? "TRANSIENT GENERATOR TEST: FAILURES"
                      : "TRANSIENT GENERATOR TEST: ALL CHECKS PASSED",
           g_failures);
    return g_failures == 0;
}

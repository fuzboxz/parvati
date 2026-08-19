// Unit pins for dsp/envelope.h — the integer ADSR+DEAD envelope. All expected
// values hand-derived against the vendored tables:
//   lut_res_env_portamento_increments[0] == 65535 (instant-ish attack)
//   wav_res_env_expo[0..3] == {0,4,7,11}, [255..256] == {255,255}
// Render() = value_>>8 where value_ = U8MixU16(a_, b_, step) — the /256
// truncation means a 255 target renders as 254/253-scale bytes, never 255.

#include "dsp/envelope.h"
#include "dsp/resources/resources.h"

#include <cstdio>

using namespace ambika::dsp;

static int g_failures = 0;

#define CHECK(cond, msg)                                            \
    do {                                                            \
        if (cond) { printf("  ok  : %s\n", msg); }                  \
        else { printf("  FAIL: %s\n", msg); ++g_failures; }         \
    } while (0)

// Sanity: the table constants the expectations below are derived from.
static void checkTableAssumptions()
{
    CHECK (lut_res_env_portamento_increments[0] == 65535,
           "table assumption: env increment LUT[0] == 65535");
    CHECK (wav_res_env_expo[0] == 0 && wav_res_env_expo[1] == 4,
           "table assumption: expo curve starts 0,4 (slow start)");
    CHECK (wav_res_env_expo[255] == 255 && wav_res_env_expo[256] == 255,
           "table assumption: expo curve tail saturates at 255");
}

int main()
{
    checkTableAssumptions();

    printf("[1] Init(): silent, DEAD, and trigger-ready\n");
    {
        Envelope e;
        e.Init();
        CHECK (e.stage() == DEAD, "fresh Init parks in DEAD");
        CHECK (e.Render() == 0, "fresh Init renders 0");
        e.Update (0, 0, 127, 0);
        e.Trigger (ATTACK);
        CHECK (e.Render() == 253, "after Update+Trigger(ATTACK) it speaks (253)");
    }

    printf("[2] Full stage chain ATTACK->DECAY->SUSTAIN(hold) with sustain 127\n");
    {
        Envelope e;
        e.Init();
        e.Update (0, 0, 127, 0);
        e.Trigger (ATTACK);
        CHECK (e.stage() == ATTACK, "stage ATTACK after Trigger");
        CHECK (e.Render() == 253, "attack render 1 == 253 ((255*254)>>8 of the expo tail)");
        CHECK (e.stage() == ATTACK, "attack spans 2 renders (inc 65535 wraps on the 2nd)");
        CHECK (e.Render() == 253 && e.stage() == DECAY,
               "attack completes on render 2 -> DECAY, output 253 (a=254, step expo[0]=0)");
        CHECK (e.Render() == 253, "decay render 3 == 253 (a=254,b=254)");
        CHECK (e.Render() == 253 && e.stage() == SUSTAIN,
               "decay completes on render 4 -> SUSTAIN, holds 253");
        CHECK (e.Render() == 253 && e.Render() == 253 && e.stage() == SUSTAIN,
               "SUSTAIN increment is 0: value never moves, stage never advances");
    }

    printf("[3] Sustain target = sustain<<1 (127->254 target, 64->128)\n");
    {
        Envelope e;
        e.Init();
        e.Update (0, 0, 64, 0);
        e.Trigger (ATTACK);
        e.Render(); e.Render();          // attack
        CHECK (e.stage() == DECAY, "sustain-64 attack done");
        CHECK (e.Render() == 127, "decay render 3 blends toward 128 target (32766>>8 == 127)");
        CHECK (e.Render() == 127 && e.stage() == SUSTAIN, "SUSTAIN snaps to 128*255>>8 == 127");
        CHECK (e.Render() == 127, "sustain 64 holds 127 (vs 253 at sustain 127)");
    }

    printf("[4] RELEASE -> DEAD\n");
    {
        Envelope e;
        e.Init();
        e.Update (0, 0, 127, 0);
        e.Trigger (ATTACK);
        for (int i = 0; i < 4; ++i) e.Render();   // to SUSTAIN
        e.Trigger (RELEASE);
        CHECK (e.stage() == RELEASE, "stage RELEASE after Trigger");
        CHECK (e.Render() == 0, "release render 1: a=253 scaled by expo tail 254 -> 253>>8 == 0");
        CHECK (e.Render() == 0 && e.stage() == DEAD, "release completes on render 2 -> DEAD");
        CHECK (e.Render() == 0 && e.stage() == DEAD, "DEAD is terminal (renders 0 forever)");
    }

    printf("[5] Trigger(DEAD) forces silence mid-attack\n");
    {
        Envelope e;
        e.Init();
        e.Update (0, 0, 127, 0);
        e.Trigger (ATTACK);
        CHECK (e.Render() == 253, "attack has begun");
        e.Trigger (DEAD);
        CHECK (e.stage() == DEAD, "Trigger(DEAD) moves to DEAD");
        CHECK (e.Render() == 0, "Trigger(DEAD) forces value_ to 0 (Kill path)");
    }

    printf("[6] Never-Updated envelope stays silent (documented Init() rationale)\n");
    {
        Envelope e;   // default-constructed: every stage_phase_increment_ is 0
        e.Trigger (ATTACK);
        CHECK (e.stage() == ATTACK, "stage is ATTACK...");
        CHECK (e.Render() == 0 && e.Render() == 0,
               "...but increment 0 means it renders 0 forever (why Init() primes Update)");
    }

    printf("[7] Non-instant attack ramps through the expo curve\n");
    {
        Envelope e;
        e.Init();
        // attack = 20: increment lut[20] is small -> the first render is a low
        // byte, far below the 253 instant-attack plateau.
        e.Update (20, 0, 127, 0);
        e.Trigger (ATTACK);
        const uint8_t first = e.Render();
        CHECK (first < 64, "slow attack starts low (expo[0..] == 0,4,7,...)");
        CHECK (e.stage() == ATTACK, "slow attack still in ATTACK after one render");
    }

    printf("\n%s (%d failures)\n",
           g_failures ? "ENVELOPE TEST: FAILURES" : "ENVELOPE TEST: ALL CHECKS PASSED",
           g_failures);
    return g_failures ? 1 : 0;
}

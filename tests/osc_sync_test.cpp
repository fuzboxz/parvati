// Hard-sync coverage for dsp/oscillator.{h,cpp} + the Voice OP_SYNC routing.
//
// [1]-[3] drive Oscillator directly: the UPDATE_PHASE contract (a non-zero
// sync_input byte resets the 24-bit phase to 0 BEFORE the increment; a phase
// wrap writes carry=1 into sync_output) and the WAVEFORM_SQUARE parameter==0
// dispatch (RenderSimpleWavetable) vs parameter!=0 (RenderBandlimitedPwm,
// which writes equal sample PAIRS).
//
// [4] drives the real Voice with mix_op == OP_SYNC and compares its rendered
// block against two hand-driven reference Oscillators wired osc1.sync_out ->
// osc2.sync_in — pinning the engine's routing (voice.cpp RenderOscillators)
// end-to-end. Patch bytes are set via offsetof offsets; mod slots
// {LFO_4->PARAM_1, ENV_2->VCA} are zeroed so parameter_/vca stay constant and
// the output is a pure deterministic function of the oscillator phases.

#include "dsp/constants.h"
#include "unified_test_runner.h"
#include "dsp/fixed_math.h"
#include "dsp/fixed_types.h"
#include "dsp/oscillator.h"
#include "dsp/patch.h"
#include "dsp/resources/resources.h"
#include "dsp/resources/resources_manager.h"
#include "dsp/voice.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

using namespace ambika::dsp;

static int g_failures = 0;

// This file keeps its own CHECK macro: it wins over the runner copy.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"

#define CHECK(cond, msg)                                            \
    do {                                                            \
        if (cond) { printf("  ok  : %s\n", msg); }                  \
        else { printf("  FAIL: %s\n", msg); ++g_failures; }         \
    } while (0)
#pragma clang diagnostic pop

static uint24_t inc24 (uint16_t integral)
{
    uint24_t i;
    i.integral   = integral;
    i.fractional = 0;
    return i;
}

TEST(osc_sync_test)
{
    printf("[1] Dirty PWM threshold (no sync): top byte crosses 127+parameter\n");
    {
        Oscillator osc;
        osc.set_parameter (0);   // threshold 127
        uint8_t noSync[40] {}, syncOut[40] {}, buf[40];
        osc.Render (WAVEFORM_DIRTY_PWM, 57, inc24 (0x800), noSync, syncOut, buf);
        // top byte = 8*(k+1): 120 at k=14, 128 at k=15, and the 24-bit
        // accumulator WRAPS at k=31 (32*0x80000 == 2^24) back to top byte 8.
        CHECK (buf[14] == 0, "sample 14 low (120 < 127)");
        CHECK (buf[15] == 255, "sample 15 high (128 >= 127)");
        CHECK (buf[30] == 255, "still high at sample 30 (248)");
        CHECK (buf[31] == 0 && buf[39] == 0,
               "sample 31 wraps the 24-bit accumulator -> low again (8..72)");
    }

    printf("[2] sync_input resets the phase to 0 before the increment\n");
    {
        Oscillator osc;
        osc.set_parameter (0);
        uint8_t noSync[40] {}, syncOut[40] {}, buf[40];
        noSync[8] = 1;   // hard-sync pulse at sample 8
        osc.Render (WAVEFORM_DIRTY_PWM, 57, inc24 (0x800), noSync, syncOut, buf);
        // k>=8: phase restarts -> top byte 8*(k-7): 120 at k=22, 128 at k=23
        CHECK (buf[22] == 0, "reset shifts the crossing: sample 22 low");
        CHECK (buf[23] == 255, "sample 23 high (8 samples later than un-synced)");
    }

    printf("[3] sync_output records the 24-bit carry (wrap pulse)\n");
    {
        Oscillator osc;
        uint8_t noSync[40] {}, syncOut[40] {}, buf[40];
        std::memset (syncOut, 0xEE, sizeof (syncOut));
        osc.Render (WAVEFORM_DIRTY_PWM, 57, inc24 (0x8000), noSync, syncOut, buf);
        bool carryAlternate = true;
        for (int k = 0; k < 40; ++k)
            carryAlternate = carryAlternate && syncOut[k] == (k & 1);
        CHECK (carryAlternate,
               "inc 0x800000 wraps every 2nd sample: carry == k&1 for all 40 samples");
    }

    printf("[4] SQUARE parameter dispatch: 0 -> wavetable, else -> PWM pairs\n");
    {
        Oscillator pw0, pw64;
        pw0.set_parameter (0);
        pw64.set_parameter (64);
        uint8_t noSync[40] {}, syncOut[40] {}, a[40], b[40];
        pw0.Render  (WAVEFORM_SQUARE, 57, inc24 (0x800), noSync, syncOut, a);
        pw64.Render (WAVEFORM_SQUARE, 57, inc24 (0x800), noSync, syncOut, b);
        bool pairsEqual = true, somePairDiffers = false;
        for (int i = 0; i < 40; i += 2)
        {
            pairsEqual    = pairsEqual    && b[i] == b[i + 1];
            somePairDiffers = somePairDiffers || a[i] != a[i + 1];
        }
        CHECK (pairsEqual, "parameter 64 (RenderBandlimitedPwm) writes equal sample PAIRS");
        CHECK (somePairDiffers, "parameter 0 (RenderSimpleWavetable) does NOT pair samples");
        bool inRange = true;
        for (int i = 0; i < 40; ++i) inRange = inRange && a[i] <= 255;   // (uint8_t; also non-const below)
        bool nonConstant = a[0] != a[20];
        CHECK (inRange && nonConstant, "wavetable square output is a real waveform");
    }

    printf("[5] Voice OP_SYNC: osc2 hard-synced to osc1's wrap pulses (end-to-end)\n");
    {
        auto patchByte = [] (Voice& v, size_t off, uint8_t value) {
            v.set_patch_data (static_cast<uint8_t> (off), value);
        };
        const size_t mixOp    = offsetof (Patch, mix_op);
        const size_t mixBal   = offsetof (Patch, mix_balance);
        const size_t modBase  = offsetof (Patch, modulation);
        const size_t modAmt   = offsetof (Modulation, amount);
        const auto oscShapeOff = [] (int i) { return offsetof (Patch, osc) + static_cast<size_t> (i) * sizeof (OscillatorSettings) + offsetof (OscillatorSettings, shape); };
        const auto oscRangeOff = [] (int i) { return offsetof (Patch, osc) + static_cast<size_t> (i) * sizeof (OscillatorSettings) + offsetof (OscillatorSettings, range); };

        const auto configure = [&] (Voice& v) {
            patchByte (v, oscShapeOff (0), WAVEFORM_DIRTY_PWM);   // osc1: sync master, range 31
            patchByte (v, oscRangeOff (0), 31);                   //  -> note ~100: carry pulse every ~15 samples
            patchByte (v, oscShapeOff (1), WAVEFORM_DIRTY_PWM);   // osc2: slave at the base note (69)
            patchByte (v, oscRangeOff (1), 0);
            patchByte (v, mixOp, OP_SYNC);
            // mix_balance 127 (NOT 255): LoadSources seeds dst[MIX_BALANCE] =
            // balance<<8, and ProcessModulationMatrix unconditionally re-clips
            // it with S16ClipU14 — a byte >= 0x80 in the MSB (balance >= 128)
            // reads as negative and clips to 0 (osc1-only). 127 -> 0x7F00 ->
            // 16383 -> osc_2_gain 255 / osc_1_gain 0: an isolated osc2 view.
            patchByte (v, mixBal, 127);
            // Zero {LFO_4->PARAM_1 (slot 7), ENV_2->VCA (slot 11)}: parameter_
            // stays 0 and the VCA never gates, so the rendered bytes are a pure
            // function of the two oscillator phase sequences.
            patchByte (v, modBase + 7 * sizeof (Modulation) + modAmt, 0);
            patchByte (v, modBase + 11 * sizeof (Modulation) + modAmt, 0);
        };

        // Replicate the RenderOscillators pitch->increment math (public
        // constants + LUT) to obtain the two increments the engine derives.
        struct IncNote { uint24_t inc; uint8_t note; };
        const auto forPitch = [] (int16_t pitchIn) -> IncNote {
            int16_t pitch = pitchIn;
            if (pitch >= kHighestNote) pitch = kHighestNote;
            int16_t ref = static_cast<int16_t> (pitch - kPitchTableStart);
            int shifts = 0;
            while (ref < 0) { ref = static_cast<int16_t> (ref + kOctave); ++shifts; }
            uint24_t inc {};
            inc.integral = ResourcesManager::Lookup<uint16_t, uint16_t> (
                lut_res_oscillator_increments, static_cast<uint16_t> (ref >> 1));
            inc.fractional = 0;
            while (shifts-- > 0)
                inc = U24ShiftRight (inc);
            const int m = (pitch >> 7) - 12;
            IncNote r;
            r.inc = inc;
            r.note = static_cast<uint8_t> (m < 0 ? 0 : m);
            return r;
        };

        const int16_t basePitch = 69 * 128;   // Trigger note
        const IncNote o1 = forPitch (static_cast<int16_t> (basePitch + 31 * 128));   // master ~note 100
        const IncNote o2 = forPitch (static_cast<int16_t> (basePitch + 0 * 128));    // slave, note 69

        Voice voice;
        voice.Init();
        configure (voice);
        voice.Trigger (69 * 128, 200, 0);

        Oscillator ref1, ref2;   // fresh instances: phase 0, like the voice's
        uint8_t noSync[40] {}, sync1[40] {}, dummy[40] {}, b1[40], b2[40];
        const uint8_t osc1Gain = 0, osc2Gain = 255;   // mix_balance 127 -> 16383 -> 255/0 (see configure)

        bool allEqual = true;
        int mismatches = 0;
        for (int block = 0; block < 3; ++block)
        {
            voice.ProcessBlock();
            ref1.Render (WAVEFORM_DIRTY_PWM, o1.note, o1.inc, noSync, sync1, b1);
            ref2.Render (WAVEFORM_DIRTY_PWM, o2.note, o2.inc, sync1, dummy, b2);
            for (int i = 0; i < kAudioBlockSize; ++i)
            {
                const unsigned m = (b1[i] * osc1Gain + b2[i] * osc2Gain) >> 8;   // osc mix
                const unsigned s = (m * 255) >> 8;   // sub-osc amount 0 (1/256 dry attenuation)
                const unsigned n = (s * 255) >> 8;   // noise amount 0
                const unsigned a = (n * 255) >> 8;   // fuzz 0
                if (voice.output()[static_cast<size_t> (i)] != a)
                {
                    allEqual = false;
                    if (++mismatches <= 3)
                        printf ("      block %d sample %d: voice %u vs expected %u\n",
                                block, i, voice.output()[static_cast<size_t> (i)], a);
                }
            }
        }
        CHECK (voice.vca() >= 2, "VCA un-gated by the zeroed ENV_2->VCA slot (251)");
        CHECK (allEqual, "voice output == reference mix of osc1 + SYNCED osc2 (120 samples, 3 blocks)");

        // Counterfactual: same patch with OP_SUM leaves osc2 free-running —
        // the synced render must differ somewhere (the check above is not vacuous).
        Voice sum;
        sum.Init();
        configure (sum);
        patchByte (sum, mixOp, OP_SUM);
        sum.Trigger (69 * 128, 200, 0);
        Voice synced;
        synced.Init();
        configure (synced);
        synced.Trigger (69 * 128, 200, 0);
        bool differs = false;
        for (int block = 0; block < 3 && !differs; ++block)
        {
            sum.ProcessBlock();
            synced.ProcessBlock();
            for (int i = 0; i < kAudioBlockSize; ++i)
                differs = differs || sum.output()[static_cast<size_t> (i)] != synced.output()[static_cast<size_t> (i)];
        }
        CHECK (differs, "OP_SYNC vs OP_SUM renders differ (sync routing actually engages)");
    }

    printf("\n%s (%d failures)\n",
           g_failures ? "OSC SYNC TEST: FAILURES" : "OSC SYNC TEST: ALL CHECKS PASSED",
           g_failures);
    return g_failures == 0;
}

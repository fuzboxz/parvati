// Voice pitch-path pins (dsp/voice.cpp RenderOscillators): the kHighestNote
// clamp, the negative-ref_pitch octave-shift reduction, the per-voice pitch
// bend hook, and low-note (midi_note < 0) validity.
//
// Method: the voice patch is set to osc1 = SINE (a note-INDEPENDENT waveform:
// RenderSimpleWavetable selects the same sine table for every midi_note), osc2
// = NONE, OP_SUM, and the two nonzero init-patch mod slots ({LFO_4->PARAM_1,
// ENV_2->VCA}) are zeroed so the VCA never gates and the rendered bytes are a
// pure deterministic function of the oscillator phase sequence. Two voices
// whose engines derive the SAME increment therefore render byte-identical
// blocks — the exact-equality oracle for the clamp and bend checks.

#include "dsp/constants.h"
#include "dsp/patch.h"
#include "dsp/resources/resources.h"
#include "dsp/resources/resources_manager.h"
#include "dsp/voice.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace ambika::dsp;

static int g_failures = 0;

#define CHECK(cond, msg)                                            \
    do {                                                            \
        if (cond) { printf("  ok  : %s\n", msg); }                  \
        else { printf("  FAIL: %s\n", msg); ++g_failures; }         \
    } while (0)

// SINE / NONE / OP_SUM with the time-varying mod slots zeroed (see header).
static void configureDeterministicVoice (Voice& v)
{
    const auto off = [] (size_t o) { return static_cast<uint8_t> (o); };
    v.set_patch_data (off (offsetof (Patch, osc)) + 0, WAVEFORM_SINE);    // osc[0].shape
    v.set_patch_data (off (offsetof (Patch, osc)) + 4, WAVEFORM_NONE);    // osc[1].shape
    v.set_patch_data (off (offsetof (Patch, mix_op)), OP_SUM);
    const size_t modBase = offsetof (Patch, modulation);
    const size_t modAmt  = offsetof (Modulation, amount);
    v.set_patch_data (off (modBase + 7 * sizeof (Modulation) + modAmt), 0);    // LFO_4->PARAM_1
    v.set_patch_data (off (modBase + 11 * sizeof (Modulation) + modAmt), 0);   // ENV_2->VCA
}

static bool blocksEqual (const Voice& a, const Voice& b)
{
    return std::memcmp (a.output(), b.output(), kAudioBlockSize) == 0;
}

static bool blockSilent (const Voice& v)
{
    for (uint8_t i = 0; i < kAudioBlockSize; ++i)
        if (v.output()[i] != 128) return false;
    return true;
}

int main()
{
    printf("[0] Stride precondition from the pitch LUT\n");
    const uint16_t incRef = ResourcesManager::Lookup<uint16_t, uint16_t> (
        lut_res_oscillator_increments, 64);   // note 69 & 81 both reduce to ref 128 -> index 64
    const uint32_t incHigh = static_cast<uint32_t> (incRef) >> 3;   // note 81: 3 octave shifts
    const uint32_t incLow  = static_cast<uint32_t> (incRef) >> 4;   // note 69: 4 octave shifts
    CHECK (2 * incLow == incHigh,
           "2*(lut[64]>>4) == lut[64]>>3 (the two notes are exactly one octave apart)");

    printf("[1] kHighestNote clamp: pitches >= 120*128 render identically\n");
    {
        Voice a, b;
        a.Init(); configureDeterministicVoice (a); a.Trigger (120 * 128, 200, 0);
        b.Init(); configureDeterministicVoice (b); b.Trigger (127 * 128, 200, 0);
        bool equal = true;
        for (int block = 0; block < 6; ++block)
        {
            a.ProcessBlock();
            b.ProcessBlock();
            equal = equal && blocksEqual (a, b);
        }
        CHECK (equal, "notes 120 and 127 produce byte-identical blocks (both clamp to kHighestNote)");
        CHECK (! blockSilent (a), "...and the render is not silent (the equality is meaningful)");
    }

    printf("[2] The clamp is not vacuous: 15359 vs 15360 differ\n");
    {
        Voice a, b;
        a.Init(); configureDeterministicVoice (a); a.Trigger (15360, 200, 0);   // == kHighestNote
        b.Init(); configureDeterministicVoice (b); b.Trigger (15359, 200, 0);   // one unit below
        bool differs = false;
        for (int block = 0; block < 3 && !differs; ++block)
        {
            a.ProcessBlock();
            b.ProcessBlock();
            differs = ! blocksEqual (a, b);
        }
        CHECK (differs, "one 1/128-semitone below the clamp sounds different (lut[255] != lut[256])");
    }

    printf("[3] set_pitch_bend_offset(+128) == exactly +1 semitone\n");
    {
        Voice a, b;
        a.Init(); configureDeterministicVoice (a);
        a.Trigger (69 * 128, 200, 0);
        a.set_pitch_bend_offset (128);
        b.Init(); configureDeterministicVoice (b); b.Trigger (70 * 128, 200, 0);
        bool equal = true;
        for (int block = 0; block < 6; ++block)
        {
            a.ProcessBlock();
            b.ProcessBlock();
            equal = equal && blocksEqual (a, b);
        }
        CHECK (equal, "bend +128 on note 69 == note 70 (byte-identical, bypasses the mod matrix)");
    }

    printf("[4] Negative-ref_pitch octave loop: note 69 is exactly one octave below 81\n");
    {
        // Notes 69 and 81 both reduce to pitch-table ref 128; the engine derives
        // increments related by exactly 2:1, so (sample k carries the phase
        // after k+1 adds) the LOW voice's ODD-indexed samples reproduce the
        // HIGH voice's stream sample-for-sample.
        Voice low, high;
        low.Init();  configureDeterministicVoice (low);  low.Trigger  (69 * 128, 200, 0);
        high.Init(); configureDeterministicVoice (high); high.Trigger (81 * 128, 200, 0);
        uint8_t lowStream[2 * kAudioBlockSize];
        uint8_t highStream[kAudioBlockSize];
        low.ProcessBlock();
        std::memcpy (lowStream, low.output(), kAudioBlockSize);
        low.ProcessBlock();
        std::memcpy (lowStream + kAudioBlockSize, low.output(), kAudioBlockSize);
        high.ProcessBlock();
        std::memcpy (highStream, high.output(), kAudioBlockSize);
        CHECK (! blockSilent (high), "high (note 81) renders audibly");
        CHECK (! blockSilent (low), "low (note 69) renders audibly");
        bool stride = true;
        for (int i = 0; i < kAudioBlockSize; ++i)
            stride = stride && lowStream[2 * i + 1] == highStream[i];
        CHECK (stride, "low.out[2i+1] == high.out[i] for all 40 samples (exact octave relation)");
    }

    printf("[5] Low-note region (midi_note < 0 clamps to 0) renders clean\n");
    {
        // Pitches 0 / 640 / 1535 are below MIDI 12: (pitch>>7)-12 < 0 clamps the
        // wavetable zone index to note 0, while the octave loop keeps deriving a
        // valid increment. These render non-silent, in-range blocks.
        bool ok = true;
        for (const int pitch : { 0, 640, 1535 })
        {
            Voice v;
            v.Init(); configureDeterministicVoice (v); v.Trigger (static_cast<uint16_t> (pitch), 200, 0);
            v.ProcessBlock(); v.ProcessBlock();
            ok = ok && ! blockSilent (v);
        }
        CHECK (ok, "pitches 0 / 640 / 1535 all render non-silent (clamped-note zone is valid)");
        // Distinctness: pitch 0 vs pitch 1535 (MIDI 11.99) both clamp midi_note
        // to 0 but derive different increments (lut[256]>>10 = 13 vs lut[255]>>9 = 27).
        Voice x, y;
        x.Init(); configureDeterministicVoice (x); x.Trigger (0, 200, 0);
        y.Init(); configureDeterministicVoice (y); y.Trigger (1535, 200, 0);
        x.ProcessBlock(); x.ProcessBlock();
        y.ProcessBlock(); y.ProcessBlock();
        CHECK (! blocksEqual (x, y), "pitch 0 vs 1535 in the clamped-note region are distinguished");
    }

    printf("\n%s (%d failures)\n",
           g_failures ? "VOICE PITCH TEST: FAILURES" : "VOICE PITCH TEST: ALL CHECKS PASSED",
           g_failures);
    return g_failures ? 1 : 0;
}

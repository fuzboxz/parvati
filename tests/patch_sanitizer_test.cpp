// patch_sanitizer_test — regression cover for the ingestion-boundary
// normalization layer (Source/dsp/patch_sanitizer.h, wired at
// PluginProcessor::loadMultiFile [.MUL] and SynthEngine::restoreState
// [host-state blob]). Two layers of proof:
//
//   [1] UNIT — every sanitized field: hostile bytes clamp to the firmware
//       domain, in-domain bytes are the identity (corrections == 0), so
//       legitimate files round-trip byte-exactly.
//   [2] WIRING — end-to-end through the REAL ingestion paths: a hostile .MUL
//       written by the real writer and loaded by loadMultiFile lands in the
//       engine SANITIZED; a hostile engine-state blob captured/restored by
//       captureState/restoreState lands SANITIZED.
//
// The DSP-side sink clamps (2026-08-18 bug hunts) stay in place as defense in
// depth — this test pins the boundary so the sink clamps become the SECOND
// line, not the only one.

#include <cstdio>
#include <cstring>

#include "dsp/patch_sanitizer.h"
#include "dsp/voice.h"
#include "PatchFile.h"

#include "test_utils.h"
#include "unified_test_runner.h"

using namespace ambika::dsp;

TEST(patch_sanitizer_test)
{
    // ------------------------------------------------------------------
    // [1a] Patch: an all-0xFF patch clamps every sanitized field to its
    //      domain ceiling; free fields (gains, int8 pitch math, padding)
    //      stay 0xFF.
    // ------------------------------------------------------------------
    printf("[1a] sanitizePatch: hostile 0xFF patch clamps to domains\n");
    {
        ambika::dsp::Patch p;
        std::memset (&p, 0xFF, sizeof (p));   // every byte hostile

        const size_t fixed = sanitizePatch (p);
        CHECK (fixed > 0, "hostile patch reports corrections");

        // osc shapes: 0xFF -> WAVEFORM_WAVEQUENCE (37)
        CHECK (p.osc[0].shape == WAVEFORM_WAVEQUENCE
                   && p.osc[1].shape == WAVEFORM_WAVEQUENCE,
               "osc shape 0xFF -> WAVEQUENCE (dispatch-table domain)");
        // mix op / sub-osc shape
        CHECK (p.mix_op == OP_LAST - 1, "mix_op 0xFF -> OP_LAST-1");
        CHECK (p.mix_sub_osc_shape == WAVEFORM_SUB_OSC_LAST - 1,
               "mix_sub_osc_shape 0xFF -> SUB_OSC_LAST-1");
        // filter modes: 0xFF -> NOTCH (3)
        CHECK (p.filter[0].mode == FILTER_MODE_NOTCH
                   && p.filter[1].mode == FILTER_MODE_NOTCH,
               "filter mode 0xFF -> NOTCH");
        // env/lfo segments
        for (int i = 0; i < 3; ++i)
        {
            CHECK (p.env_lfo[i].attack == 127 && p.env_lfo[i].decay == 127
                       && p.env_lfo[i].sustain == 127 && p.env_lfo[i].release == 127,
                   "env_lfo a/d/s/r 0xFF -> 127 (LUT domain)");
            CHECK (p.env_lfo[i].shape == LFO_WAVEFORM_LAST - 1,
                   "env_lfo shape 0xFF -> LFO_WAVEFORM_LAST-1");
            CHECK (p.env_lfo[i].rate == kNumSyncedLfoRates + 127,
                   "env_lfo rate 0xFF -> 142 (15 synced + 128 free)");
        }
        CHECK (p.voice_lfo_shape == LFO_WAVEFORM_RAMP,
               "voice_lfo_shape 0xFF -> RAMP (voicecard domain)");
        CHECK (p.voice_lfo_rate == kNumSyncedLfoRates + 127,
               "voice_lfo_rate 0xFF -> 142");
        // mod matrix
        for (int i = 0; i < kNumModulations; ++i)
        {
            CHECK (p.modulation[i].source == MOD_SRC_LAST - 1,
                       "mod source 0xFF -> MOD_SRC_LAST-1");
            CHECK (p.modulation[i].destination == MOD_DST_LAST - 1,
                       "mod destination 0xFF -> MOD_DST_LAST-1 (the OOB-write class)");
        }
        // modifiers
        for (int i = 0; i < kNumModifiers; ++i)
        {
            CHECK (p.modifier[i].operands[0] == MOD_SRC_LAST - 1
                       && p.modifier[i].operands[1] == MOD_SRC_LAST - 1,
                   "modifier operands 0xFF -> MOD_SRC_LAST-1");
            CHECK (p.modifier[i].op == MODIFIER_LAST - 1, "modifier op 0xFF -> MODIFIER_LAST-1");
        }
        // FREE fields stay byte-exact (0xFF): gains + int8 pitch math.
        CHECK (p.osc[0].parameter == 0xFF && p.mix_balance == 0xFF
                   && p.filter[0].cutoff == 0xFF && p.filter[0].resonance == 0xFF,
               "free gain fields untouched (0xFF preserved)");
        CHECK (p.osc[0].detune == static_cast<int8_t> (0xFF)
                   && p.filter_env == static_cast<int8_t> (0xFF),
               "free int8 fields untouched (sign preserved)");
    }

    // ------------------------------------------------------------------
    // [1b] Identity: an in-domain patch is untouched (0 corrections) —
    //      save/load round-trips stay byte-exact.
    // ------------------------------------------------------------------
    printf("[1b] sanitizePatch: in-domain patch is the identity\n");
    {
        ambika::dsp::Patch p {};
        p.osc[0].shape = WAVEFORM_SAW;
        p.osc[0].parameter = 200;
        p.osc[1].shape = WAVEFORM_WAVETABLE_16;   // 36: the domain ceiling
        p.mix_op = OP_BITS;
        p.mix_sub_osc_shape = WAVEFORM_SUB_OSC_POP;
        p.filter[0].mode = FILTER_MODE_HP;
        p.env_lfo[0] = { 127, 127, 127, 127, LFO_WAVEFORM_LAST - 1,
                         kNumSyncedLfoRates + 127, 0, 1 };
        p.voice_lfo_shape = LFO_WAVEFORM_RAMP;
        p.voice_lfo_rate = 0;
        for (auto& m : p.modulation) { m.source = MOD_SRC_LAST - 1; m.destination = MOD_DST_LAST - 1; m.amount = -63; }
        for (auto& mod : p.modifier) { mod.operands[0] = MOD_SRC_LAST - 1; mod.operands[1] = 0; mod.op = MODIFIER_LAST - 1; }

        CHECK (sanitizePatch (p) == 0, "fully in-domain patch: 0 corrections (identity)");
    }

    // ------------------------------------------------------------------
    // [1c] PartData: hostile bytes clamp per the byte map; free bytes stay.
    // ------------------------------------------------------------------
    printf("[1c] sanitizePartData: hostile PartData clamps per the byte map\n");
    {
        std::array<uint8_t, 84> pd {};
        pd.fill (0xFF);
        // byte 0 volume free; 4 tuning -> 32; 6 portamento -> 127;
        // arp block: mode->2, dir->5, octave->4 (0xFF clamps DOWN to 4),
        // pattern->21, resolution->14; seq lengths->16; polyphony->4.
        const size_t fixed = sanitizePartData (pd);
        CHECK (fixed > 0, "hostile PartData reports corrections");
        CHECK (pd[0] == 0xFF, "[0] volume free (0xFF kept)");
        CHECK (pd[4] == 32, "[4] tuning/raga 0xFF -> 32");
        CHECK (pd[6] == 127, "[6] portamento 0xFF -> 127");
        CHECK (pd[7] == 2 && pd[8] == 5 && pd[9] == 4 && pd[10] == 21 && pd[11] == 14,
               "[7..11] arp block clamped (mode/dir/octave/pattern/resolution)");
        CHECK (pd[12] == 16 && pd[13] == 16 && pd[14] == 16, "[12..14] seq lengths -> 16");
        CHECK (pd[15] == 4, "[15] polyphony 0xFF -> 4");
        CHECK (pd[16] == 0xFF && pd[79] == 0xFF, "[16..79] seq step data free");
        CHECK (pd[80] == 0xFF && pd[83] == 0xFF, "[80..83] reserved free");

        // The zero-octave audio-thread hang class: arpOctave 0 -> 1.
        std::array<uint8_t, 84> hang {};
        hang[9] = 0;
        sanitizePartData (hang);
        CHECK (hang[9] == 1, "[9] arpOctave 0 -> 1 (the Random-wrap hang class)");

        // Identity for an in-domain PartData.
        std::array<uint8_t, 84> okPd {};
        okPd[4] = 17; okPd[6] = 40;
        okPd[7] = 1; okPd[8] = 3; okPd[9] = 2; okPd[10] = 5; okPd[11] = 9;
        okPd[12] = 1; okPd[13] = 16; okPd[14] = 8; okPd[15] = 0;
        CHECK (sanitizePartData (okPd) == 0, "in-domain PartData: 0 corrections (identity)");
    }

    // ------------------------------------------------------------------
    // [2a] WIRING (.MUL): hostile bytes in a real written file land in the
    //      engine SANITIZED via ParvatiAudioProcessor::loadMultiFile.
    // ------------------------------------------------------------------
    printf("[2a] .MUL ingestion wiring (loadMultiFile sanitizes)\n");
    {
        AmbikaMulti multi;
        multi.name = "hostile";
        for (auto& mp : multi.parts)
        {
            mp.hasPatch = true;
            mp.hasPart = true;
            mp.patch.fill (0xFF);   // every Patch byte hostile
            mp.part.fill (0xFF);    // every PartData byte hostile
        }
        multi.hasMultiData = true;   // zeroed MultiData: channel 0, full range
        multi.multiData.fill (0);

        juce::File tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("parvati_sanitizer_hostile.mul");
        CHECK (writeAmbikaMultiFile (tmp, multi), "hostile .MUL written by the real writer");

        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);
        CHECK (proc.loadMultiFile (tmp), "loadMultiFile accepts the well-formed hostile .MUL");

        auto& part0 = proc.getEngine().getPart (0);
        std::array<uint8_t, 112> got {};
        part0.patchBytes.copyTo (got);
        // Reinterpret as Patch and check the sanitized fields (offsets 0/9/11/18:
        // osc[0].shape / mix_op / mix_sub_osc_shape / filter[0].mode).
        CHECK (got[0] == WAVEFORM_WAVEQUENCE && got[9] == OP_LAST - 1
                   && got[11] == WAVEFORM_SUB_OSC_LAST - 1 && got[18] == FILTER_MODE_NOTCH,
               "engine patchBytes sanitized after .MUL load (shape/mixOp/subShape/mode)");
        CHECK (got[10] == 0xFF, "free mix_parameter byte still 0xFF after .MUL load");

        std::array<uint8_t, 84> pd {};
        part0.partBytes.copyTo (pd);
        CHECK (pd[9] == 4 && pd[12] == 16 && pd[15] == 4,
               "engine partBytes sanitized after .MUL load (arpOctave/seqLen/polyphony)");

        tmp.deleteFile();
    }

    // ------------------------------------------------------------------
    // [2b] WIRING (host-state blob): hostile engine bytes captured into a
    //      state blob land SANITIZED after restoreState.
    // ------------------------------------------------------------------
    printf("[2b] host-state ingestion wiring (restoreState sanitizes)\n");
    {
        ParvatiAudioProcessor a;
        a.prepareToPlay (48000.0, 256);

        // Inject hostile bytes directly into the engine's storage (the same MT
        // path the setters use), then capture: the blob now carries the hostile
        // bytes verbatim (captureState writes patchBytes byte-exact).
        std::array<uint8_t, 112> hostilePatch {};
        hostilePatch.fill (0xFF);
        a.getEngine().getPart (1).patchBytes = hostilePatch;

        juce::MemoryBlock blob;
        a.getEngine().captureState (blob);

        ParvatiAudioProcessor b;
        b.prepareToPlay (48000.0, 256);
        CHECK (b.getEngine().restoreState (blob.getData(), blob.getSize()),
               "restoreState accepts the captured blob");

        std::array<uint8_t, 112> got {};
        b.getEngine().getPart (1).patchBytes.copyTo (got);
        CHECK (got[0] == WAVEFORM_WAVEQUENCE && got[9] == OP_LAST - 1
                   && got[50] == MOD_SRC_LAST - 1,
               "restored patchBytes sanitized (osc shape / mix op / mod source)");
    }

    return true;
}

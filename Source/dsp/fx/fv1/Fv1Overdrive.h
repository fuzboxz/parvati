// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.
//
// Fv1Overdrive — FV-1-style classic tube overdrive: Drive pre-gain into a
// 1024-entry asymmetric 12AX7-ish soft-clip wavetable (the FV-1 "external
// EEPROM table" idiom), Bias shifts the read index for even harmonics, Tone
// one-pole LP, Level. JUCE-FREE (Fv1FxProcessor/Fv1Engine only).
//
// ANTI-CRACKLE OVERSAMPLING (2026-08-17): the table stage runs INSIDE a 6x
// oversampled domain (the vendored Warps polyphase FIR the Wavefolder uses —
// SampleRateConverter<SRC_UP/DOWN,6,48>). A hard-nonlinear transfer curve
// generates harmonics far above the 16.384 kHz internal Nyquist, which FOLD
// back as inharmonic components (measured: at a 3 kHz input the worst folded
// spur was only ~16 dB below the fundamental — audible as a crackle/fizz
// burst on every note attack, when the envelope peak drives the curve
// deepest). 6x-oversampling the memoryless table evaluation moves the fold
// point to 98 kHz and attenuates the images with a real FIR: measured worst
// inharmonic spur drops to <= -46 dB at any input frequency. The Drive/LUT
// evaluation itself stays EXACTLY the Q.23 saturating fixed-point path
// (converted per 6x sample); only the linear rate conversion around it is
// float, exactly like the Wavefolder/RingModulator slots. Tone LP + Level
// stay at the 1x internal rate (linear stages do not alias).
//
// Params (param[4] is UNUSED; Mix is the chain Dry/Wet):
//   * Drive  (p0): 1..16x pre-gain (log) into the table.
//   * Bias   (p1): -0.3..+0.3 index offset (asymmetry / even harmonics).
//   * Tone   (p2): 700..15000 Hz post-LP.
//   * Level  (p3): 0..2 output trim (0.5 at center).

#ifndef HELLCAT_DSP_FX_FV1_FV1OVERDRIVE_H
#define HELLCAT_DSP_FX_FV1_FV1OVERDRIVE_H

#include <cstdint>
#include <vector>

#include "dsp/fx/fv1/Fv1FxProcessor.h"
#include "warps/dsp/sample_rate_converter.h"   // 6x polyphase FIR (shaper OS)

namespace hellcat::fv1
{

class Fv1Overdrive : public Fv1FxProcessor
{
public:
    Fv1Overdrive();

    void setParams (const std::array<float, kNumFxSlotParams>& param) override;
    void prepareInternal (double sampleRate, int maxBlock) override;
    void resetInternal() override;
    // 6x-oversampled table path (see the file header): upsample the internal
    // block 6x, run the Q.23 shaper per oversampled sample, downsample, then
    // apply Tone + Level at 1x.
    void process (float* L, float* R, int numSamples) override;
    // The 6x OS pair's group delay (SRC_UP 4 + SRC_DOWN 4 = 8 INTERNAL
    // samples), converted to HOST samples in prepareInternal so the chain's
    // dry/wet + parallel blends can align wet against dry (same contract as
    // FxWavefolder/FxRingModulator). 0 before the first prepare (the
    // stage-time latency snapshot then sees a passthrough, refreshed live by
    // the chain's re-prepare).
    int latency() const noexcept override { return latencyHost_; }
    FxType type() const noexcept override { return FxType::Overdrive; }

protected:
    // The PURE shaper: Drive pre-gain + Bias + table lookup. Used at the 6x
    // rate by process(); no Tone/Level here (they run at 1x in process()).
    void processSampleFx (int32_t lin, int32_t rin, int32_t& lout, int32_t& rout) override;

private:
    static constexpr int kTableSize = 1024;   // 10-bit index, domain [-4,4)

    // The tube curve (int16 Q.14), built once in the ctor ("EEPROM table").
    int16_t table_[kTableSize];

    // 6x oversampling of the nonlinear table stage (see the file header).
    warps::SampleRateConverter<warps::SRC_UP, 6, 48>   srcUpL_, srcUpR_;
    warps::SampleRateConverter<warps::SRC_DOWN, 6, 48> srcDownL_, srcDownR_;
    std::vector<float> osL_, osR_;   // 6x scratch (sized in prepareInternal)

    OnePoleLpFx toneLp_;      // post-LP (1x)
    int16_t drive14_ = 4096;  // q14 fractional pre-gain
    int driveShift_  = 0;     // integer 2x stages of the pre-gain (1..16x)
    int biasIdx_     = 0;     // Bias index offset (±38 idx = ±0.3 table domain)
    // Level (p3, documented 0..2) split exactly like the compressor's gain:
    // q14 tops out at ~1.0, so the integer 2x stage lives here and only the
    // fractional remainder is quantized. (The old q14(p3*2) clamped every
    // p3 > ~0.5 to unity — the upper half of the knob had no effect.)
    int16_t level14_ = 8192;  // q14 fractional trim (0..1); 8192 = unity
    int levelShift_  = 0;     // 0/1 extra x2 stage when Level > 1

    // One-pole ~10 Hz high-pass DC blocker on the wet output (kills the
    // bias-DC and the curve-asymmetry DC; the Tone LP passes both).
    float dcX1_ = 0.0f, dcY1_ = 0.0f;

    // 6x OS group delay in HOST samples (captured in prepareInternal).
    int latencyHost_ = 0;
};

} // namespace hellcat::fv1

#endif // HELLCAT_DSP_FX_FV1_FV1OVERDRIVE_H

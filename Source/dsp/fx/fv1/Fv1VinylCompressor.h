// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Fv1VinylCompressor — Lo-Fi Vinyl Compressor, part of the FV-1 hardware-
// emulation FX family. Signal path:
//   feed-forward peak compressor (sidechain in float, gain applied in
//   fixed-point) -> pitch-warp (dual sine-LFO modulated) 50 ms delay -> age
//   low-pass -> LCG crackle mixed at the output.
//
// JUCE-FREE: only the FV-1/framework headers + <cstdint>, so it compiles
// standalone in seconds for its unit test, exactly like the rest of the family.

#ifndef PARVATI_DSP_FX_FV1_FV1VINYLCOMPRESSOR_H
#define PARVATI_DSP_FX_FV1_FV1VINYLCOMPRESSOR_H

#include <cstdint>

#include "dsp/fx/fv1/Fv1FxProcessor.h"

namespace parvati::fv1
{

// Params (generic 0..1 slot params; param[4] is the chain dry/wet, never read):
//   param[0] Compress : threshold th = 1 - 0.9*p (lowers), makeup mg = 1 + 3*p
//                       (max 4.0). Ratio fixed ~4:1; feed-forward peak.
//   param[1] Pitch    : dual sine-LFO (0.5 Hz + 4.0 Hz) depth 0..~3 samples on
//                       the 50 ms (= 1638 sample) delay read pointer.
//   param[2] Crackle  : output level of an LCG crackle (click when value > 0.98).
//   param[3] Age      : 1000..15000 Hz 1-pole LP cutoff (fc = 1000*15^p).
class Fv1VinylCompressor : public Fv1FxProcessor
{
public:
    Fv1VinylCompressor() = default;

    void setParams (const float param[5]) override;
    void prepareInternal (double sampleRate, int maxBlock) override;
    void resetInternal() override;
    FxType type() const noexcept override;

private:
    void processSampleFx (int32_t lin, int32_t rin,
                          int32_t& lout, int32_t& rout) override;

    // Cached, clamped 0..1 params (param[4] is the chain dry/wet — never read).
    float pCompress_ = 0.0f;
    float pPitch_    = 0.0f;
    float pCrackle_  = 0.0f;
    float pAge_      = 0.0f;

    // Compressor sidechain (float detector; the gain is applied in fixed-point).
    float env_     = 0.0f;
    float th_      = 1.0f;   // 1 - 0.9*pCompress
    float makeup_  = 1.0f;   // 1 + 3*pCompress  (max 4.0)
    // Attack/release one-pole coefficients (fixed at the FV-1 internal rate).
    float attackA_  = 0.0f;  // 1 - exp(-1/(0.002*32768))   (2 ms attack)
    float releaseA_ = 0.0f;  // 1 - exp(-1/(0.150*32768))   (150 ms release)

    // Pitch-warp modulated delay (50 ms = 1638 samples at 32.768 kHz).
    DelayLine<2048> delay_;
    float ph1_ = 0.0f;       // 0.5 Hz sine LFO phase
    float ph2_ = 0.0f;       // 4.0 Hz sine LFO phase

    // Age low-pass (1-pole, fixed-point; cutoff = Age param).
    OnePoleLpFx ageLp_;

    // Crackle LCG (glibc-style constants; 32-bit unsigned wraps mod 2^32).
    uint32_t lcgState_ = 0u;
};

} // namespace parvati::fv1

#endif // PARVATI_DSP_FX_FV1_FV1VINYLCOMPRESSOR_H

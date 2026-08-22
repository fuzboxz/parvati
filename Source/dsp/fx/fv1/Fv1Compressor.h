// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Fv1Compressor — the bread-and-butter clean feed-forward peak compressor
// (the non-vinyl sibling of the Vinyl Compressor: no wow, no noise, no
// saturation — just leveling). Float sidechain (the FV-1 LOG/EXP equivalent);
// gain applied in fixed point exactly like the family contract.
//
// Params (param[4] is UNUSED; Mix is the chain Dry/Wet):
//   * Amount  (p0): threshold 1.0 -> 0.05, ratio 2:1 -> 10:1, makeup 1 -> 3.
//   * Attack  (p1): 0.5..50 ms (log).
//   * Release (p2): 20..500 ms (log).
//   * Level   (p3): 0..2 output trim (1.0 at center).

#ifndef PARVATI_DSP_FX_FV1_FV1COMPRESSOR_H
#define PARVATI_DSP_FX_FV1_FV1COMPRESSOR_H

#include <cstdint>

#include "dsp/fx/fv1/Fv1FxProcessor.h"

namespace parvati::fv1
{

class Fv1Compressor : public Fv1FxProcessor
{
public:
    void setParams (const std::array<float, kNumFxSlotParams>& param) override;
    void prepareInternal (double sampleRate, int maxBlock) override;
    void resetInternal() override;
    FxType type() const noexcept override { return FxType::Compressor; }

protected:
    void processSampleFx (int32_t lin, int32_t rin, int32_t& lout, int32_t& rout) override;

private:
    // Float sidechain state.
    float env_      = 0.0f;
    float th_       = 1.0f;
    float ratioExp_ = 0.5f;    // gain = (th/env)^exp
    float makeup_   = 1.0f;
    float attackA_  = 0.0f;
    float releaseA_ = 0.0f;
    int16_t level14_ = 8192;  // q14 fractional trim (0..1); 8192 = unity
    int levelShift_ = 0;      // 0/1 extra x2 stage when Level > 1 (q14 alone
                              // tops out at ~1.0 — the old q14(p3*2) clamped
                              // every p3 > ~0.5 to unity: dead upper half)
};

} // namespace parvati::fv1

#endif // PARVATI_DSP_FX_FV1_FV1COMPRESSOR_H

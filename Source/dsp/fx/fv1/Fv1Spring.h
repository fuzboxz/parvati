// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Fv1Spring — a dispersive-network spring reverb: each spring is a feedback
// loop of [long delay -> cascade of SIX short allpasses -> damping LP], with a
// soft-clipped driver. The short-AP cascade IS the dispersion — transients
// come out chirping ("boing"), the classic spring signature no plain delay
// loop has. Two springs with slightly different lengths/AP orders give
// natural stereo; Width blends the second spring in.
//
// Params (param[4] is UNUSED; Mix is the chain Dry/Wet):
//   * Decay (p0): 0.2..4 s -> loop feedback.
//   * Damp  (p1): 500..8000 Hz loop LP.
//   * Chirp (p2): AP coefficient 0.35..0.95 (dispersion strength = boing).
//   * Width (p3): 0 = one spring (mono), 1 = both springs (stereo).

#ifndef PARVATI_DSP_FX_FV1_FV1SPRING_H
#define PARVATI_DSP_FX_FV1_FV1SPRING_H

#include <cstdint>

#include "dsp/fx/fv1/Fv1FxProcessor.h"

namespace parvati::fv1
{

class Fv1Spring : public Fv1FxProcessor
{
public:
    void setParams (const float param[5]) override;
    void resetInternal() override;
    FxType type() const noexcept override { return FxType::Spring; }

protected:
    void processSampleFx (int32_t lin, int32_t rin, int32_t& lout, int32_t& rout) override;

private:
    static constexpr int kSpringDelay[2] = { 1146, 1123 };          // ~35 ms loops
    static constexpr int kApLen[2][6] = { { 23, 29, 31, 37, 41, 43 },
                                          { 21, 27, 33, 39, 43, 47 } };
    DelayLine<2048> delay_[2];
    DelayLine<64> aps_[2][6];
    OnePoleLpFx damp_[2];

    int16_t fb14_     = 0;
    int16_t chirp14_  = 0;
    int16_t width14_  = 0;
};

} // namespace parvati::fv1

#endif // PARVATI_DSP_FX_FV1_FV1SPRING_H

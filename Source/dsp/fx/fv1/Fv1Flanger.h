// Copyright (c) 2026 805Labs Kft. / Hellcat.
//
// Fv1Flanger — jet-style flanger: a SHORT modulated delay (0.15..6 ms) with
// up to 0.92 feedback and a fixed 8 kHz loop damper for stability. The two
// LFO phases sit 180 deg apart for a wide sweep. Manual sets the base delay
// (sweep center) so the knob doubles as a static comb/jet-position control.
//
// Params (param[4] is UNUSED; Mix is the chain Dry/Wet):
//   * Rate     (p0): 0.05..3 Hz (log).
//   * Depth    (p1): 0..4.5 ms LFO sweep.
//   * Manual   (p2): 0.15..6 ms base delay (the comb position).
//   * Feedback (p3): 0..0.92 (damper keeps the top of the range stable).

#ifndef HELLCAT_DSP_FX_FV1_FV1FLANGER_H
#define HELLCAT_DSP_FX_FV1_FV1FLANGER_H

#include <cstdint>

#include "dsp/fx/fv1/Fv1FxProcessor.h"

namespace hellcat::fv1
{

class Fv1Flanger : public Fv1FxProcessor
{
public:
    void setParams (const std::array<float, kNumFxSlotParams>& param) override;
    void resetInternal() override;
    FxType type() const noexcept override { return FxType::Flanger; }

protected:
    void processSampleFx (int32_t lin, int32_t rin, int32_t& lout, int32_t& rout) override;

private:
    DelayLine<1024> line_;          // 31 ms cap; 6 ms + 4.5 ms swing max
    OnePoleLpFx damp_;              // 8 kHz fixed loop damper
    LoopDcKiller fbDc_ {};          // (2026-08-21 consistency: fb 0.92 = 12.5x DC loop gain)
    float inc_ = 0.0f;
    float phase_ = 0.0f;            // L voice phase; R = +0.5 (180 deg)
    float depthSamp_ = 0.0f;
    float baseSamp_  = 20.0f;   // TARGET base delay (samples); the read glides
    int32_t baseQL_  = 0;       // GLIDING base in Q16 (0 = unset sentinel; see .cpp)
    int16_t fb14_ = 0;
};

} // namespace hellcat::fv1

#endif // HELLCAT_DSP_FX_FV1_FV1FLANGER_H

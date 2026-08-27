// Copyright (c) 2026 805Labs Kft. / Hellcat.
//
// Fv1Gate — classic noise/expander gate with a hard-BYPASS corner: at
// Threshold=0 the gate is fully OPEN (transparent pass-through — the knob
// disables it, exactly the requested behaviour). Peak detector + hold time +
// smoothed open/close gains (clickless). Fixed-point gain application per the
// family contract.
//
// Params (param[4] is UNUSED; Mix is the chain Dry/Wet):
//   * Threshold (p0): 0 (= gate DISABLED, always open) .. 1 (threshold
//                     0.0 -> 0.7 of full scale, exp 0..0.7).
//   * Attack  (p1): 0.05..10 ms (log).
//   * Hold    (p2): 0..150 ms.
//   * Release (p3): 5..500 ms (log).

#ifndef HELLCAT_DSP_FX_FV1_FV1GATE_H
#define HELLCAT_DSP_FX_FV1_FV1GATE_H

#include <cstdint>

#include "dsp/fx/fv1/Fv1FxProcessor.h"

namespace hellcat::fv1
{

class Fv1Gate : public Fv1FxProcessor
{
public:
    void setParams (const std::array<float, kNumFxSlotParams>& param) override;
    void resetInternal() override;
    FxType type() const noexcept override { return FxType::Gate; }

protected:
    void processSampleFx (int32_t lin, int32_t rin, int32_t& lout, int32_t& rout) override;

private:
    float th_       = 0.0f;    // 0 = disabled (always open)
    float env_      = 0.0f;    // peak envelope
    float openA_    = 0.5f;    // attack one-pole toward OPEN gain
    float closeA_   = 0.01f;   // release one-pole toward CLOSED gain
    float holdLeft_ = 0.0f;    // hold countdown (internal samples)
    float holdSamp_ = 0.0f;
    float gain_     = 1.0f;    // smoothed 0..1 gate gain
    bool  open_     = true;
};

} // namespace hellcat::fv1

#endif // HELLCAT_DSP_FX_FV1_FV1GATE_H

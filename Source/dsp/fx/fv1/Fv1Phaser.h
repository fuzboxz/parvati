// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Fv1Phaser — 6-stage digital phaser (FV-1 hardware-emulation family).
//
// Six cascaded 1st-order allpass stages (memoryless; each holds its own
// x[n-1]/y[n-1]) swept by a triangle LUT LFO, summed with the dry input to form
// the classic notch sweep, with a hard-clip-saturated feedback path. The shared
// allpass coefficient is computed once per sample from center + depth*lfo and
// applied to all six stages (respecting the FV-1 compute budget).
//
// Params (verbatim from docs/FX_FV1_DESIGN.md so the FxSlotLabels readouts match):
//   p0 Rate     0.1..8.0 Hz = 0.1*pow(80,p)   (triangle LUT LFO)
//   p1 Depth    0..1, depthHz = p*1500         (LFO amplitude on the sweep)
//   p2 Feedback -0.9..0.9 = -0.9 + p*1.8
//   p3 Center   200..2000 Hz = 200*pow(10,p)
//   p4 UNUSED   (Mix is the chain Dry/Wet; never read here)
//
// JUCE-FREE: only the FV-1/framework headers + standard <...> headers.

#ifndef PARVATI_DSP_FX_FV1_FV1PHASER_H
#define PARVATI_DSP_FX_FV1_FV1PHASER_H

#include <array>
#include <cstdint>

#include "dsp/fx/fv1/Fv1FxProcessor.h"

namespace parvati::fv1
{

class Fv1Phaser : public Fv1FxProcessor
{
public:
    void processSampleFx (int32_t lin, int32_t rin, int32_t& lout, int32_t& rout) override;
    void prepareInternal (double sampleRate, int maxBlock) override;
    void resetInternal() override;
    void setParams (const float param[5]) override;
    FxType type() const noexcept override { return FxType::Phaser; }

private:
    static constexpr int kNumStages = 6;

    std::array<Allpass1Fx, kNumStages> stages_ {};

    float phase_   = 0.0f;   // triangle LFO phase in [0,1)
    int32_t prevOut_ = 0;    // Q.23 previous wet output (feedback source)

    // LOOP DC KILLER (2026-08-21, caught by fx-invariants [I2]): at max
    // feedback (loop gain 0.9) a hard-driven asymmetric input rectifies on
    // the loop rail into |mean|/rms ~0.13 DC — the same poisoning class as
    // the delays (see Fv1Echo.h). In the feedback return path.
    LoopDcKiller fbDc_ {};

    // Cached control parameters (each param clamped to [0,1] then mapped).
    float rateHz_   = 0.5f;  // 0.1..8 Hz
    float depthHz_  = 0.0f;  // 0..1500 Hz sweep amplitude
    float fb_       = 0.0f;  // -0.9..0.9
    float centerHz_ = 200.0f;// 200..2000 Hz
    // Block-constant derived values (precomputed in setParams, not per sample).
    float   inc_    = 0.0f;  // LFO phase increment per internal sample = rateHz_/32768
    int16_t fb14_   = 0;     // q14(fb_) — 14-bit feedback coefficient
};

} // namespace parvati::fv1

#endif // PARVATI_DSP_FX_FV1_FV1PHASER_H

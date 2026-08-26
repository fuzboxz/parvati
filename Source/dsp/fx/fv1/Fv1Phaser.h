// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.
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

#ifndef HELLCAT_DSP_FX_FV1_FV1PHASER_H
#define HELLCAT_DSP_FX_FV1_FV1PHASER_H

#include <array>
#include <cstdint>

#include "dsp/fx/fv1/Fv1FxProcessor.h"

namespace hellcat::fv1
{

class Fv1Phaser : public Fv1FxProcessor
{
public:
    void processSampleFx (int32_t lin, int32_t rin, int32_t& lout, int32_t& rout) override;
    void prepareInternal (double sampleRate, int maxBlock) override;
    void resetInternal() override;
    void setParams (const std::array<float, kNumFxSlotParams>& param) override;
    FxType type() const noexcept override { return FxType::Phaser; }

private:
    static constexpr int kNumStages = 6;

    std::array<Allpass1Fx, kNumStages> stages_ {};

    float phase_   = 0.0f;   // triangle LFO phase in [0,1)
    int32_t prevOut_ = 0;    // Q.23 previous wet output (feedback source)

    // LOOP DC KILLER (2026-08-21, caught by fx-invariants [I2]): at max
    // feedback (loop gain 0.9) a hard-driven asymmetric input rectifies on
    // the loop rail into |mean|/rms ~0.13 DC — the same contamination class as
    // the delays (see Fv1Echo.h). In the feedback return path.
    LoopDcKiller fbDc_ {};

    // FEEDBACK HF DAMP (2026-08-21 — the "100% params crackle" fix): six
    // cascaded 1st-order allpasses give ~pi phase each at high frequency
    // (6*pi == 0 mod 2*pi) = POSITIVE feedback near Nyquist, so at fb 0.9
    // the RateBridge's linear-resampler artifacts (~0.025 floor) amplify
    // ~4x into audible ticks (measured worst 0.096 on a pure sine, no
    // modulation) that pulse with the LFO sweep. A one-pole LP at 8 kHz in
    // the regen return (the Fv1Flanger idiom — analog regen stages are
    // band-limited too) removes the near-Nyquist amplification while leaving
    // the musical notch band (<= 3.5 kHz at max Center+Depth) untouched.
    // FOUR cascaded poles at 5 kHz (measured ladder: 1 pole -> 0.063 worst,
    // 2 poles -> 0.034-0.046: the fb-0.9 resonance is up to +20 dB and the
    // resampler artifacts are broadband, so 10-13 kHz energy still gained;
    // 4 poles = -24 dB/oct keeps every resonance-above-damp at net-negative
    // gain — measured at the no-FX engine baseline 0.025-0.027).
    OnePoleLpFx fbDamp1_ {}, fbDamp2_ {}, fbDamp3_ {}, fbDamp4_ {};

    // Cached control parameters (each param clamped to [0,1] then mapped).
    float rateHz_   = 0.5f;  // 0.1..8 Hz
    float depthHz_  = 0.0f;  // 0..1500 Hz sweep amplitude
    float fb_       = 0.0f;  // -0.9..0.9
    float centerHz_ = 200.0f;// 200..2000 Hz
    // Block-constant derived values (precomputed in setParams, not per sample).
    float   inc_    = 0.0f;  // LFO phase increment per internal sample = rateHz_/32768
    int16_t fb14_   = 0;     // q14(fb_) — 14-bit feedback coefficient
};

} // namespace hellcat::fv1

#endif // HELLCAT_DSP_FX_FV1_FV1PHASER_H

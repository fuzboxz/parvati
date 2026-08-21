// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Fv1Ensemble — BBD-style ensemble chorus. Two parallel delay lines are
// modulated 90 deg out of phase via the 32-value sine LUT (NO per-sample trig)
// and panned hard L/R for a wide stereo shimmer. Part of the FV-1 hardware-
// emulation FX family; subclasses Fv1FxProcessor and is JUCE-free.
//
// Parameter slots (see docs/FX_FV1_DESIGN.md — verbatim mappings):
//   p0 Rate     0..1 -> 0.1..8 Hz (rate = 0.1*pow(80,p))
//   p1 Depth    0..1 -> 0..15 ms  (samples = p*15e-3*32768)
//   p2 Center   0..1 -> 2..25 ms  (samples = (2+p*23)e-3*32768)
//   p3 Feedback 0..1 -> -0.9..0.9 (-0.9+p*1.8)
//   p4 UNUSED (Mix is the chain Dry/Wet — never read here).

#ifndef PARVATI_DSP_FX_FV1_FV1ENSEMBLE_H
#define PARVATI_DSP_FX_FV1_FV1ENSEMBLE_H

#include "dsp/fx/fv1/Fv1FxProcessor.h"

namespace parvati::fv1
{

// BBD-style stereo ensemble chorus. The whole audio path is 24-bit fixed-point
// (Q.23) with 14-bit coefficient / control quantization, exactly the FV-1
// constraint set. The chain feeds mono duplicated to L==R; we treat `lin` as the
// mono input and emit the wet signal on BOTH channels (mono-in/stereo-out). Dry/
// wet blending is the chain's job — this class only produces the wet signal.
class Fv1Ensemble : public Fv1FxProcessor
{
public:
    void prepareInternal (double sampleRate, int maxBlock) override;
    void resetInternal() override;
    void setParams (const float param[5]) override;
    FxType type() const override;

protected:
    void processSampleFx (int32_t lin, int32_t rin,
                          int32_t& lout, int32_t& rout) override;

private:
    // Two parallel BBD rings (hard-panned L/R). Capacity 2048 each so the max
    // read delay (Center 25 ms = 819 + Depth 15 ms = 491 = 1311 samples) never
    // wraps the power-of-two ring. 2*2048 = 4096 samples total.
    DelayLine<2048> lineA_;
    DelayLine<2048> lineB_;

    // LFO phase accumulators in [0,1). phaseB_ trails phaseA_ by 0.25 (90 deg).
    float phaseA_ = 0.0f;
    float phaseB_ = 0.25f;

    // Cached control parameters (mapped from the generic 0..1 slot params).
    float inc_        = 0.0f;   // LFO phase increment per internal sample.
    float depthSamp_  = 0.0f;   // LFO depth in samples.
    float centerSamp_ = 0.0f;   // Center delay in samples.
    int16_t fb14_     = 0;      // q14(feedback) in [-0.9, 0.9].

    // REGEN RETURN TREATMENT (2026-08-21, subagent audit — the phaser pre-fix
    // signature, twice over): fb +-0.9 with a raw addSat loop had (a) NO HF
    // damping — the depth clamp's dl=1.0 dwell + the sweep past dl~2 give
    // near-Nyquist resonance (bound 10x, +20 dB over the 0.025 bridge floor
    // = audible LFO-rate ticks), and (b) NO DC killer — 10x DC loop gain, the
    // delay->shaper poisoning class. 4 poles @ 5 kHz + soft knee + killer
    // (the exact Fv1Phaser recipe).
    // Per-LINE chains (A and B are independent loops: shared filter state
    // would interleave the two signals' history).
    OnePoleLpFx fbDampA_[4] {}, fbDampB_[4] {};
    LoopDcKiller fbDcA_ {}, fbDcB_ {};
};

} // namespace parvati::fv1

#endif // PARVATI_DSP_FX_FV1_FV1ENSEMBLE_H

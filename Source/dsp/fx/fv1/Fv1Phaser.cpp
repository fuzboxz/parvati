// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Fv1Phaser — 6-stage digital phaser implementation.
// See Fv1Phaser.h for the parameter mapping + algorithm description.
//
// The audio path inside processSampleFx is 24-bit fixed-point (Q.23) with
// saturation; the LFO read + the one-per-sample allpass coefficient solve are
// the only float control math (allowed once per sample by the family spec).
//
// MEMORY: this effect uses NO delay lines — the six 1st-order allpass stages are
// memoryless (state = x[n-1]/y[n-1] only). State-only; total delay memory = 0.
// Trivial static_assert kept for parity with the family memory budget contract.
// Max single delay value = 0 (no DelayLine instances).

#include "dsp/fx/fv1/Fv1Phaser.h"

#include <cmath>

namespace parvati::fv1
{

// No DelayLine instances in this effect; the summed delay memory is 0 samples.
static_assert (0 <= kMaxMemorySamples, "Fv1Phaser: total delay memory within budget");

namespace
{
inline float clamp01 (float x) noexcept
{
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}
} // namespace

void Fv1Phaser::setParams (const float param[5])
{
    const float p0 = clamp01 (param[0]);
    const float p1 = clamp01 (param[1]);
    const float p2 = clamp01 (param[2]);
    const float p3 = clamp01 (param[3]);
    // param[4] is UNUSED (Mix is the chain Dry/Wet).

    rateHz_   = 0.1f * std::pow (80.0f, p0);   // 0.1..8 Hz
    depthHz_  = p1 * 1500.0f;                  // 0..1500 Hz
    fb_       = -0.9f + p2 * 1.8f;             // -0.9..0.9
    centerHz_ = 200.0f * std::pow (10.0f, p3); // 200..2000 Hz
}

void Fv1Phaser::prepareInternal (double /*sampleRate*/, int /*maxBlock*/)
{
    for (auto& s : stages_)
        s.clear();
    phase_   = 0.0f;
    prevOut_ = 0;
}

void Fv1Phaser::resetInternal()
{
    for (auto& s : stages_)
        s.clear();
    phase_   = 0.0f;
    prevOut_ = 0;
}

void
Fv1Phaser::processSampleFx (int32_t lin, int32_t /*rin*/, int32_t& lout, int32_t& rout) noexcept
{
    // --- Triangle LFO (table-driven; no per-sample trig) ---
    const float lfo = lutTri32 (phase_);                    // [-1, 1]
    // Advance the phase accumulator at the internal rate (rate cycles/sec).
    const float inc = rateHz_ / static_cast<float> (kInternalRate);   // cycles per sample
    // Wrap the new phase into [0,1) (deterministic; no per-sample trig).
    phase_ = phase_ + inc - std::floor (phase_ + inc);

    // --- Shared allpass coefficient, computed ONCE per sample (float control) ---
    float fc = centerHz_ + depthHz_ * lfo;
    if (fc < 50.0f)  fc = 50.0f;
    if (fc > 7000.0f) fc = 7000.0f;
    const float x = 3.14159265f * fc / static_cast<float> (kInternalRate);
    const float c = (1.0f - x) / (1.0f + x);   // 1st-order AP coef (small-angle tan approx)
    for (auto& s : stages_)                    // setCoef clamps + q14-quantizes internally
        s.setCoef (c);

    // --- Feedback with hard-clip saturation ---
    const int32_t fbVal = f24_sat (prevOut_);
    const int32_t fbIn  = f24_addSat (lin, f24_mulk (fbVal, q14 (fb_)));

    // --- Cascade through the six stages ---
    int32_t s = fbIn;
    for (auto& st : stages_)
        s = st.process (s);
    prevOut_ = s;

    // --- Output: dry + allpass notch sum, saturating (mono-in / stereo-out) ---
    const int32_t out = f24_sat (f24_addSat (lin, s));
    lout = out;
    rout = out;
}

} // namespace parvati::fv1

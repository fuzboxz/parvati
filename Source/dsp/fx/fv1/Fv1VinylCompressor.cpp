// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Fv1VinylCompressor implementation. See the header for the signal path and
// parameter mapping. The entire audio-path multiply/add is 24-bit fixed-point
// (Q.23) with saturation; only the compressor *detector* (envelope + gain) and
// the per-block coefficient setup run in float, as the per-effect spec allows.

#include "dsp/fx/fv1/Fv1VinylCompressor.h"

#include <algorithm>
#include <cmath>

namespace parvati::fv1
{

// Total delay memory: one DelayLine<2048> (no extra buffer). Well within the
// FV-1 32,768-sample RAM budget, and the largest single delay VALUE in use
// (1638 + ~6 of LFO swing = 1644) is far under kMaxDelaySamples (32767).
static_assert (DelayLine<2048>::capacity <= kMaxMemorySamples,
               "Fv1VinylCompressor total delay memory within the FV-1 budget");

void Fv1VinylCompressor::setParams (const float param[5])
{
    pCompress_ = std::clamp (param[0], 0.0f, 1.0f);
    pPitch_    = std::clamp (param[1], 0.0f, 1.0f);
    pCrackle_  = std::clamp (param[2], 0.0f, 1.0f);
    pAge_      = std::clamp (param[3], 0.0f, 1.0f);
    // param[4] is the chain Dry/Wet — NEVER read here.

    // Compressor: threshold lowers with Compress, makeup rises (max 4.0, matching
    // docs/FX_FV1_DESIGN.md — enough to restore level under heavy compression).
    th_     = 1.0f - 0.9f * pCompress_;
    makeup_ = 1.0f + 3.0f * pCompress_;

    // Age low-pass cutoff (1000..15000 Hz). setCutoff quantizes to 14-bit.
    ageLp_.setCutoff (1000.0f * std::pow (15.0f, pAge_));
}

void Fv1VinylCompressor::prepareInternal (double /*sampleRate*/, int /*maxBlock*/)
{
    // Attack/release are fixed at the FV-1 internal rate (32.768 kHz).
    attackA_  = 1.0f - std::exp (-1.0f / (0.002f * 32768.0f));
    releaseA_ = 1.0f - std::exp (-1.0f / (0.150f * 32768.0f));
    resetInternal();
}

void Fv1VinylCompressor::resetInternal()
{
    delay_.clear();
    ageLp_.clear();
    env_     = 0.0f;
    ph1_     = 0.0f;
    ph2_     = 0.0f;
    lcgState_ = 0u;
    // Re-apply the cutoff from the cached Age param (clear() preserves coeffs,
    // but this keeps the filter sane even before the first setParams()).
    ageLp_.setCutoff (1000.0f * std::pow (15.0f, pAge_));
}

FxType Fv1VinylCompressor::type() const noexcept
{
    return FxType::VinylCompressor;
}

void Fv1VinylCompressor::processSampleFx (int32_t lin, int32_t /*rin*/,
                                          int32_t& lout, int32_t& rout)
{
    // ---- Compressor sidechain (float detector) ----
    const float xf = std::fabs (f24_toFloat (lin));
    const float a  = (xf > env_) ? attackA_ : releaseA_;
    env_ += a * (xf - env_);

    float gain = 1.0f;
    if (env_ > th_)
        gain = std::pow (th_ / env_, 0.75f);   // ~4:1 ratio
    float g = gain * makeup_;
    if (g < 0.0f) g = 0.0f;
    if (g > 4.0f) g = 4.0f;

    // Apply compressor+makeup gain g (in [0,4]) as saturating fixed-point.
    // Decompose g = ki + kf (ki in {0,1,2,3}, kf in [0,1)) so a single 14-bit
    // fractional multiply plus up to three saturating adds cover the full range;
    // the saturating clip on heavy makeup is part of the lo-fi character.
    int ki = static_cast<int> (g);
    if (ki > 3) ki = 3;
    const float kf = g - static_cast<float> (ki);
    int32_t comp = f24_mulk (lin, q14 (kf));   // kf*lin
    for (int i = 0; i < ki; ++i)
        comp = f24_addSat (comp, lin);        // + ki*lin  (saturating)
    comp = f24_sat (comp);

    // ---- Pitch-warp modulated delay (50 ms = 1638 samples; +/- depth) ----
    delay_.write (comp);
    const float depth = pPitch_ * 3.0f;
    const float readDelay = 1638.0f + (lutSine32 (ph1_) + lutSine32 (ph2_)) * depth;
    const int32_t warped = delay_.readFrac (readDelay);
    ph1_ += 0.5f / 32768.0f;
    ph2_ += 4.0f / 32768.0f;
    if (ph1_ >= 1.0f) ph1_ -= 1.0f;   // keep float precision tidy over long runs
    if (ph2_ >= 1.0f) ph2_ -= 1.0f;

    // ---- Age low-pass ----
    const int32_t aged = ageLp_.process (warped);

    // ---- Crackle (LCG impulse when value > 0.98) ----
    lcgState_ = lcgState_ * 1103515245u + 12345u;   // unsigned: wraps mod 2^32
    const float v = static_cast<float> ((lcgState_ >> 16) & 0xFFFFu) / 65535.0f;
    int32_t click = 0;
    if (v > 0.98f)
        click = f24_mulk (f24_fromFloat ((v - 0.98f) / 0.02f), q14 (pCrackle_));

    // ---- Output (mono-in / stereo-out; dry/wet is the chain's job) ----
    const int32_t out = f24_addSat (aged, click);
    lout = out;
    rout = out;
}

} // namespace parvati::fv1

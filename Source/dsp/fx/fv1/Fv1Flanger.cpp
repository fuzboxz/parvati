// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Fv1Flanger implementation — short modulated delay, 180-deg stereo phases,
// damped feedback up to 0.92.

#include "dsp/fx/fv1/Fv1Flanger.h"

#include <algorithm>
#include <cmath>

namespace parvati::fv1
{

static_assert (DelayLine<1024>::capacity <= kMaxMemorySamples,
               "Fv1Flanger within the FV-1 RAM budget");

void Fv1Flanger::setParams (const float param[5])
{
    const float p0 = std::clamp (param[0], 0.0f, 1.0f);
    const float p1 = std::clamp (param[1], 0.0f, 1.0f);
    const float p2 = std::clamp (param[2], 0.0f, 1.0f);
    const float p3 = std::clamp (param[3], 0.0f, 1.0f);
    // param[4] is UNUSED (Mix is the chain Dry/Wet — never read here).

    const float rate = 0.05f * std::pow (60.0f, p0);       // 0.05..3 Hz
    inc_ = rate / static_cast<float> (kInternalRate);
    depthSamp_ = p1 * 4.5e-3f * static_cast<float> (kInternalRate);
    baseSamp_  = (0.15f + p2 * 5.85f) * 1.0e-3f * static_cast<float> (kInternalRate);
    // Depth clamp: never let the sweep pin at the 1-sample read floor.
    // Base min (0.15 ms = 4.9) << Depth max (4.5 ms = 147.5), so the corner
    // Manual=0/Depth=1 used to clamp inside readFrac for ~49% of EVERY LFO
    // cycle — half the sweep dead at ~zero delay, the jet collapsing toward
    // a near-through path. Capping depth at base-1 keeps the deepest read at
    // exactly 1 sample; the documented ranges are untouched (only the
    // out-of-range combination is affected).
    if (depthSamp_ > baseSamp_ - 1.0f)
        depthSamp_ = baseSamp_ - 1.0f;
    fb14_ = q14 (p3 * 0.92f);
    damp_.setCutoff (8000.0f);
}

void Fv1Flanger::resetInternal()
{
    line_.clear();
    damp_.clear();
    phase_ = 0.0f;
}

void Fv1Flanger::processSampleFx (int32_t lin, int32_t /*rin*/,
                                  int32_t& lout, int32_t& rout)
{
    // Two sweep phases 180 deg apart over the same line (offset reads).
    const float dl = baseSamp_ + depthSamp_ * lutSine32 (phase_);
    const float dr = baseSamp_ + depthSamp_ * lutSine32 (phase_ + 0.5f);
    const int32_t rL = line_.readFrac (dl);
    const int32_t rR = line_.readFrac (dr);

    // One feedback loop on the L-phase tap, damped (jet regeneration).
    const int32_t fb = damp_.process (rL);
    line_.write (f24_addSat (lin, f24_mulk (fb, fb14_)));

    phase_ += inc_;
    phase_ -= std::floor (phase_);

    lout = rL;
    rout = rR;
}

} // namespace parvati::fv1

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

// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Fv1Chorus implementation — two detuned SIN-LFO delay voices, panned L/R.

#include "dsp/fx/fv1/Fv1Chorus.h"

#include <algorithm>
#include <cmath>

namespace parvati::fv1
{

static_assert (2 * DelayLine<2048>::capacity <= kMaxMemorySamples,
               "Fv1Chorus within the FV-1 RAM budget");

void Fv1Chorus::setParams (const std::array<float, kNumFxSlotParams>& param)
{
    const float p0 = std::clamp (param[0], 0.0f, 1.0f);
    const float p1 = std::clamp (param[1], 0.0f, 1.0f);
    const float p2 = std::clamp (param[2], 0.0f, 1.0f);
    const float p3 = std::clamp (param[3], 0.0f, 1.0f);
    // param[4] is UNUSED (Mix is the chain Dry/Wet — never read here).

    const float rate = 0.1f * std::pow (80.0f, p0);
    inc_ = rate / static_cast<float> (kInternalRate);
    depthSamp_  = p1 * 6.0e-3f * static_cast<float> (kInternalRate);
    centerSamp_ = fxlaw::chorusLoopSeconds (p2) * static_cast<float> (kInternalRate);
    // Depth clamp: never let the sweep pin at the 1-sample read floor.
    // Center min (5 ms = 163.8) < Depth max (6 ms = 196.6), so the corner
    // Center=0/Depth=1 used to clamp inside readFrac for ~19% of every LFO
    // cycle (a flat-topped sweep). Capping depth at center-1 keeps the
    // deepest read at exactly 1 sample — the sweep always moves.
    if (depthSamp_ > centerSamp_ - 1.0f)
        depthSamp_ = centerSamp_ - 1.0f;
    fb14_ = q14 (fxlaw::chorusFeedbackGain (p3));
}

void Fv1Chorus::resetInternal()
{
    lineL_.clear();
    lineR_.clear();
    phase_ = 0.0f;
}

void Fv1Chorus::processSampleFx (int32_t lin, int32_t /*rin*/,
                                 int32_t& lout, int32_t& rout)
{
    // R voice trails 0.30 of a cycle (108 deg) — wide, always moving.
    const float rphase = phase_ + 0.30f;
    const int32_t rL = lineL_.readFrac (centerSamp_ + depthSamp_ * lutSine32 (phase_));
    const int32_t rR = lineR_.readFrac (centerSamp_ + depthSamp_ * lutSine32 (rphase));

    lineL_.write (f24_addSat (lin, f24_mulk (rL, fb14_)));
    lineR_.write (f24_addSat (lin, f24_mulk (rR, fb14_)));

    phase_ += inc_;
    phase_ -= std::floor (phase_);

    lout = rL;
    rout = rR;
}

} // namespace parvati::fv1

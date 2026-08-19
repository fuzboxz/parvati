// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Fv1Compressor implementation — clean feed-forward peak leveling.

#include "dsp/fx/fv1/Fv1Compressor.h"

#include <algorithm>
#include <cmath>

namespace parvati::fv1
{

static_assert (0 <= kMaxMemorySamples, "Fv1Compressor within the FV-1 RAM budget");

void Fv1Compressor::setParams (const float param[5])
{
    const float p0 = std::clamp (param[0], 0.0f, 1.0f);
    const float p1 = std::clamp (param[1], 0.0f, 1.0f);
    const float p2 = std::clamp (param[2], 0.0f, 1.0f);
    const float p3 = std::clamp (param[3], 0.0f, 1.0f);
    // param[4] is UNUSED (Mix is the chain Dry/Wet — never read here).

    th_       = 1.0f - 0.95f * p0;                 // 1.0 -> 0.05
    ratioExp_ = 0.5f + 0.4f * p0;                  // 2:1 -> 10:1  (1 - 1/ratio)
    makeup_   = 1.0f + 2.0f * p0;                  // 1 -> 3
    // Level 0..2 via the same ki/kf split used for g below: q14() clamps
    // c >= 1.0 to 8191, so q14(p3*2) pinned every p3 > ~0.5 to ~unity (the
    // upper half of the knob was dead — audit rev_dyn.md).
    {
        float lvl = p3 * 2.0f;
        if (lvl < 0.0f) lvl = 0.0f;
        if (lvl > 2.0f) lvl = 2.0f;
        levelShift_ = (lvl >= 1.0f) ? 1 : 0;
        level14_    = q14 (lvl - static_cast<float> (levelShift_));
    }

    const float atkMs = 0.5f * std::pow (100.0f, p1);      // 0.5..50 ms
    const float relMs = 20.0f * std::pow (25.0f, p2);      // 20..500 ms
    attackA_  = 1.0f - std::exp (-1.0f / (atkMs * 1.0e-3f * 32768.0f));
    releaseA_ = 1.0f - std::exp (-1.0f / (relMs * 1.0e-3f * 32768.0f));
}

void Fv1Compressor::prepareInternal (double, int) {}

void Fv1Compressor::resetInternal()
{
    env_ = 0.0f;
}

void Fv1Compressor::processSampleFx (int32_t lin, int32_t /*rin*/,
                                     int32_t& lout, int32_t& rout)
{
    const float xf = std::fabs (f24_toFloat (lin));
    const float a  = (xf > env_) ? attackA_ : releaseA_;
    env_ += a * (xf - env_);

    float gain = 1.0f;
    if (env_ > th_)
        gain = std::pow (th_ / env_, ratioExp_);   // 2:1 .. 10:1
    float g = gain * makeup_;
    if (g < 0.0f) g = 0.0f;
    if (g > 3.5f) g = 3.5f;

    int ki = static_cast<int> (g);
    if (ki > 3) ki = 3;
    const float kf = g - static_cast<float> (ki);
    int32_t comp = f24_mulk (lin, q14 (kf));
    for (int i = 0; i < ki; ++i)
        comp = f24_addSat (comp, lin);
    comp = f24_sat (comp);

    // Output trim 0..2: fractional part + the integer x2 stage.
    int32_t out = f24_mulk (comp, level14_);
    if (levelShift_ != 0)
        out = f24_addSat (out, comp);
    lout = out;
    rout = out;
}

} // namespace parvati::fv1

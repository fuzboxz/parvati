// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Fv1Echo implementation — ping-pong stereo echo at the exact 32K RAM budget.

#include "dsp/fx/fv1/Fv1Echo.h"

#include <algorithm>
#include <cmath>

namespace parvati::fv1
{

static_assert (2 * DelayLine<16384>::capacity == kMaxMemorySamples,
               "Fv1Echo consumes exactly the FV-1 delay RAM budget (2 x 16K)");

void Fv1Echo::setParams (const float param[5])
{
    const float p0 = std::clamp (param[0], 0.0f, 1.0f);
    const float p1 = std::clamp (param[1], 0.0f, 1.0f);
    const float p2 = std::clamp (param[2], 0.0f, 1.0f);
    const float p3 = std::clamp (param[3], 0.0f, 1.0f);
    // param[4] is UNUSED (Mix is the chain Dry/Wet — never read here).

    const float ms = 10.0f * std::pow (47.0f, p0);            // 10..470 ms
    timeL_ = ms * 1.0e-3f * static_cast<float> (kInternalRate);
    timeR_ = timeL_ * (1.0f + p3);                             // 1..2x spread
    if (timeR_ > 16383.0f) timeR_ = 16383.0f;                  // ring guard
    // 0..100% knob -> 0..0.995 loop gain: "100% reads as infinite". The Q.14
    // coefficient quantizes 0.995 to 8150/8192 (-0.044 dB per repeat).
    fb14_ = q14 (p1 * 0.995f);
    damp_.setCutoff (700.0f * std::pow (12000.0f / 700.0f, p2));
}

void Fv1Echo::resetInternal()
{
    lineL_.clear();
    lineR_.clear();
    damp_.clear();
}

void Fv1Echo::processSampleFx (int32_t lin, int32_t /*rin*/,
                               int32_t& lout, int32_t& rout)
{
    const int32_t tapL = lineL_.readFrac (timeL_);
    const int32_t tapR = lineR_.readFrac (timeR_);

    // Ping-pong: the L tap walks into the R line; the damped R tap walks back
    // into the L line together with the input.
    lineR_.write (tapL);
    const int32_t fb = damp_.process (tapR);
    lineL_.write (f24_addSat (lin, f24_mulk (fb, fb14_)));

    lout = tapL;
    rout = tapR;
}

} // namespace parvati::fv1

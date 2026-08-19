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

namespace
{
// Q.16 glide constants — verbatim the Fv1ClockedDelay tap glide (see the
// TIME GLIDE note in this file's header): one-pole k = 1/256 per internal
// sample, capped at ~0.25 sample/sample (tape-speed pitch bend, click-free),
// with the sub-1/16-sample tail snapped and a +/-1 Q16 minimum step so the
// glide can never stall below one quantum.
constexpr int32_t kQOne    = 65536;    // 1.0 sample in Q.16
constexpr int32_t kGlideCapQ = 16384;  // ~0.25 sample per internal sample
constexpr int kGlideShift  = 8;        // one-pole k = 1/256 per sample

inline int32_t echoTargetQ (float timeSamples) noexcept
{
    return static_cast<int32_t> (std::lround (
        static_cast<double> (timeSamples) * 65536.0));
}

inline void glideTapQ16 (int32_t& cur, int32_t target) noexcept
{
    const int32_t deltaQ = target - cur;
    const int32_t distQ  = (deltaQ < 0) ? -deltaQ : deltaQ;
    if (distQ <= kQOne / 16)
    {
        cur = target;   // settle the inaudible tail instantly
        return;
    }
    int32_t stepQ = deltaQ >> kGlideShift;
    if (stepQ >  kGlideCapQ) stepQ =  kGlideCapQ;
    if (stepQ < -kGlideCapQ) stepQ = -kGlideCapQ;
    if (stepQ == 0) stepQ = (deltaQ > 0) ? 1 : -1;   // never stall below 1 Q16
    cur += stepQ;
}
} // namespace

void Fv1Echo::setParams (const float param[5])
{
    const float p0 = std::clamp (param[0], 0.0f, 1.0f);
    const float p1 = std::clamp (param[1], 0.0f, 1.0f);
    const float p2 = std::clamp (param[2], 0.0f, 1.0f);
    const float p3 = std::clamp (param[3], 0.0f, 1.0f);
    // param[4] is UNUSED (Mix is the chain Dry/Wet — never read here).

    const float ms = 10.0f * std::pow (47.0f, p0);            // 10..470 ms
    timeTargetL_ = ms * 1.0e-3f * static_cast<float> (kInternalRate);
    timeTargetR_ = timeTargetL_ * (1.0f + p3);                 // 1..2x spread
    if (timeTargetR_ > 16383.0f) timeTargetR_ = 16383.0f;      // ring guard
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
    // "unset" glide sentinel: the next block snaps both taps to target
    // (a fresh instance must not glide in from zero for a tenth of a second).
    timeQL_ = 0;
    timeQR_ = 0;
}

void Fv1Echo::processSampleFx (int32_t lin, int32_t /*rin*/,
                               int32_t& lout, int32_t& rout)
{
    // GLIDE both taps toward their targets (Time AND Spread — spread moves
    // timeR_ too). The read is fractional, so a moving base is a smooth pitch
    // bend, never a read-pointer jump (the pre-glide behaviour clicked on
    // every stepped Time/Spread edit; see the header's TIME GLIDE note).
    if (timeQL_ <= 1) timeQL_ = echoTargetQ (timeTargetL_);   // snap on (re)start
    else               glideTapQ16 (timeQL_, echoTargetQ (timeTargetL_));
    if (timeQR_ <= 1) timeQR_ = echoTargetQ (timeTargetR_);
    else               glideTapQ16 (timeQR_, echoTargetQ (timeTargetR_));

    const float timeL = static_cast<float> (timeQL_) * (1.0f / 65536.0f);
    const float timeR = static_cast<float> (timeQR_) * (1.0f / 65536.0f);

    const int32_t tapL = lineL_.readFrac (timeL);
    const int32_t tapR = lineR_.readFrac (timeR);

    // Ping-pong: the L tap walks into the R line; the damped R tap walks back
    // into the L line together with the input.
    lineR_.write (tapL);
    const int32_t fb = damp_.process (tapR);
    lineL_.write (f24_addSat (lin, f24_mulk (fb, fb14_)));

    lout = tapL;
    rout = tapR;
}

} // namespace parvati::fv1

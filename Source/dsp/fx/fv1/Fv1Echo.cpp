// Copyright (c) 2026 805Labs Kft. / Hellcat.
//
// Fv1Echo implementation — ping-pong stereo echo at the exact 32K RAM budget.

#include "dsp/fx/fv1/Fv1Echo.h"

#include <algorithm>
#include <cmath>

namespace hellcat::fv1
{

static_assert (2 * DelayLine<16384>::capacity == kMaxMemorySamples,
               "Fv1Echo consumes exactly the FV-1 delay RAM budget (2 x 16K)");

namespace
{
inline int32_t echoTargetQ (float timeSamples) noexcept
{
    return static_cast<int32_t> (std::lround (
        static_cast<double> (timeSamples) * 65536.0));
}
} // namespace

void Fv1Echo::setParams (const std::array<float, kNumFxSlotParams>& param)
{
    const float p0 = std::clamp (param[0], 0.0f, 1.0f);
    const float p1 = std::clamp (param[1], 0.0f, 1.0f);
    const float p2 = std::clamp (param[2], 0.0f, 1.0f);
    const float p3 = std::clamp (param[3], 0.0f, 1.0f);
    // param[4] is UNUSED (Mix is the chain Dry/Wet — never read here).

    const float ms = fxlaw::echoBaseMs (p0);                     // 10..470 ms
    timeTargetL_ = ms * 1.0e-3f * static_cast<float> (kInternalRate);
    timeTargetR_ = timeTargetL_ * (1.0f + p3);                 // 1..2x spread
    if (timeTargetR_ > (float) fxlaw::echoHalfLineSamples)
        timeTargetR_ = (float) fxlaw::echoHalfLineSamples;     // ring guard
    // 0..100% knob -> 0..0.995 loop gain: "100% reads as infinite". The Q.14
    // coefficient quantizes 0.995 to 8150/8192 (-0.044 dB per repeat).
    fb14_ = q14 (fxlaw::echoFeedbackGain (p1));
    damp_.setCutoff (700.0f * std::pow (12000.0f / 700.0f, p2));
}

void Fv1Echo::resetInternal()
{
    lineR_.clear();
    lineL_.clear();
    damp_.clear();
    dcKiller_.clear();
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

    // Ping-pong: the L tap is written into the R line; the damped R tap is
    // written back into the L line together with the input. The R->L hop carries the LOOP
    // DC KILLER (see the header): ~10 Hz one-pole HP so near-unity regen can
    // never integrate DC into the runaway offset.
    lineR_.write (tapL);
    lineL_.write (f24_addSat (lin, f24_mulk (dcKiller_.process (damp_.process (tapR)), fb14_)));

    lout = tapL;
    rout = tapR;
}

} // namespace hellcat::fv1

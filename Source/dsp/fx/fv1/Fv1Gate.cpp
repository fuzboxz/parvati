// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.
//
// Fv1Gate implementation — peak-keyed expander with a knob-disabling corner.

#include "dsp/fx/fv1/Fv1Gate.h"

#include <algorithm>
#include <cmath>

namespace hellcat::fv1
{

static_assert (0 <= kMaxMemorySamples, "Fv1Gate within the FV-1 RAM budget");

void Fv1Gate::setParams (const std::array<float, kNumFxSlotParams>& param)
{
    const float p0 = std::clamp (param[0], 0.0f, 1.0f);
    const float p1 = std::clamp (param[1], 0.0f, 1.0f);
    const float p2 = std::clamp (param[2], 0.0f, 1.0f);
    const float p3 = std::clamp (param[3], 0.0f, 1.0f);
    // param[4] is UNUSED (Mix is the chain Dry/Wet — never read here).

    th_ = p0 * 0.7f;                                    // 0 = DISABLED (always open)
    const float atkMs = 0.05f * std::pow (200.0f, p1);  // 0.05..10 ms
    const float relMs = 5.0f * std::pow (100.0f, p3);   // 5..500 ms
    openA_  = 1.0f - std::exp (-1.0f / (atkMs * 1.0e-3f * 32768.0f));
    closeA_ = 1.0f - std::exp (-1.0f / (relMs * 1.0e-3f * 32768.0f));
    holdSamp_ = p2 * 0.150f * 32768.0f;                 // 0..150 ms
}

void Fv1Gate::resetInternal()
{
    env_ = 0.0f;
    gain_ = 1.0f;
    holdLeft_ = 0.0f;
    open_ = true;
}

void Fv1Gate::processSampleFx (int32_t lin, int32_t /*rin*/,
                               int32_t& lout, int32_t& rout)
{
    const float xf = std::fabs (f24_toFloat (lin));
    env_ += 0.02f * (xf - env_);   // ~2.7 ms detector (peak-ish, chatter-resistant)

    // State machine: above threshold (or disabled) -> OPEN, else CLOSED after
    // the hold expires. The gain one-pole moves between 1 and 0.
    if (th_ <= 0.0f || env_ > th_)
    {
        open_ = true;
        holdLeft_ = holdSamp_;               // re-arm the hold
    }
    else if (open_ && holdLeft_ > 0.0f)
    {
        holdLeft_ -= 1.0f;                   // still holding after a dip
    }
    else
    {
        open_ = false;
    }

    const float a = open_ ? openA_ : closeA_;
    gain_ += a * ((open_ ? 1.0f : 0.0f) - gain_);

    const int32_t out = f24_mulk (lin, q14 (std::clamp (gain_, 0.0f, 1.0f)));
    lout = out;
    rout = out;
}

} // namespace hellcat::fv1

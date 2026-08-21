// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Fv1Room implementation — Schroeder room, decorrelated stereo AP chains.

#include "dsp/fx/fv1/Fv1Room.h"

#include <algorithm>
#include <cmath>

namespace parvati::fv1
{

// 2*2048 + 2*4096 + 4*512 = 14336 words of the 32768 budget.
static_assert (2 * 2048 + 2 * 4096 + 4 * 512 <= kMaxMemorySamples,
               "Fv1Room within the FV-1 RAM budget");

Fv1Room::Fv1Room()
: quarter14_ (q14 (0.25f))
, apGain14_ (q14 (0.7f))
{
}

void Fv1Room::setParams (const float param[5])
{
    const float p0 = std::clamp (param[0], 0.0f, 1.0f);
    const float p1 = std::clamp (param[1], 0.0f, 1.0f);
    const float p2 = std::clamp (param[2], 0.0f, 1.0f);
    const float p3 = std::clamp (param[3], 0.0f, 1.0f);
    // param[4] is UNUSED (Mix is the chain Dry/Wet — never read here).

    // Decay 0.1..3 s -> PER-COMB feedback g_i = 10^(-3*D_i/(decay*fs)): the
    // per-pass RT60 law (g_i is applied once every D_i samples, so t60 ==
    // Decay by construction). The old per-SAMPLE law 10^(-3/(decay*fs)) hit the
    // 0.999 clamp for every decay > 0.21 s — knob inert above p0 ~= 0.038,
    // real LF t60 6-8 MINUTES (audit/fx_review_20260819/rev_reverbs.md).
    // The [0, 0.999] clamp is now a never-engaging stability guard.
    const float decay = 0.1f + p0 * 2.9f;
    for (int i = 0; i < 4; ++i)
    {
        float g = std::pow (10.0f,
                            -3.0f * static_cast<float> (kCombD[i]) / (decay * 32768.0f));
        g = std::clamp (g, 0.0f, 0.999f);
        g14_[i] = q14 (g);
    }

    const float fc = 500.0f * std::pow (24.0f, p1);
    for (auto& lp : lp_)
        lp.setCutoff (fc);

    width14_ = q14 (p2);
    invWidth14_ = q14 (1.0f - p2);
    const float tfc = 700.0f * std::pow (15000.0f / 700.0f, p3);
    toneLpL_.setCutoff (tfc);
    toneLpR_.setCutoff (tfc);
}

void Fv1Room::resetInternal()
{
    for (auto& k : dck_) k.clear();
    comb0_.clear(); comb1_.clear(); comb2_.clear(); comb3_.clear();
    apL0_.clear(); apL1_.clear(); apR0_.clear(); apR1_.clear();
    for (auto& lp : lp_) lp.clear();
    toneLpL_.clear();
    toneLpR_.clear();
}

// One Schroeder allpass: y = -g*in + r ; write(in + g*y).
static inline int32_t schroederAp (DelayLine<512>& ap, int d, int32_t in, int16_t g14)
{
    const int32_t r = ap.read (d);
    const int32_t y = f24_addSat (-f24_mulk (in, g14), r);
    ap.write (f24_addSat (in, f24_mulk (y, g14)));
    return y;
}

void Fv1Room::processSampleFx (int32_t lin, int32_t /*rin*/,
                               int32_t& lout, int32_t& rout)
{
    const int32_t o0 = comb0_.read (kCombD[0]);
    const int32_t o1 = comb1_.read (kCombD[1]);
    const int32_t o2 = comb2_.read (kCombD[2]);
    const int32_t o3 = comb3_.read (kCombD[3]);

    comb0_.write (f24_addSat (lin, f24_mulk (dck_[0].process (lp_[0].process (o0)), g14_[0])));
    comb1_.write (f24_addSat (lin, f24_mulk (dck_[1].process (lp_[1].process (o1)), g14_[1])));
    comb2_.write (f24_addSat (lin, f24_mulk (dck_[2].process (lp_[2].process (o2)), g14_[2])));
    comb3_.write (f24_addSat (lin, f24_mulk (dck_[3].process (lp_[3].process (o3)), g14_[3])));

    const int32_t sum = o0 + o1 + o2 + o3;              // plain int32: 4x Q.23 fits
    const int32_t in  = f24_mulk (sum, quarter14_);

    // Two decorrelated series-AP chains from the same comb bank.
    const int32_t outL = schroederAp (apL1_, kApL[1],
                          schroederAp (apL0_, kApL[0], in, apGain14_), apGain14_);
    const int32_t outRraw = schroederAp (apR1_, kApR[1],
                          schroederAp (apR0_, kApR[0], in, apGain14_), apGain14_);

    // Width: R crossfades between the L chain (mono) and its own chain.
    const int32_t rMix = f24_addSat (f24_mulk (outL, invWidth14_),
                                     f24_mulk (outRraw, width14_));

    lout = toneLpL_.process (outL);
    rout = toneLpR_.process (rMix);
}

} // namespace parvati::fv1

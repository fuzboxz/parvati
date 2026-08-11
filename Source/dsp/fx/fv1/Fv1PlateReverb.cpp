// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Fv1PlateReverb implementation. See the header for the topology and the
// parameter->physical mappings (which match docs/FX_FV1_DESIGN.md verbatim so
// the UI readouts agree). The audio path is pure 24-bit fixed-point (Q.23) with
// saturating arithmetic; coefficients are 14-bit via q14().

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "dsp/fx/fv1/Fv1PlateReverb.h"

namespace parvati::fv1
{

// FV-1 RAM budget: the sum of every fixed-point delay line in this effect must
// fit in the 32768-sample budget (1.0 s at 32.768 kHz).
static_assert (Fv1PlateReverb::kTotalMemory <= kMaxMemorySamples,
               "Fv1PlateReverb: total delay memory exceeds the 32768-sample budget");

Fv1PlateReverb::Fv1PlateReverb()
: quarter14_ (q14 (0.25f))
, apGain14_ (q14 (0.7f))
{
}

void Fv1PlateReverb::setParams (const float param[5])
{
    const float p0 = std::clamp (param[0], 0.0f, 1.0f);
    const float p1 = std::clamp (param[1], 0.0f, 1.0f);
    const float p2 = std::clamp (param[2], 0.0f, 1.0f);
    const float p3 = std::clamp (param[3], 0.0f, 1.0f);
    // param[4] is UNUSED (Mix is the chain Dry/Wet — never read here).

    // Predelay 0..100 ms -> samples (cap at the predelay line capacity - 1).
    int pd = static_cast<int> (std::lround (p0 * 0.1f * 32768.0f));
    if (pd < 0) pd = 0;
    if (pd > kPredelayCap - 1) pd = kPredelayCap - 1;
    predelayLen_ = pd;

    // Decay 0.1..4.0 s -> comb feedback g = pow(10, -3/(decay*fs)), clamped.
    const float decay = 0.1f + p1 * (4.0f - 0.1f);
    float g = std::pow (10.0f, -3.0f / (decay * 32768.0f));
    if (g < 0.0f) g = 0.0f;
    if (g > 0.999f) g = 0.999f;
    g14_ = q14 (g);

    // Damping 500..12000 Hz -> 1-pole LP cutoff in each comb loop.
    const float fc = 500.0f * std::pow (24.0f, p2);
    lp0_.setCutoff (fc);
    lp1_.setCutoff (fc);
    lp2_.setCutoff (fc);
    lp3_.setCutoff (fc);

    // Mod 0..1 -> allpass delay-length LFO amplitude 0..15 samples.
    modDepth_ = p3 * 15.0f;
}

void Fv1PlateReverb::resetInternal()
{
    predelay_.clear();
    comb0_.clear();
    comb1_.clear();
    comb2_.clear();
    comb3_.clear();
    ap0_.clear();
    ap1_.clear();
    lp0_.clear();
    lp1_.clear();
    lp2_.clear();
    lp3_.clear();
    apPhase0_ = 0.0f;
    apPhase1_ = 0.0f;
}

void Fv1PlateReverb::processSampleFx (int32_t lin, int32_t /*rin*/, int32_t& lout, int32_t& rout)
{
    // Mono input (lin is the duplicated L==R). Predelay, then feed the 4 combs.
    predelay_.write (lin);
    const int32_t pd = predelay_.read (predelayLen_);

    // ---- Four parallel lowpass-combs (damping LP inside each feedback loop) ----
    const int32_t o0 = comb0_.read (kCombD[0]);
    const int32_t o1 = comb1_.read (kCombD[1]);
    const int32_t o2 = comb2_.read (kCombD[2]);
    const int32_t o3 = comb3_.read (kCombD[3]);

    const int32_t d0 = lp0_.process (o0);
    const int32_t d1 = lp1_.process (o1);
    const int32_t d2 = lp2_.process (o2);
    const int32_t d3 = lp3_.process (o3);

    comb0_.write (f24_addSat (pd, f24_mulk (d0, g14_)));
    comb1_.write (f24_addSat (pd, f24_mulk (d1, g14_)));
    comb2_.write (f24_addSat (pd, f24_mulk (d2, g14_)));
    comb3_.write (f24_addSat (pd, f24_mulk (d3, g14_)));

    // Sum the raw comb outputs (pre-damping) and average by 0.25. Sum the four
    // Q.23 values in a plain int32 FIRST (4x <= 2^25 fits comfortably, no wrap)
    // then a SINGLE f24_mulk(0.25); per-add f24_sat here would clip prematurely
    // (four combs at 0.6 each would clip at 1.0 -> 0.25 instead of 0.6).
    const int32_t sum = o0 + o1 + o2 + o3;
    int32_t in = f24_mulk (sum, quarter14_);

    // ---- Two series Schroeder allpasses with slow delay-length modulation ----
    //   r   = ap.readFrac(D)          ; D = baseD + round(lfo*depth)
    //   y   = -g*in + r               ; g ~ 0.7
    //   ap.write(in + g*y)            ; Schroeder allpass form
    {
        float dl = static_cast<float> (kApBaseD[0])
                   + std::round (lutSine32 (apPhase0_) * modDepth_);
        if (dl < 1.0f) dl = 1.0f;
        if (dl > static_cast<float> (kApCap0 - 1)) dl = static_cast<float> (kApCap0 - 1);
        const int32_t r = ap0_.readFrac (dl);
        const int32_t y = f24_addSat (-f24_mulk (in, apGain14_), r);
        ap0_.write (f24_addSat (in, f24_mulk (y, apGain14_)));
        in = y;
        apPhase0_ += kApInc0;
        if (apPhase0_ >= 1.0f) apPhase0_ -= 1.0f;
    }
    {
        float dl = static_cast<float> (kApBaseD[1])
                   + std::round (lutSine32 (apPhase1_) * modDepth_);
        if (dl < 1.0f) dl = 1.0f;
        if (dl > static_cast<float> (kApCap1 - 1)) dl = static_cast<float> (kApCap1 - 1);
        const int32_t r = ap1_.readFrac (dl);
        const int32_t y = f24_addSat (-f24_mulk (in, apGain14_), r);
        ap1_.write (f24_addSat (in, f24_mulk (y, apGain14_)));
        in = y;
        apPhase1_ += kApInc1;
        if (apPhase1_ >= 1.0f) apPhase1_ -= 1.0f;
    }

    // Wet mono -> both channels (mono-in / stereo-out). Dry/wet is the chain.
    lout = in;
    rout = in;
}

} // namespace parvati::fv1

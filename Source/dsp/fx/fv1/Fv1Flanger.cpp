// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Fv1Flanger implementation — short modulated delay, 180-deg stereo phases,
// damped feedback up to 0.92.

#include "dsp/fx/fv1/Fv1Flanger.h"

#include <algorithm>
#include <cmath>

namespace parvati::fv1
{

// BASE-DELAY GLIDE (2026-08-21, the Fv1Echo/Fv1ClockedDelay idiom): the
// Manual knob steps the read position at the sub-chunk cadence; a fast drag
// jumps it ~2.5 samples per tick. The Q16 one-pole below (k=1/256, capped at
// ~0.25 sample/internal-sample, sub-1/16 tail snapped, never stalls below one
// quantum) turns every Manual move into a tape-speed pitch bend — click-free.
namespace
{
constexpr int32_t kFlQOne    = 65536;
constexpr int32_t kFlGlideCapQ = 16384;
constexpr int kFlGlideShift = 8;

inline void flGlideTapQ16 (int32_t& cur, int32_t target) noexcept
{
    const int32_t deltaQ = target - cur;
    const int32_t distQ  = (deltaQ < 0) ? -deltaQ : deltaQ;
    if (distQ <= kFlQOne / 16) { cur = target; return; }
    int32_t stepQ = deltaQ >> kFlGlideShift;
    if (stepQ >  kFlGlideCapQ) stepQ =  kFlGlideCapQ;
    if (stepQ < -kFlGlideCapQ) stepQ = -kFlGlideCapQ;
    if (stepQ == 0) stepQ = (deltaQ > 0) ? 1 : -1;
    cur += stepQ;
}
} // namespace

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
    fbDc_.clear();
    baseQL_ = 0;   // glide sentinel: snap on the next first sample
    line_.clear();
    damp_.clear();
    phase_ = 0.0f;
}

void Fv1Flanger::processSampleFx (int32_t lin, int32_t /*rin*/,
                                  int32_t& lout, int32_t& rout)
{
    // Glide the base toward its target (snap on the first sample after a
    // reset: baseQL_ == 0 is the "unset" sentinel), then the two sweep
    // phases 180 deg apart read the GLIDING base.
    {
        const int32_t targetQ = static_cast<int32_t> (std::lround (
            static_cast<double> (baseSamp_) * 65536.0));
        if (baseQL_ <= 1) baseQL_ = targetQ;
        else              flGlideTapQ16 (baseQL_, targetQ);
    }
    const float baseNow = static_cast<float> (baseQL_) * (1.0f / 65536.0f);
    const float dl = baseNow + depthSamp_ * lutSine32 (phase_);
    const float dr = baseNow + depthSamp_ * lutSine32 (phase_ + 0.5f);
    const int32_t rL = line_.readFrac (dl);
    const int32_t rR = line_.readFrac (dr);

    // One feedback loop on the L-phase tap, damped (jet regeneration).
    // SOFT-SATURATED loop write (2026-08-21 — the flanger "crackle" fix): the
    // regen loop at high feedback resonates ~1/(1-fb) (12.5x at fb 0.92), so
    // the loop signal LEGALLY exceeds the Q.23 rail on a unity input — the old
    // f24_addSat hard-clipped every recirculation, squaring the resonant
    // buildup: measured -58 dB inharmonic foldback splatter on a PURE SINE
    // (tests/parvati_fx_foldback_probe). The float-domain soft knee below is
    // transparent up to +/-0.6 (C1 at the knee) and eases into the rail like
    // a regen analog stage — the jet character stays, the edges go.
    const int32_t fbTap = damp_.process (rL);
    {
        // Knee + DC killer on the FEEDBACK COMPONENT only (2026-08-21
        // refinement): killing the whole write added a small phase lead that
        // perturbed near-tie lag measurements; in the RETURN branch (the
        // Echo/ClockedDelay placement) the input path stays pristine while
        // the loop DC gain still drops to ~0 (fb * |HP(0)| = 0).
        float fbF = f24_toFloat (fbTap)
                  * (static_cast<float> (fb14_) * (1.0f / 8191.0f));
        if (fbF > 0.6f)       fbF =  0.6f + 0.4f * std::tanh (( fbF - 0.6f) * 2.5f);
        else if (fbF < -0.6f) fbF = -0.6f - 0.4f * std::tanh ((-fbF - 0.6f) * 2.5f);
        line_.write (f24_addSat (lin, f24_mulk (fbDc_.process (f24_fromFloat (fbF)), fb14_)));
    }

    phase_ += inc_;
    phase_ -= std::floor (phase_);

    lout = rL;
    rout = rR;
}

} // namespace parvati::fv1

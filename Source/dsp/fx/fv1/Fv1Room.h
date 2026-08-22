// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Fv1Room — the canonical Schroeder room: four parallel lowpass-damped combs
// -> sum -> TWO series-allpass chains with slightly different lengths per
// side (decorrelated stereo from one comb bank — the "boingy 1980s digital
// room" character). Delays are mutually prime; feedback via 14-bit coeffs.
//
// Params (param[4] is UNUSED; Mix is the chain Dry/Wet):
//   * Decay (p0): 0.1..3 s -> PER-COMB feedback
//     g_i = 10^(-3*D_i/(decay*fs)) (per-pass RT60 law: t60 == Decay by
//     construction; D_i = the comb delay. The [0,0.999] clamp is a
//     never-engaging stability guard).
//   * Damp  (p1): 500..12000 Hz loop LP.
//   * Width (p2): 0 = mono (both outs = the L chain), 1 = full stereo.
//   * Tone  (p3): 700..15000 Hz output LP.

#ifndef PARVATI_DSP_FX_FV1_FV1ROOM_H
#define PARVATI_DSP_FX_FV1_FV1ROOM_H

#include <cstdint>

#include "dsp/fx/fv1/Fv1FxProcessor.h"

namespace parvati::fv1
{

class Fv1Room : public Fv1FxProcessor
{
public:
    Fv1Room();

    void setParams (const std::array<float, kNumFxSlotParams>& param) override;
    void resetInternal() override;
    FxType type() const noexcept override { return FxType::Room; }

protected:
    void processSampleFx (int32_t lin, int32_t rin, int32_t& lout, int32_t& rout) override;

private:
    // Mutually-prime Schroeder comb lengths + per-side allpass chains.
    static constexpr int kCombD[4]     = { 1687, 1601, 2053, 2251 };
    static constexpr int kApL[2]       = { 191, 281 };
    static constexpr int kApR[2]       = { 179, 271 };

    DelayLine<2048> comb0_, comb1_;
    DelayLine<4096> comb2_, comb3_;
    DelayLine<512> apL0_, apL1_, apR0_, apR1_;
    OnePoleLpFx lp_[4];
    OnePoleLpFx toneLpL_, toneLpR_;   // one per channel (a shared one would run 2x rate)

    LoopDcKiller dck_[4] {};   // loop DC killers (see Fv1Engine.h)
    int16_t g14_[4] = {};   // q14(per-comb feedback g_i — see setParams)
    int16_t quarter14_ = 0;
    int16_t apGain14_ = 0;
    int16_t width14_ = 0;      // q14(Width): crossfades R between the L chain and its own
    int16_t invWidth14_ = 0;   // q14(1-Width)  (precomputed — no per-sample q14)
};

} // namespace parvati::fv1

#endif // PARVATI_DSP_FX_FV1_FV1ROOM_H

// Copyright (c) 2026 805Labs Kft. / Hellcat.
//
// Fv1Spring — a dispersive-network spring reverb: each spring is a feedback
// loop of [long delay -> cascade of SIX short allpasses -> damping LP], with a
// soft-clipped driver. The short-AP cascade IS the dispersion — transients
// emerge chirped ("boing"), the classic spring signature that no plain delay
// loop has. Two springs with slightly different lengths/AP orders give
// natural stereo; Width blends the second spring in.
//
// Params (param[4] is UNUSED; Mix is the chain Dry/Wet):
//   * Decay (p0): 0.2..4 s -> PER-SPRING loop feedback
//     g_s = 10^(-3*D_s/(decay*fs))*(1-0.25*Chirp) (per-pass RT60 law:
//     t60 == Decay at Chirp 0; D_s = the per-spring loop length. Chirp
//     trades tail length for dispersion).
//   * Damp  (p1): 500..8000 Hz loop LP.
//   * Chirp (p2): AP coefficient 0.35..0.95 (dispersion strength = boing).
//   * Width (p3): 0 = one spring (TRUE mono: L and R are bit-identical),
//     1 = both springs (decorrelated stereo).

#ifndef HELLCAT_DSP_FX_FV1_FV1SPRING_H
#define HELLCAT_DSP_FX_FV1_FV1SPRING_H

#include <cstdint>

#include "dsp/fx/fv1/Fv1FxProcessor.h"

namespace hellcat::fv1
{

class Fv1Spring : public Fv1FxProcessor
{
public:
    void setParams (const std::array<float, kNumFxSlotParams>& param) override;
    void resetInternal() override;
    FxType type() const noexcept override { return FxType::Spring; }

protected:
    void processSampleFx (int32_t lin, int32_t rin, int32_t& lout, int32_t& rout) override;

private:
    // Spring A is ODD (2026-08-21, subagent audit): 1146 (even) + the six
    // odd-length chirp APs (each exactly -1 at Nyquist) gave total loop phase
    // 0 mod 2*pi at Nyquist = positive feedback with ~0.60 loop gain on a
    // single damp pole — the phaser-crackle regime measured as insufficient.
    // 1145 keeps the ~35 ms loop but makes the total Nyquist phase pi
    // (negative feedback). Spring B (1123) was already odd/safe.
    static constexpr int kSpringDelay[2] = { 1145, 1123 };          // ~35 ms loops
    static constexpr int kApLen[2][6] = { { 23, 29, 31, 37, 41, 43 },
                                          { 21, 27, 33, 39, 43, 47 } };
    DelayLine<2048> delay_[2];
    DelayLine<64> aps_[2][6];
    OnePoleLpFx damp_[2];

    int16_t fb14_[2]    = {};   // q14(per-spring loop gain — see setParams)
    LoopDcKiller dck_[2] {};    // loop DC killers (see Fv1Engine.h)
    int16_t chirp14_  = 0;
    int16_t width14_  = 0;      // q14(Width): crossfades R from spring A (mono) to spring B
    int16_t invWidth14_ = 8191; // q14(1-Width)  (precomputed — no per-sample q14)
};

} // namespace hellcat::fv1

#endif // HELLCAT_DSP_FX_FV1_FV1SPRING_H

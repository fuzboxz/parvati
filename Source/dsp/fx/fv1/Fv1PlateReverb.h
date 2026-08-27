// Copyright (c) 2026 805Labs Kft. / Hellcat.
//
// Fv1PlateReverb — FV-1 hardware-emulation "Sparse Digital Plate Reverb"
// (Schroeder/Moorer topology): predelay -> four parallel lowpass-comb filters ->
// two series Schroeder allpasses with slow delay-length modulation. The whole
// audio path is 24-bit fixed-point (Q.23) with saturating arithmetic and 14-bit
// coefficients; JUCE-FREE (built on Fv1FxProcessor / Fv1Engine).
//
// Params (param[4] is UNUSED; Mix is the chain Dry/Wet):
//   * Predelay (p0): 0..100 ms linear   -> predelay samples.
//   * Decay    (p1): 0.1..4.0 s          -> PER-COMB feedback
//     g_i = pow(10, -3*D_i/(decay*fs)) (per-pass RT60 law: t60 == Decay by
//     construction; D_i = the comb delay. The [0,0.999] clamp is a
//     never-engaging stability guard).
//   * Damping  (p2): 500..12000 Hz       -> 1-pole LP cutoff in each comb loop.
//   * Mod      (p3): 0..1                -> allpass delay-length LFO depth
//     0..15 samples (the short AP1 line clamps its swing to 14 — capacity
//     guard; read pointers sweep fractionally via readFrac).

#ifndef HELLCAT_DSP_FX_FV1_FV1PLATEREVERB_H
#define HELLCAT_DSP_FX_FV1_FV1PLATEREVERB_H

#include <cstdint>

#include "dsp/fx/fv1/Fv1Engine.h"
#include "dsp/fx/fv1/Fv1FxProcessor.h"

namespace hellcat::fv1
{

class Fv1PlateReverb : public Fv1FxProcessor
{
public:
    Fv1PlateReverb();

    void setParams (const std::array<float, kNumFxSlotParams>& param) override;
    void resetInternal() override;
    FxType type() const override { return FxType::PlateReverb; }

protected:
    void processSampleFx (int32_t lin, int32_t rin, int32_t& lout, int32_t& rout) override;

private:
    // ---- Delay-line capacities (powers of two) & fixed delay values ----
    // NOTE: the 7 read/write pointers below EXCEED the general FV-1 "<= 4
    // pointers" guideline, but this is the MANDATED Schroeder/Moorer plate
    // topology the spec calls for (predelay + 4 combs + 2 allpasses), an
    // implicit override of the guideline exactly like the phaser's six allpass.
    static constexpr int kPredelayCap = 4096;   // >= 100 ms (3277 samples)
    static constexpr int kCombCap0 = 2048;      // delay value 1427
    static constexpr int kCombCap1 = 4096;      // delay value 2063
    static constexpr int kCombCap2 = 4096;      // delay value 3187
    static constexpr int kCombCap3 = 8192;      // delay value 4759
    static constexpr int kApCap0   = 512;       // base delay value 347
    static constexpr int kApCap1   = 128;       // base delay value 113
    // Mutually-prime comb delay values + allpass base delay values (samples).
    static constexpr int kCombD[4]   = { 1427, 2063, 3187, 4759 };
    static constexpr int kApBaseD[2] = { 347, 113 };
    // Slow allpass delay-length modulation LFOs (~0.3 Hz / ~0.5 Hz).
    static constexpr float kApRate0 = 0.3f;
    static constexpr float kApRate1 = 0.5f;
    static constexpr float kApInc0  = kApRate0 / static_cast<float> (kInternalRate);
    static constexpr float kApInc1  = kApRate1 / static_cast<float> (kInternalRate);

    // ---- Fixed-point state (24-bit Q.23) ----
    DelayLine<kPredelayCap> predelay_;
    DelayLine<kCombCap0> comb0_;
    DelayLine<kCombCap1> comb1_;
    DelayLine<kCombCap2> comb2_;
    DelayLine<kCombCap3> comb3_;
    DelayLine<kApCap0> ap0_;
    DelayLine<kApCap1> ap1_;
    OnePoleLpFx lp0_, lp1_, lp2_, lp3_;   // damping LP inside each comb loop

    // ---- Cached control settings (14-bit quantized; set in setParams) ----
    int predelayLen_ = 0;      // predelay samples [0, kPredelayCap-1]
    int16_t g14_[4]  = {};     // q14(per-comb feedback g_i — see setParams)
    LoopDcKiller dck_[4] {};   // loop DC killers (see Fv1Engine.h; comb loops)
    float modDepth_  = 0.0f;   // allpass LFO amplitude [0,15] samples
    int16_t quarter14_ = 0;    // q14(0.25)  (set once in the ctor)
    int16_t apGain14_  = 0;    // q14(0.7)   (set once in the ctor)
    float apPhase0_ = 0.0f;    // running allpass LFO phases [0,1)
    float apPhase1_ = 0.0f;

public:
    // Total fixed-point delay memory (sum of every DelayLine capacity). The .cpp
    // static_asserts this stays within the 32768-sample FV-1 RAM budget.
    static constexpr int kTotalMemory = kPredelayCap + kCombCap0 + kCombCap1 + kCombCap2
                                        + kCombCap3 + kApCap0 + kApCap1;
};

} // namespace hellcat::fv1

#endif // HELLCAT_DSP_FX_FV1_FV1PLATEREVERB_H

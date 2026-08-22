// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Fv1VinylCompressor — Lo-Fi Vinyl Compressor, part of the FV-1 hardware-
// emulation FX family. Tuned after the Roland SP-303/SP-404 "Vinyl Sim" COMP
// ("the compression feel, a unique part of the analog record's sound"):
// deep feed-forward squash with lots of makeup, soft analog saturation glue,
// slow heavy wow + faster flutter, and a SUBTLE vinyl noise floor (soft ticks
// + hiss — never full-scale pops).
//
// Signal path:
//   feed-forward peak compressor (sidechain in float, gain applied in
//   fixed-point) -> cubic soft-saturation "lathe" (fixed-point, warmth rises
//   with Compress) -> pitch-warp (slow wow + fast flutter on a modulated
//   50 ms delay) -> age low-pass -> vinyl noise (decaying soft ticks + low
//   hiss) mixed at the output.
//
// JUCE-FREE: only the FV-1/framework headers + <cstdint>, so it compiles
// standalone in seconds for its unit test, exactly like the rest of the family.

#ifndef PARVATI_DSP_FX_FV1_FV1VINYLCOMPRESSOR_H
#define PARVATI_DSP_FX_FV1_FV1VINYLCOMPRESSOR_H

#include <cstdint>

#include "dsp/fx/fv1/Fv1FxProcessor.h"

namespace parvati::fv1
{

// Params (generic 0..1 slot params; param[4] is the chain dry/wet, never read):
//   param[0] Compress : SP-style macro. Threshold th = 1 - 0.96*p (down to
//                       0.04), ratio 4:1 -> ~16:1 (exp 0.75+0.19*p), makeup
//                       mg = 1 + 5*p (max 6.0), saturation drive a =
//                       0.06+0.25*p. Feed-forward peak detector.
//   param[1] Wow/Flut : slow wow (0.4 Hz, up to 300 samples ~ 2.3 % pitch
//                       deviation — the audible "thumb on the record"
//                       warble) + fast flutter (3.1 Hz, up to 24 samples ~
//                       1.4 %) on the 50 ms (= 1638 sample) delay read
//                       pointer. (24-sample wow was sub-audible at 0.18 %.)
//   param[2] Crackle  : vinyl noise floor: sparse soft ticks (decaying
//                       envelope, skewed to small amplitudes, ceiling ~0.18)
//                       + low hiss (~-54 dB). Never full-scale pops.
//   param[3] Age      : 700..15000 Hz 1-pole LP cutoff (fc = 700*pow(15000/700,p)).
class Fv1VinylCompressor : public Fv1FxProcessor
{
public:
    Fv1VinylCompressor() = default;

    void setParams (const std::array<float, kNumFxSlotParams>& param) override;
    void prepareInternal (double sampleRate, int maxBlock) override;
    void resetInternal() override;
    FxType type() const noexcept override;

private:
    void processSampleFx (int32_t lin, int32_t rin,
                          int32_t& lout, int32_t& rout) override;

    // Cached, clamped 0..1 params (param[4] is the chain dry/wet — never read).
    float pCompress_ = 0.0f;
    float pWow_      = 0.0f;
    float pCrackle_  = 0.0f;
    float pAge_      = 0.0f;

    // Compressor sidechain (float detector; the gain is applied in fixed-point).
    float env_     = 0.0f;
    float th_      = 1.0f;   // 1 - 0.96*pCompress (down to 0.04)
    float ratioExp_ = 0.75f; // gain = (th/env)^(0.75+0.19p): 4:1 .. ~16:1
    float makeup_  = 1.0f;   // 1 + 5*pCompress (max 6.0)
    // Attack/release one-pole coefficients (fixed at the FV-1 internal rate):
    // fast attack (0.8 ms — vinyl mastering glue) + slow release (250 ms —
    // the SP pump).
    float attackA_  = 0.0f;  // 1 - exp(-1/(0.0008*32768))
    float releaseA_ = 0.0f;  // 1 - exp(-1/(0.250*32768))

    // Cubic soft-saturation "lathe" (fixed-point): y = x - a*x^3 with a flat
    // top at yMax = (2/3)*xKnee, xKnee = 1/sqrt(3a). Unity slope at 0
    // (transparent when idle), compressive above — the analog glue + warmth.
    // Both stages quantized to 14-bit; drive a rises with Compress.
    int16_t satA14_   = 0;   // q14(a)  (0 until the first setParams -> linear)
    int32_t satMaxQ_  = kMaxQ23;   // flat-top ceiling in Q.23 (default: no clip)
    int32_t satClampQ_ = static_cast<int32_t> (1.6f * static_cast<float> (kOneQ23));   // pre-cubic input clamp

    // Wow/flutter modulated delay (50 ms = 1638 samples at 32.768 kHz).
    DelayLine<2048> delay_;
    float ph1_ = 0.0f;       // 0.4 Hz slow wow sine phase (heavy, warpy)
    float ph2_ = 0.0f;       // 3.1 Hz flutter sine phase

    // Age low-pass (1-pole, fixed-point; cutoff = Age param).
    OnePoleLpFx ageLp_;

    // Vinyl noise (LCG): sparse soft ticks (decaying envelope) + low hiss.
    uint32_t lcgState_ = 0u;
    float tickEnv_     = 0.0f;   // current tick amplitude (decays 0.55/sample)
};

} // namespace parvati::fv1

#endif // PARVATI_DSP_FX_FV1_FV1VINYLCOMPRESSOR_H

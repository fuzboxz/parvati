// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.
//
// Fv1JunoChorus — the Dual-BBD Chorus. This effect ports the documented
// Roland Juno-60/106 chorus configuration into the FV-1 family. See the
// configuration table in Fv1JunoChorus.cpp and docs/FX_FV1_DESIGN.md.
//
// Configuration (documented, community consensus from the service manuals):
//   * TWO BBD lines (MN3007 class, 1024 stages), one per stereo side.
//   * ONE shared sine LFO drives both lines; line 2 runs at INVERTED phase.
//     The opposite-phase pair is the signature of the source chorus.
//   * Mode I: LFO about 0.56 Hz, depth about 2.5 ms. Mode II: about 1.13 Hz,
//     deeper (about 4.0 ms), brighter (wider low-pass on the wet path).
//   * The output stage sums dry + line 1 (left-dominant) + line 2
//     (right-dominant). The lines have NO feedback loop. The source hardware
//     mixes the wet signal at a fixed level below the dry signal.
//
// Params (this effect reads FOUR slots; the chain Dry/Wet stays a separate
// control like every family member):
//   * Mode     (p0): 0..1 maps Chorus I / Chorus II (below 0.5 = I).
//   * Rate     (p1): trim, 0.5x..2x the mode rate (center 1.0x, log law).
//   * Depth    (p2): trim, 0..2x the mode depth (0.5 = the stock depth).
//   * Mix      (p3): internal dry/wet blend (see the mix law below). This
//                  param is a deliberate family deviation: the source chorus
//                  sums dry and wet INSIDE the effect, so the stock ratio
//                  must live inside it too.
//   * param[4] is unused.

#ifndef HELLCAT_DSP_FX_FV1_FV1JUNOCHORUS_H
#define HELLCAT_DSP_FX_FV1_FV1JUNOCHORUS_H

#include <cstdint>

#include "dsp/fx/fv1/Fv1FxProcessor.h"

namespace hellcat::fv1
{

// One-pole high-pass with a settable cutoff, fixed-point in/out. Fv1Engine's
// LoopDcKiller pins its pole at 10 Hz; the BBD leak target is near 30 Hz, so
// this local section mirrors that structure with a settable cutoff.
struct BbdLeakHp
{
    float x1 = 0.0f, y1 = 0.0f;
    float pole = 0.999f;   // 1 - 2*pi*fc/fs

    void clear() noexcept { x1 = y1 = 0.0f; }
    void setCutoff (float fc) noexcept
    {
        pole = 1.0f - 6.28318530718f * fc / static_cast<float> (kInternalRate);
    }
    int32_t process (int32_t x) noexcept
    {
        const float xf = f24_toFloat (x);
        const float y  = xf - x1 + pole * y1;
        x1 = xf;
        y1 = y;
        return f24_fromFloat (y);
    }
};

class Fv1JunoChorus : public Fv1FxProcessor
{
public:
    void setParams (const std::array<float, kNumFxSlotParams>& param) override;
    void resetInternal() override;
    FxType type() const noexcept override { return FxType::JunoChorus; }

protected:
    void processSampleFx (int32_t lin, int32_t rin, int32_t& lout, int32_t& rout) override;

private:
    DelayLine<2048> lineL_, lineR_;   // 65.5 ms capacity; lines run at ~26..30 ms
    float inc_ = 0.0f;                // LFO phase increment per internal sample
    float phase_ = 0.0f;              // ONE shared LFO phase
    float depthSamp_ = 0.0f;          // sweep depth in samples
    float centerSamp_ = 0.0f;         // center delay in samples
    int16_t dry14_  = 8191;           // q14 dry weight
    int16_t line14_ = 0;              // q14 per-line weight
    OnePoleLpFx lpLa_ {}, lpRa_ {};   // per-line post-BBD low-pass, stage 1
    OnePoleLpFx lpLb_ {}, lpRb_ {};   // per-line post-BBD low-pass, stage 2
    BbdLeakHp hpL_ {}, hpR_ {};       // per-line clock-leak high-pass
};

} // namespace hellcat::fv1

#endif // HELLCAT_DSP_FX_FV1_FV1JUNOCHORUS_H

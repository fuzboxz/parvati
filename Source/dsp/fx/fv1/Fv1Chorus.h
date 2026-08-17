// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Fv1Chorus — the classic single-delay-per-side chorus (the clean sibling of
// the BBD Ensemble): two slightly detuned SIN LFOs (the canonical AN-0001
// technique), independent voices panned L/R, light feedback. No BBD coloration
// — plain "bread and butter" chorus.
//
// Params (param[4] is UNUSED; Mix is the chain Dry/Wet):
//   * Rate     (p0): 0.1..8 Hz (log) — both LFOs scale together.
//   * Depth    (p1): 0..6 ms of LFO sweep.
//   * Center   (p2): 5..25 ms base delay.
//   * Feedback (p3): 0..0.5 (light regeneration into both lines).

#ifndef PARVATI_DSP_FX_FV1_FV1CHORUS_H
#define PARVATI_DSP_FX_FV1_FV1CHORUS_H

#include <cstdint>

#include "dsp/fx/fv1/Fv1FxProcessor.h"

namespace parvati::fv1
{

class Fv1Chorus : public Fv1FxProcessor
{
public:
    void setParams (const float param[5]) override;
    void resetInternal() override;
    FxType type() const noexcept override { return FxType::Chorus; }

protected:
    void processSampleFx (int32_t lin, int32_t rin, int32_t& lout, int32_t& rout) override;

private:
    DelayLine<2048> lineL_, lineR_;   // 25+6 ms max swing fits comfortably
    float inc_ = 0.0f;                // LFO phase increment
    float phase_ = 0.0f;              // L voice phase; R trails by 0.30
    float depthSamp_ = 0.0f;
    float centerSamp_ = 328.0f;
    int16_t fb14_ = 0;
};

} // namespace parvati::fv1

#endif // PARVATI_DSP_FX_FV1_FV1CHORUS_H

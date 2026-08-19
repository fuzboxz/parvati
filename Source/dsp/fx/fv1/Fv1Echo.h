// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Fv1Echo — stereo ping-pong digital echo: input -> L delay -> R delay ->
// cross-feedback, tone-damped. The two 16384-sample rings consume the FV-1's
// ENTIRE 32K-word delay RAM (like a real max-length EEPROM program). Spread
// stretches the R delay time (1x = dual echo, 2x = long bounce).
//
// Params (param[4] is UNUSED; Mix is the chain Dry/Wet):
//   * Time     (p0): 10..470 ms per side (log).
//   * Feedback (p1): 0..0.995 internal loop gain. The UI shows 0..100%; 100%
//                  reads as INFINITE (-0.04 dB per repeat — below perception,
//                  a tail that outlives the note). The loop stays stable: the
//                  Tone damper bleeds HF every round trip and sub-unity gain
//                  means DC cannot lock either. (The old 0.95 cap lost
//                  -0.45 dB/repeat — an audible pump-away that contradicted
//                  the "100%" readout.)
//   * Tone     (p2): 700..12000 Hz loop damper.
//   * Spread   (p3): R time factor 1..2x.
//
// TIME GLIDE: Time/Spread changes do NOT step the read taps instantly (a
// stepped tap is a hard read-pointer discontinuity = the click; the
// ~980 Hz param cadence would repeat it every knob notch). The taps glide
// as Q.16 fixed point toward their targets, exactly the Fv1ClockedDelay
// glide (one-pole k = 1/256, capped at ~0.25 sample/internal-sample so the
// pitch bend stays tape-like; sub-1/16-sample tails snap; never stalls).

#ifndef PARVATI_DSP_FX_FV1_FV1ECHO_H
#define PARVATI_DSP_FX_FV1_FV1ECHO_H

#include <cstdint>

#include "dsp/fx/fv1/Fv1FxProcessor.h"

namespace parvati::fv1
{

class Fv1Echo : public Fv1FxProcessor
{
public:
    void setParams (const float param[5]) override;
    void resetInternal() override;
    FxType type() const noexcept override { return FxType::Echo; }

protected:
    void processSampleFx (int32_t lin, int32_t rin, int32_t& lout, int32_t& rout) override;

private:
    // The two rings consume the ENTIRE FV-1 delay RAM budget — like a real
    // max-length FV-1 echo program (the .cpp static_asserts the exact fit).
    DelayLine<16384> lineL_, lineR_;
    OnePoleLpFx damp_;

    // Read-tap targets, set instantly by setParams (Time / Spread knobs).
    float timeTargetL_ = 328.0f;   // 10 ms default (internal samples)
    float timeTargetR_ = 328.0f;

    // The GLIDING read taps as Q.16 fixed point (samples << 16), slewed per
    // internal sample toward the targets — see the TIME GLIDE note in the
    // file header. 0 is the "unset" sentinel: legal glide values are always
    // >= 10 ms * 32768 * 65536, so the first block after (re)construction
    // snaps exactly instead of gliding in from zero.
    int32_t timeQL_ = 0;
    int32_t timeQR_ = 0;

    int16_t fb14_ = 0;
};

} // namespace parvati::fv1

#endif // PARVATI_DSP_FX_FV1_FV1ECHO_H

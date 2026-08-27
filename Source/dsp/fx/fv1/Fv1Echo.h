// Copyright (c) 2026 805Labs Kft. / Hellcat.
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
//                  a tail longer than the note). The loop stays stable: the
//                  Tone damper removes HF every round trip, and sub-unity gain
//                  prevents DC build-up. (The old 0.95 cap lost
//                  -0.45 dB/repeat — an audible decay that contradicted
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

#ifndef HELLCAT_DSP_FX_FV1_FV1ECHO_H
#define HELLCAT_DSP_FX_FV1_FV1ECHO_H

#include <cstdint>

#include "dsp/fx/fv1/Fv1FxProcessor.h"

namespace hellcat::fv1
{

class Fv1Echo : public Fv1FxProcessor
{
public:
    void setParams (const std::array<float, kNumFxSlotParams>& param) override;
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

    // LOOP DC KILLER (2026-08-21 — the delay->reverb->shaper "complete voice
    // dropout" root cause): at max Feedback (0.995 loop gain) the ping-pong
    // loop is a DC integrator with DC gain 1/(1-0.995) = 200x — any residual
    // input DC / saturation asymmetry accumulates until the loop parks near a
    // rail (measured dc -0.28 on a held chord through Echo->Plate, peak >1.0),
    // and the DC-heavy output makes any following shaper pin constant -> its own
    // DC blocker strips it -> gated silence. A 10 Hz one-pole high-pass in the
    // R->L hop kills the recirculation (loop DC gain ~0) while leaving the
    // audio-band regen untouched. The shared LoopDcKiller (Fv1Engine.h) is the
    // same float-domain recurrence the family's control math uses.
    LoopDcKiller dcKiller_;
};

} // namespace hellcat::fv1

#endif // HELLCAT_DSP_FX_FV1_FV1ECHO_H

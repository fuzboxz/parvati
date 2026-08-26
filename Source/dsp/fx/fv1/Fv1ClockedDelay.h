// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.
//
// Fv1ClockedDelay — DAW-synced "Clocked Delay" from the FV-1 hardware-emulation
// family. Mono-in, stereo-out (the chain duplicates the mono source to L==R and
// applies its own dry/wet blend; this effect emits only the wet signal).
//
// Tempo sync: the host BPM arrives via setTransport(); at the start of every
// process() block the integer base delay length is recomputed from the cached
// Sync param + BPM, picking one of eight note divisions
// {1/1,1/2,1/3,1/4,1/6,1/8,1/12,1/16}. The whole audio core then runs at the
// FV-1 32.768 kHz internal rate inside processSampleFx as pure 24-bit Q.23
// fixed-point with saturation, exactly as the base Fv1FxProcessor frames it.
//
// Per internal sample: GRIT (bit-truncation) + a TAPE-AGE 1-pole LP on the delay
// write path, a modulated fractional read (sine-LUT LFO on the read pointer,
// depth grows with Tape Age), and saturating feedback. Output is the delayed,
// grit+aged read duplicated to both channels.

#ifndef HELLCAT_DSP_FX_FV1_FV1CLOCKEDDELAY_H
#define HELLCAT_DSP_FX_FV1_FV1CLOCKEDDELAY_H

#include <cstdint>

#include "dsp/fx/FxTypes.h"
#include "dsp/fx/fv1/Fv1Engine.h"
#include "dsp/fx/fv1/Fv1FxProcessor.h"

namespace hellcat::fv1
{

class Fv1ClockedDelay : public Fv1FxProcessor
{
public:
    void prepareInternal (double sampleRate, int maxBlock) override;
    void resetInternal() override;
    void setParams (const std::array<float, kNumFxSlotParams>& param) override;
    void setTransport (double bpm, bool isPlaying) override;
    // Recompute the tempo-synced delay length at block start, then run the
    // BW-limited host<->internal fixed-point core.
    void process (float* L, float* R, int numSamples) override;

    FxType type() const noexcept override { return FxType::ClockedDelay; }

protected:
    void processSampleFx (int32_t lin, int32_t rin,
                          int32_t& lout, int32_t& rout) override;

private:
    // Pick the division from the cached Sync param + BPM and clamp the result
    // to [1, kMaxDelaySamples]. Called once per process() block (tempo-synced).
    void recomputeDelayLen();

    // One ring == the full FV-1 RAM budget (32768 samples == 1.0 s @ 32.768 kHz).
    DelayLine<32768> delay_;

    // Cached generic params (each clamped to [0,1] in setParams).
    float pSync_ = 0.0f;   // param[0] Sync (note-division select)
    float pFb_   = 0.0f;   // param[1] Feedback (display 0..0.95)
    float pAge_  = 0.0f;   // param[2] Tape Age
    float pGrit_ = 0.0f;   // param[3] Grit
    // param[4] is UNUSED (Mix is the chain Dry/Wet).

    // Derived control state (recomputed in setParams; quantized where noted).
    int16_t fbK14_       = 0;     // q14(pFb_*0.95) — feedback gain (14-bit)
    int     gritBits_    = 24;    // 24..8 effective bits for the delay-input grit
    float   ageLfoDepth_ = 0.0f;  // pAge_*6.0 — read-pointer LFO depth (samples)
    // LOOP DC KILLER state (2026-08-21 — see Fv1Echo.h's note; same integrator
    // class: near-unity regen accumulates input/saturation DC until the loop
    // parks near a rail, contaminating everything downstream).
    LoopDcKiller loopDc_;
    // OUTPUT DC blocker state (the Grit truncation-DC removal — see the
    // output comment in the .cpp).
    LoopDcKiller outDc_;
    // IN-LOOP HF DAMP (2026-08-21, subagent audit): the loop (fb 0.95,
    // integer tempo-synced reads = zero interpolation loss) had NO high-
    // frequency damping — tapeLp_ ages the INPUT branch only — so even-parity
    // delays put a pole at internal Nyquist with a 20x bound, right in the
    // bridge's resampler-artifact band (the phaser crackle mechanism).
    OnePoleLpFx fbDamp_[2] {};

    // Tempo-sync state (BPM persists across reset; it is transport, not DSP).
    double bpm_      = 120.0;  // host BPM from setTransport()
    int    delayLen_ = 1;      // base delay in internal samples [1, kMaxDelaySamples]

    // Tape-age read-pointer LFO (~0.6 Hz sine, 32-value LUT). Phase in [0,1).
    float lfoPhase_ = 0.0f;

    // Tape-age 1-pole LP on the delay write path (cutoff 2000..200 Hz).
    OnePoleLpFx tapeLp_;
};

} // namespace hellcat::fv1

#endif // HELLCAT_DSP_FX_FV1_FV1CLOCKEDDELAY_H

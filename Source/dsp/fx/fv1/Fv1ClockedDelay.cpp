// Copyright (c) 2026 805Labs Kft. / Hellcat.
//
// Fv1ClockedDelay implementation. See the header for the full signal-flow doc.
// JUCE-FREE: only the FV-1/framework headers + <cmath> are included.

#include "dsp/fx/fv1/Fv1ClockedDelay.h"

#include <algorithm>
#include <cmath>

namespace hellcat::fv1
{

// Total delay RAM: exactly one DelayLine<32768> (== kMaxMemorySamples == the full
// 1.0 s FV-1 budget), and no extra buffers. The single delay VALUE is runtime-
// clamped to kMaxDelaySamples inside recomputeDelayLen()/processSampleFx().
static_assert (DelayLine<32768>::capacity <= kMaxMemorySamples,
               "Fv1ClockedDelay total delay memory exceeds the FV-1 RAM budget");

namespace
{
// kMaxDelaySamples as a float, for the read-pointer clamp below.
constexpr float kMaxDelayF = static_cast<float> (kMaxDelaySamples);

// The tempo-synced base delay target, in whole internal samples, from the
// cached Sync param + BPM (clamped to [1, kMaxDelaySamples]). Pure: no state.
// Shared by recomputeDelayLen() (block-start snap) and processSampleFx()
// (per-sample glide target), so both always agree on the same math. The
// seconds law itself is fxlaw::clockedDelaySeconds (FxTypes.h), shared with
// the tail table.
inline int tempoDelayTargetSamples (float pSync, double bpm) noexcept
{
    const double delaySeconds = fxlaw::clockedDelaySeconds (static_cast<double> (pSync), bpm);
    long len = std::lround (delaySeconds * kInternalRate);
    if (len < 1)                len = 1;
    if (len > kMaxDelaySamples) len = kMaxDelaySamples;
    return static_cast<int> (len);
}

// delayLen_ repurposing (see processSampleFx): it stores the GLIDING base
// delay as Q.16 fixed point (samples << 16). Whole-sample targets snap to
// exact multiples of 65536, so the glide never loses its fractional residual
// (an int-samples one-pole would stall when the remaining distance < 1).
constexpr int32_t kDelayQOne = 65536;          // 1.0 sample in Q.16
} // namespace

void Fv1ClockedDelay::prepareInternal (double, int)
{
    // All fixed-point state is fixed-size (DelayLine<32768> + OnePoleLpFx + LFO
    // phase): no heap to reserve. State is cleared by resetInternal().
}

void Fv1ClockedDelay::resetInternal()
{
    loopDc_.clear();
    outDc_.clear();
    fbDamp_[0].clear();
    fbDamp_[1].clear();
    delay_.clear();
    tapeLp_.clear();
    lfoPhase_ = 0.0f;
    // Mark the Q.16 glide state "unset" (0 is never a legal glide value: it
    // only ever lives between snap-settled targets, all ≥ kDelayQOne). The
    // next block's recomputeDelayLen() then SNAPS to the target exactly, so a
    // fresh/reset instance never starts a long startup glide from zero.
    delayLen_ = 0;
}

void Fv1ClockedDelay::setParams (const std::array<float, kNumFxSlotParams>& param)
{
    pSync_ = std::clamp (param[0], 0.0f, 1.0f);
    pFb_   = std::clamp (param[1], 0.0f, 1.0f);
    pAge_  = std::clamp (param[2], 0.0f, 1.0f);
    pGrit_ = std::clamp (param[3], 0.0f, 1.0f);
    // param[4] is UNUSED (Mix is the chain Dry/Wet).

    // Feedback gain (display 0..0.95): quantize to 14-bit.
    fbK14_ = q14 (pFb_ * 0.95f);
    fbDamp_[0].setCutoff (5000.0f);   // in-loop HF damp (see header)
    fbDamp_[1].setCutoff (5000.0f);

    // Tape-age read-pointer LFO depth (0..~6 samples).
    ageLfoDepth_ = pAge_ * 6.0f;

    // Tape-age write-path LP cutoff: 2000 Hz (fresh) .. 200 Hz (old).
    const float fc = 2000.0f * (1.0f - pAge_) + 200.0f * pAge_;
    tapeLp_.setCutoff (fc);

    // Grit: effective bits 24..8 (24 - round(pGrit*16)).
    int bits = 24 - static_cast<int> (std::lround (pGrit_ * 16.0f));
    if (bits < 8)  bits = 8;
    if (bits > 24) bits = 24;
    gritBits_ = bits;
}

void Fv1ClockedDelay::setTransport (double bpm, bool isPlaying)
{
    (void) isPlaying;
    if (bpm > 0.0)
        bpm_ = bpm;
}

void Fv1ClockedDelay::recomputeDelayLen()
{
    // Block-start tempo-sync service. The TARGET is computed per internal
    // sample by processSampleFx() via tempoDelayTargetSamples(); here the only
    // remaining block-rate job is the first-set/exact snap (delayLen_ <= 1 is
    // the "unset" sentinel: the constructor's default 1, or resetInternal()'s
    // 0 — legal glide values are always ≥ kDelayQOne = 65536).
    if (delayLen_ <= 1)
        delayLen_ = tempoDelayTargetSamples (pSync_, bpm_) << 16;
}

void Fv1ClockedDelay::process (float* L, float* R, int numSamples)
{
    // Tempo-sync: service the delay-length glide state at block start (exact
    // snap on the first block after construction/reset).
    recomputeDelayLen();
    // Then run the BW-limited host<->internal fixed-point core.
    Fv1FxProcessor::process (L, R, numSamples);
}

void Fv1ClockedDelay::processSampleFx (int32_t lin, int32_t rin,
                                       int32_t& lout, int32_t& rout)
{
    (void) rin;   // mono-in: the chain duplicates the mono source to L==R.

    const int32_t mono = lin;

    // TEMPO-GLIDE: delayLen_ holds the base delay as Q.16 fixed point and
    // slews toward the current tempo target ONE INTERNAL SAMPLE AT A TIME
    // (the read is fractional — readFrac — so a moving base is a smooth
    // pitch bend, never a read-pointer jump). Before this fix the length
    // stepped by whole hundreds of samples at block boundaries on a
    // tempo/param change: a hard read-pointer discontinuity = the click.
    // Exponential one-pole (k = 1/256) capped at ~0.25 sample/sample, so the
    // pitch deviation stays tape-like; the sub-1/16-sample tail snaps exact
    // (inaudible) so the glide always settles instead of crawling. Reading
    // the target per sample also picks up sub-chunk Sync/BPM edits
    // immediately, matching the chain's ~980 Hz param cadence.
    const int32_t targetQ = static_cast<int32_t> (
        tempoDelayTargetSamples (pSync_, bpm_)) << 16;
    glideTapQ16 (delayLen_, targetQ);
    const float baseDelay = static_cast<float> (delayLen_)
                          * (1.0f / static_cast<float> (kDelayQOne));

    // GRIT on the delay input (bit-truncation, pure mask).
    const int32_t q = f24_quantBits (mono, gritBits_);

    // TAPE-AGE LP on the delay write path (24-bit fixed-point 1-pole).
    const int32_t lpOut = tapeLp_.process (q);

    // TAPE-AGE LFO on the read pointer (sine LUT, ~0.6 Hz; depth 0..~6 samples).
    float modDelay = baseDelay + lutSine32 (lfoPhase_) * ageLfoDepth_;
    // Hard-clamp the read VALUE to the documented max (<= kMaxDelaySamples) so a
    // clamped delayLen_ (32767) plus LFO depth never wraps the power-of-two ring.
    if (modDelay < 1.0f)   modDelay = 1.0f;
    if (modDelay > kMaxDelayF) modDelay = kMaxDelayF;

    // Modulated fractional read (the delayed, grit+aged signal).
    const int32_t readSamp = delay_.readFrac (modDelay);

    // Feedback write (saturating; feedback gain quantized to 14-bit) through
    // the LOOP DC KILLER (~10 Hz one-pole HP — see the header note): removes the
    // near-unity regen's DC accumulation without touching audio-band feedback.
    {
        const int32_t damped = fbDamp_[1].process (fbDamp_[0].process (loopDc_.process (readSamp)));
        delay_.write (f24_addSat (lpOut, f24_mulk (damped, fbK14_)));
    }

    // Advance the LFO phase (~0.6 Hz at 32.768 kHz).
    lfoPhase_ += 0.6f / 32768.0f;
    if (lfoPhase_ >= 1.0f) lfoPhase_ -= 1.0f;

    // Wet output: the delayed read, duplicated to both channels (stereo from
    // mono). OUTPUT DC BLOCKER (~10 Hz HP — caught by the fx-invariants [I2]
    // loop-DC test, 2026-08-21): the Grit stage is authentic TRUNCATION
    // quantization (the FV-1 AND-MASK), whose systematic truncation error is
    // a DC source (measured |mean|/rms 0.17 at 100% Grit) — blocked here so
    // the lo-fi character survives without contaminating downstream shapers
    // with DC.
    {
        const int32_t out = outDc_.process (readSamp);
        lout = out;
        rout = out;
    }
}

} // namespace hellcat::fv1

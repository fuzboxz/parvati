// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Fv1ClockedDelay implementation. See the header for the full signal-flow doc.
// JUCE-FREE: only the FV-1/framework headers + <cmath> are included.

#include "dsp/fx/fv1/Fv1ClockedDelay.h"

#include <cmath>

namespace parvati::fv1
{

// Total delay RAM: exactly one DelayLine<32768> (== kMaxMemorySamples == the full
// 1.0 s FV-1 budget), and no extra buffers. The single delay VALUE is runtime-
// clamped to kMaxDelaySamples inside recomputeDelayLen()/processSampleFx().
static_assert (DelayLine<32768>::capacity <= kMaxMemorySamples,
               "Fv1ClockedDelay total delay memory exceeds the FV-1 RAM budget");

namespace
{
// The eight clocked note divisions {1/1,1/2,1/3,1/4,1/6,1/8,1/12,1/16} expressed
// as their denominators: delaySeconds = (4/divisor) * (60/bpm).
constexpr double kDivisors[8] = { 1.0, 2.0, 3.0, 4.0, 6.0, 8.0, 12.0, 16.0 };

// kMaxDelaySamples as a float, for the read-pointer clamp below.
constexpr float kMaxDelayF = static_cast<float> (kMaxDelaySamples);

inline float clamp01 (float v) noexcept
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}
} // namespace

void Fv1ClockedDelay::prepareInternal (double, int)
{
    // All fixed-point state is fixed-size (DelayLine<32768> + OnePoleLpFx + LFO
    // phase): no heap to reserve. State is cleared by resetInternal().
}

void Fv1ClockedDelay::resetInternal()
{
    delay_.clear();
    tapeLp_.clear();
    lfoPhase_ = 0.0f;
}

void Fv1ClockedDelay::setParams (const float param[5])
{
    pSync_ = clamp01 (param[0]);
    pFb_   = clamp01 (param[1]);
    pAge_  = clamp01 (param[2]);
    pGrit_ = clamp01 (param[3]);
    // param[4] is UNUSED (Mix is the chain Dry/Wet).

    // Feedback gain (display 0..0.95): quantize to 14-bit.
    fbK14_ = q14 (pFb_ * 0.95f);

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
    int i = static_cast<int> (std::lround (pSync_ * 7.0));
    if (i < 0) i = 0;
    if (i > 7) i = 7;

    const double bpm = (bpm_ > 0.0) ? bpm_ : 120.0;
    const double delaySeconds = (4.0 / kDivisors[static_cast<size_t> (i)])
                              * (60.0 / bpm);
    long len = std::lround (delaySeconds * kInternalRate);
    if (len < 1)                len = 1;
    if (len > kMaxDelaySamples) len = kMaxDelaySamples;
    delayLen_ = static_cast<int> (len);
}

void Fv1ClockedDelay::process (float* L, float* R, int numSamples)
{
    // Tempo-sync: recompute the delay length from the host BPM at block start.
    recomputeDelayLen();
    // Then run the BW-limited host<->internal fixed-point core.
    Fv1FxProcessor::process (L, R, numSamples);
}

void Fv1ClockedDelay::processSampleFx (int32_t lin, int32_t rin,
                                       int32_t& lout, int32_t& rout)
{
    (void) rin;   // mono-in: the chain duplicates the mono source to L==R.

    const int32_t mono = lin;

    // GRIT on the delay input (bit-truncation, pure mask).
    const int32_t q = f24_quantBits (mono, gritBits_);

    // TAPE-AGE LP on the delay write path (24-bit fixed-point 1-pole).
    const int32_t lpOut = tapeLp_.process (q);

    // TAPE-AGE LFO on the read pointer (sine LUT, ~0.6 Hz; depth 0..~6 samples).
    float modDelay = static_cast<float> (delayLen_)
                   + lutSine32 (lfoPhase_) * ageLfoDepth_;
    // Hard-clamp the read VALUE to the documented max (<= kMaxDelaySamples) so a
    // clamped delayLen_ (32767) plus LFO depth never wraps the power-of-two ring.
    if (modDelay < 1.0f)   modDelay = 1.0f;
    if (modDelay > kMaxDelayF) modDelay = kMaxDelayF;

    // Modulated fractional read (the delayed, grit+aged signal).
    const int32_t readSamp = delay_.readFrac (modDelay);

    // Feedback write (saturating; feedback gain quantized to 14-bit).
    delay_.write (f24_addSat (lpOut, f24_mulk (readSamp, fbK14_)));

    // Advance the LFO phase (~0.6 Hz at 32.768 kHz).
    lfoPhase_ += 0.6f / 32768.0f;
    if (lfoPhase_ >= 1.0f) lfoPhase_ -= 1.0f;

    // Wet output: the delayed read, duplicated to both channels (stereo from mono).
    const int32_t out = readSamp;
    lout = out;
    rout = out;
}

} // namespace parvati::fv1

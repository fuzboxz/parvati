// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Fv1Ensemble implementation — BBD-style ensemble chorus. See Fv1Ensemble.h and
// docs/FX_FV1_DESIGN.md for the verbatim parameter mappings and algorithm.

#include "dsp/fx/fv1/Fv1Ensemble.h"

#include <algorithm>
#include <cmath>

namespace parvati::fv1
{

// Total delay memory for this effect: two 2048-sample power-of-two rings
// (capacity sized so the max read delay, Center 25 ms + Depth 15 ms = 1311
// samples, never wraps). Must stay within the FV-1 1.0 s RAM budget.
static_assert (2 * DelayLine<2048>::capacity <= kMaxMemorySamples,
               "Fv1Ensemble total delay memory exceeds the 32768-sample budget");

void Fv1Ensemble::prepareInternal (double /*sampleRate*/, int /*maxBlock*/)
{
    // Fixed-point state lives in the member delay lines; just clear it.
    lineA_.clear();
    lineB_.clear();
}

void Fv1Ensemble::resetInternal()
{
    lineA_.clear();
    lineB_.clear();
    phaseA_ = 0.0f;
    phaseB_ = 0.25f;
}

void Fv1Ensemble::setParams (const float param[5])
{
    const float p0 = std::clamp (param[0], 0.0f, 1.0f);   // Rate
    const float p1 = std::clamp (param[1], 0.0f, 1.0f);   // Depth
    const float p2 = std::clamp (param[2], 0.0f, 1.0f);   // Center
    const float p3 = std::clamp (param[3], 0.0f, 1.0f);   // Feedback
    // param[4] is UNUSED (Mix is the chain Dry/Wet — never read here).

    // Rate: 0.1..8 Hz (rate = 0.1*pow(80,p)); phase increment per 32.768 kHz
    // sample. (pow/exp only here in setParams, never per-sample in the core.)
    const float rate = 0.1f * std::pow (80.0f, p0);
    inc_ = rate / static_cast<float> (kInternalRate);

    // Depth: 0..15 ms -> samples at the internal rate.
    depthSamp_ = p1 * 15.0e-3f * static_cast<float> (kInternalRate);

    // Center: 2..25 ms -> samples at the internal rate.
    centerSamp_ = (2.0f + p2 * 23.0f) * 1.0e-3f * static_cast<float> (kInternalRate);

    // Feedback: -0.9..0.9, quantized to a 14-bit coefficient.
    const float fb = -0.9f + p3 * 1.8f;
    fb14_ = q14 (fb);
}

FxType Fv1Ensemble::type() const
{
    return FxType::Ensemble;
}

void Fv1Ensemble::processSampleFx (int32_t lin, int32_t /*rin*/,
                                   int32_t& lout, int32_t& rout)
{
    // Read each BBD line at the center delay + LFO-driven depth (sine LUT, no
    // per-sample trig). The two LFO phases sit 90 deg apart.
    const int32_t readA = lineA_.readFrac (centerSamp_ + depthSamp_ * lutSine32 (phaseA_));
    const int32_t readB = lineB_.readFrac (centerSamp_ + depthSamp_ * lutSine32 (phaseB_));

    // Per-line feedback loop (14-bit feedback coefficient, saturating add).
    const int32_t writeA = f24_addSat (lin, f24_mulk (readA, fb14_));
    const int32_t writeB = f24_addSat (lin, f24_mulk (readB, fb14_));
    lineA_.write (writeA);
    lineB_.write (writeB);

    // Advance the LFO: phaseA_ wraps to [0,1); phaseB_ trails it by 0.25.
    // (lutSine32 wraps its argument internally, so phaseB_ > 1 is fine.)
    phaseA_ += inc_;
    phaseA_ -= std::floor (phaseA_);
    phaseB_ = phaseA_ + 0.25f;

    // Stereo out: the two BBD lines panned hard L/R.
    lout = readA;
    rout = readB;
}

} // namespace parvati::fv1

// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Fv1LutDistortion implementation — 16 weird distortion wavetables + shared
// clock jitter + Tone. Fixed-point audio path; float only in the ctor (table
// build) and setParams.

#include "dsp/fx/fv1/Fv1LutDistortion.h"

#include <algorithm>
#include <cmath>

namespace parvati::fv1
{

// RAM budget: two 64-sample jitter rings = 128 words. Everything else is LUT.
static_assert (2 * DelayLine<64>::capacity <= kMaxMemorySamples,
               "Fv1LutDistortion within the FV-1 RAM budget");

namespace
{
// The 16 weird shapes, defined on x in [-4,4) and output-clamped to [-1,1].
float shapeFn (int s, float x)
{
    auto clamp1 = [] (float v) { return std::clamp (v, -1.0f, 1.0f); };
    switch (s)
    {
        case 0:  // Clip: hard knees at +-0.7, 1.6x hot
            return clamp1 (1.6f * x);
        case 1:  // Soft: x/(1+|x|), 1.5x out
            return clamp1 (1.5f * x / (1.0f + std::fabs (x)));
        case 2:  // Tube: asym soft (even harmonics)
            return clamp1 (x >= 0.0f ? 1.5f * x / (1.0f + 0.35f * x * x)
                                     : 1.7f * x / (1.0f + 0.22f * x * x));
        case 3:  // Wrap: ring-modulo wraparound (digital alias foldover)
        {
            float w = x * 0.5f;
            w = w - std::floor (w + 0.5f);        // wrap to [-0.5,0.5)
            return clamp1 (2.0f * w);
        }
        case 4:  // OctUp: full-wave rectified, offset down (octave fuzz)
            return clamp1 (1.5f * std::fabs (x) - 0.45f);
        case 5:  // Fuzz: gated soft-rect (sparse, buzzy)
            return clamp1 (x >= 0.12f ? 2.4f * (x - 0.12f) / (1.0f + x) : 0.15f * x);
        case 6:  // Square: near-comparator with soft edges
            return clamp1 (std::tanh (2.5f * x));
        case 7:  // Steps: 6-level staircase of the soft clip (gritty ladder)
        {
            const float soft = 1.5f * x / (1.0f + std::fabs (x));
            return clamp1 (std::round (soft * 3.0f) / 3.0f);
        }
        case 8:  // SFold: sinusoid over the wrapped input (sine folder)
        {
            float w = x * 0.5f;
            w = w - std::floor (w + 0.5f);
            return clamp1 (std::sin (w * 6.2831853f));
        }
        case 9:  // Cheby2: 2x^2-1 (octave-dominant)
            return clamp1 (0.95f * (2.0f * x * x - 1.0f) * std::exp (-std::fabs (x) * 0.35f));
        case 10: // Cheby3: 4x^3-3x (fifth-ish, hollow)
            return clamp1 (0.95f * (4.0f * x * x * x - 3.0f * x));
        case 11: // Asym: cubic around a -0.15 bias point (asym + warm)
        {
            const float t = x - 0.15f;
            return clamp1 (1.4f * (t - 0.4f * t * t * t));
        }
        case 12: // Mirror: mirrored tube (even harmonics, opposite tilt)
            return clamp1 (x >= 0.0f ? 1.7f * x / (1.0f + 0.22f * x * x)
                                     : 1.5f * x / (1.0f + 0.35f * x * x));
        case 13: // HGate: hot positive, crushed negative (gate-y)
            return clamp1 (x >= 0.0f ? 1.4f * x / (1.0f + 0.3f * x)
                                     : 0.3f * x / (1.0f + std::fabs (x)));
        case 14: // Crush4: 16-level staircase, loud (the "digital rasp")
            return clamp1 (1.35f * std::round (x * 8.0f) / 8.0f);
        case 15: // Sparse: sign*|x|^0.3 (expander curve — loud, gate-like)
            return clamp1 ((x < 0.0f ? -1.0f : 1.0f) * std::pow (std::fabs (x) + 1.0e-6f, 0.3f));
        default: return clamp1 (x);
    }
}
} // namespace

Fv1LutDistortion::Fv1LutDistortion()
{
    for (int s = 0; s < kShapes; ++s)
        for (int i = 0; i < kTableSize; ++i)
        {
            const float x = (static_cast<float> (i) - 512.0f) / 128.0f;   // [-4,4)
            const float y = shapeFn (s, x);
            tables_[s][static_cast<size_t> (i)] =
                static_cast<int16_t> (std::lround (y * 16383.0f * 0.75f));   // Q.14, hot
        }
}

void Fv1LutDistortion::setParams (const float param[5])
{
    const float p0 = std::clamp (param[0], 0.0f, 1.0f);
    const float p1 = std::clamp (param[1], 0.0f, 1.0f);
    const float p2 = std::clamp (param[2], 0.0f, 1.0f);
    const float p3 = std::clamp (param[3], 0.0f, 1.0f);
    // param[4] is UNUSED (Mix is the chain Dry/Wet — never read here).

    const float drive = 1.0f + p0 * 7.0f;                 // 1..8x
    driveShift_ = 0;
    while (drive / static_cast<float> (1 << (driveShift_ + 1)) >= 1.0f) ++driveShift_;
    drive14_ = q14 (drive / static_cast<float> (1 << driveShift_));

    int s = static_cast<int> (p1 * static_cast<float> (kShapes));
    if (s < 0) s = 0;
    if (s >= kShapes) s = kShapes - 1;                    // 16 stepped shapes
    // Click-free shape change: keep the previous table and crossfade old->new
    // (see the header). No fade on the very first setParams or an identical
    // shape — nothing to crossfade from/into.
    if (! shapeSet_)
        shapeSet_ = true;
    else if (tables_[s] != shape_)
    {
        fadeFrom_ = shape_;
        fade14_   = 0;
    }
    shape_ = tables_[s];

    jitAmt_  = p2 * 12.0f;                                // 0..12 samples
    toneLp_.setCutoff (700.0f * std::pow (15000.0f / 700.0f, p3));
}

void Fv1LutDistortion::prepareInternal (double sampleRate, int maxBlock)
{
    // 6x scratch for the bridge's worst-case internal count (same bound as
    // RateBridge::prepare: ceil(maxBlock * 32768/rate) + 4).
    const double rate = sampleRate > 0.0 ? sampleRate : 44100.0;
    const int worst = static_cast<int> (std::ceil (std::max (1, maxBlock)
                                    * (kInternalRate / rate))) + 4;
    const size_t n = static_cast<size_t> (worst * 6 + 8);
    osL_.assign (n, 0.0f);
    osR_.assign (n, 0.0f);
    srcUpL_.Init();   srcUpR_.Init();
    srcDownL_.Init(); srcDownR_.Init();
}

void Fv1LutDistortion::resetInternal()
{
    jitL_.clear();
    jitR_.clear();
    toneLp_.clear();
    jitLp_ = 0.0f;
    fadeFrom_ = nullptr;      // a cleared engine has no tail to crossfade
    fade14_   = 8191;
    srcUpL_.Init();   srcUpR_.Init();
    srcDownL_.Init(); srcDownR_.Init();
}

void Fv1LutDistortion::process (float* L, float* R, int numSamples)
{
    const int m = bridge().hostToInternal (L, R, numSamples);
    float* il = bridge().internalL();
    float* ir = bridge().internalR();

    // ---- 1x pre-pass: clock jitter only (input-side, in internal samples).
    // Runs BEFORE the 6x upsample (jitter AFTER the shaper would change the
    // documented signal path). The shape-crossfade clock is advanced INSIDE
    // the 6x loop below (once per internal sample, AFTER that sample's six
    // table evaluations) so the ~3.9 ms fade stays on the 1x timeline.
    for (int i = 0; i < m; ++i)
    {
        int32_t lin = f24_fromFloat (il[static_cast<size_t> (i)]);
        int32_t rin = f24_fromFloat (ir[static_cast<size_t> (i)]);

        // LCG -> one-pole-smoothed wobble; the read position wobbles around 1.
        // Jitter=0 reads at a fixed 1-sample delay (inaudible).
        lcg_ = lcg_ * 1103515245u + 12345u;
        const float n = static_cast<float> ((lcg_ >> 16) & 0xFFFFu) * (2.0f / 65535.0f) - 1.0f;
        jitLp_ += jitCoef_ * (n - jitLp_);
        const float readPos = 1.0f + jitLp_ * jitAmt_;
        jitL_.write (lin);
        jitR_.write (rin);
        const float rp = readPos < 1.0f ? 1.0f : readPos;
        il[static_cast<size_t> (i)] = f24_toFloat (jitL_.readFrac (rp));
        ir[static_cast<size_t> (i)] = f24_toFloat (jitR_.readFrac (rp));
    }

    // ---- 6x-oversampled table stage (see the file header). SRC_DOWN
    // requires a multiple-of-6 count: the up stage emits exactly 6*m. The
    // shape crossfade advances once per INTERNAL sample (1x clock) after
    // that sample's six table evaluations.
    if (m > 0)
    {
        srcUpL_.Process (il, osL_.data(), static_cast<size_t> (m));
        srcUpR_.Process (ir, osR_.data(), static_cast<size_t> (m));
        for (int i = 0; i < m; ++i)
        {
            for (int k = 0; k < 6; ++k)
            {
                const auto j = static_cast<size_t> (i * 6 + k);
                int32_t lo = 0, ro = 0;
                processSampleFx (f24_fromFloat (osL_[j]), f24_fromFloat (osR_[j]), lo, ro);
                osL_[j] = f24_toFloat (lo);
                osR_[j] = f24_toFloat (ro);
            }
            // Advance the shape crossfade once per INTERNAL sample (1x clock).
            if (fadeFrom_ != nullptr)
            {
                const int next = fade14_ + kFadeStep14;
                if (next >= 8191) { fade14_ = 8191; fadeFrom_ = nullptr; }   // fade done
                else                fade14_ = static_cast<int16_t> (next);
            }
        }
        srcDownL_.Process (osL_.data(), il, static_cast<size_t> (m * 6));
        srcDownR_.Process (osR_.data(), ir, static_cast<size_t> (m * 6));
    }

    // ---- Tone at 1x (linear stage does not alias). Mono effect: L -> L/R.
    for (int i = 0; i < m; ++i)
    {
        const int32_t y = toneLp_.process (f24_fromFloat (il[static_cast<size_t> (i)]));
        il[static_cast<size_t> (i)] = f24_toFloat (y);
        ir[static_cast<size_t> (i)] = f24_toFloat (y);
    }
    bridge().internalToHost (L, R, numSamples);
}

int32_t Fv1LutDistortion::lutShape (int32_t x)
{
    // Drive (2^shift stages + fractional) -> wavetable, blended with the
    // previous table across a shape change (equal-gain Q.14 crossfade).
    int32_t v = f24_mulk (x, drive14_);
    for (int s = 0; s < driveShift_; ++s)
        v = f24_addSat (v, v);
    int idx = (v >> 13) + 512;
    if (idx < 0)    idx = 0;
    if (idx > 1023) idx = 1023;
    const int32_t yn = static_cast<int32_t> (shape_[static_cast<size_t> (idx)])
                       * 512;   // Q.14 -> Q.23
    if (fadeFrom_ == nullptr)
        return yn;
    const int32_t yo = static_cast<int32_t> (fadeFrom_[static_cast<size_t> (idx)]) * 512;
    const int16_t f = fade14_;
    const int16_t g = static_cast<int16_t> (8191 - f);
    return f24_addSat (f24_mulk (yo, g), f24_mulk (yn, f));
}

void Fv1LutDistortion::processSampleFx (int32_t lin, int32_t rin,
                                        int32_t& lout, int32_t& rout)
{
    // PURE table shaper (6x rate): average of the two (jittered, at 1x)
    // channels through the curve — the mono-ization of the original path.
    const int32_t y = f24_sat (f24_addSat (lutShape (lin), lutShape (rin)) / 2);
    lout = y;
    rout = y;
}

} // namespace parvati::fv1

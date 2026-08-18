// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Fv1Overdrive implementation — Drive/Bias into an asymmetric soft-clip
// wavetable, Tone LP, Level trim. Pure 24-bit fixed-point audio path; only the
// table build (ctor) and setParams run in float.

#include "dsp/fx/fv1/Fv1Overdrive.h"

#include <algorithm>
#include <cmath>

namespace parvati::fv1
{

// No delay lines: the only state is the LUT + one-pole LP. RAM budget 0.
static_assert (0 <= kMaxMemorySamples, "Fv1Overdrive within the FV-1 RAM budget");

Fv1Overdrive::Fv1Overdrive()
{
    // 12AX7-ish asymmetric soft clip over x in [-4,4): positive half breaks up
    // earlier and rounds off (asymmetric head-room), negative half is stiffer.
    // Unity slope near 0 keeps low-Drive transparent.
    for (int i = 0; i < kTableSize; ++i)
    {
        const float x = (static_cast<float> (i) - 512.0f) / 128.0f;   // [-4,4)
        float y;
        if (x >= 0.0f)
        {
            const float a = 1.35f * x;
            y = a / (1.0f + 0.42f * a * a);          // softer, saturates ~1.6
            y = std::min (y, 1.35f);
        }
        else
        {
            const float a = -x;
            const float n = 1.55f * a;
            y = -(n / (1.0f + 0.30f * a * a));       // stiffer negative half
            y = std::max (y, -1.55f);
        }
        const float clamped = std::clamp (y, -1.0f, 1.0f);
        table_[static_cast<size_t> (i)] =
            static_cast<int16_t> (std::lround (clamped * 16383.0f * 0.72f));   // Q.14, ~0 dB at knee
    }
}

void Fv1Overdrive::setParams (const float param[5])
{
    const float p0 = std::clamp (param[0], 0.0f, 1.0f);
    const float p1 = std::clamp (param[1], 0.0f, 1.0f);
    const float p2 = std::clamp (param[2], 0.0f, 1.0f);
    const float p3 = std::clamp (param[3], 0.0f, 1.0f);
    // param[4] is UNUSED (Mix is the chain Dry/Wet — never read here).

    const float drive = std::pow (16.0f, p0);            // 1..16x
    // Split drive into integer 2x stages + a 14-bit fractional remainder
    // (q14 tops out at 1.99; 16x alone would overflow).
    driveShift_ = 0;
    while (drive / static_cast<float> (1 << (driveShift_ + 1)) >= 1.0f) ++driveShift_;
    drive14_ = q14 (drive / static_cast<float> (1 << driveShift_));

    biasIdx_  = static_cast<int> (std::lround ((p1 - 0.5f) * 0.6f * 256.0f));  // ±0.3 domain ≈ ±77 idx
    toneLp_.setCutoff (700.0f * std::pow (15000.0f / 700.0f, p2));
    level14_ = q14 (p3 * 2.0f);
}

void Fv1Overdrive::prepareInternal (double sampleRate, int maxBlock)
{
    // Size the 6x scratch for the bridge's worst-case internal count (the
    // same bound RateBridge::prepare uses: ceil(maxBlock * 32768/rate) + 4).
    const double rate = sampleRate > 0.0 ? sampleRate : 44100.0;
    const int worst = static_cast<int> (std::ceil (std::max (1, maxBlock)
                                    * (kInternalRate / rate))) + 4;
    const size_t n = static_cast<size_t> (worst * 6 + 8);
    osL_.assign (n, 0.0f);
    osR_.assign (n, 0.0f);
    srcUpL_.Init();   srcUpR_.Init();
    srcDownL_.Init(); srcDownR_.Init();
}

void Fv1Overdrive::resetInternal()
{
    toneLp_.clear();
    srcUpL_.Init();   srcUpR_.Init();
    srcDownL_.Init(); srcDownR_.Init();
}

void Fv1Overdrive::process (float* L, float* R, int numSamples)
{
    const int m = bridge().hostToInternal (L, R, numSamples);
    float* il = bridge().internalL();
    float* ir = bridge().internalR();

    // 6x-oversampled table stage (see the file header). SRC_DOWN requires a
    // multiple-of-6 input count: the up stage emits exactly 6*m.
    if (m > 0)
    {
        srcUpL_.Process (il, osL_.data(), static_cast<size_t> (m));
        srcUpR_.Process (ir, osR_.data(), static_cast<size_t> (m));
        const int m6 = m * 6;
        for (int i = 0; i < m6; ++i)
        {
            int32_t lo = 0, ro = 0;
            processSampleFx (f24_fromFloat (osL_[static_cast<size_t> (i)]),
                             f24_fromFloat (osR_[static_cast<size_t> (i)]),
                             lo, ro);
            osL_[static_cast<size_t> (i)] = f24_toFloat (lo);
            osR_[static_cast<size_t> (i)] = f24_toFloat (ro);
        }
        srcDownL_.Process (osL_.data(), il, static_cast<size_t> (m6));
        srcDownR_.Process (osR_.data(), ir, static_cast<size_t> (m6));
    }

    // Tone + Level at the 1x internal rate (linear stages do not alias).
    for (int i = 0; i < m; ++i)
    {
        int32_t lo = f24_fromFloat (il[static_cast<size_t> (i)]);
        lo = f24_mulk (toneLp_.process (lo), level14_);
        il[static_cast<size_t> (i)] = f24_toFloat (lo);
        ir[static_cast<size_t> (i)] = f24_toFloat (lo);   // mono effect: L -> L/R
    }
    bridge().internalToHost (L, R, numSamples);
}

void Fv1Overdrive::processSampleFx (int32_t lin, int32_t /*rin*/,
                                    int32_t& lout, int32_t& rout)
{
    // PURE shaper (no Tone/Level — they run at 1x in process()). Drive:
    // integer 2x stages (saturating adds) + 14-bit fractional gain, then the
    // LUT index over [-4,4): idx = (x>>13) + 512 + bias.
    int32_t v = f24_mulk (lin, drive14_);
    for (int s = 0; s < driveShift_; ++s)
        v = f24_addSat (v, v);                     // x * 2^shift (saturating)
    int idx = (v >> 13) + 512 + biasIdx_;
    if (idx < 0)   idx = 0;
    if (idx > 1023) idx = 1023;
    const int32_t y = static_cast<int32_t> (table_[static_cast<size_t> (idx)]) * 512;   // Q.14 -> Q.23
    lout = y;
    rout = y;
}

} // namespace parvati::fv1

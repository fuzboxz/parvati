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

namespace
{
// One-pole DC blocker pole (high-pass at ~10 Hz over the 32.768 kHz internal
// rate): y = x - x1 + a*y1. The table's asymmetric curve (positive/negative
// halves peak at different magnitudes) and the Bias read offset both put DC
// on the wet output; this removes it while leaving everything >= ~20 Hz.
constexpr float kDcPole = 1.0f - 6.28318530718f * 10.0f
                              / static_cast<float> (kInternalRate);
}

Fv1Overdrive::Fv1Overdrive()
{
    // 12AX7-ish asymmetric soft clip over x in [-4,4): positive half breaks up
    // earlier and rounds off, negative half is stiffer. Unity slope near 0
    // (0.72 * 1.35 = 0.97) keeps low-Drive transparent.
    // TRUE curve shape (do not "fix" without retuning):
    //   positive half 1.35x/(1+0.42a^2) peaks at ~0.772 (x ~= 1.14), then
    //     droops to ~0.408 at x=4 (read flat at the table edge);
    //   negative half peaks at ~-1.415 (x ~= -0.76) and is hard-clipped at
    //     -1 by the [-1,1] output clamp for steeper inputs.
    // The peak asymmetry is the even-harmonic character; the DC it creates is
    // removed by the output DC blocker (kDcPole).
    for (int i = 0; i < kTableSize; ++i)
    {
        const float x = (static_cast<float> (i) - 512.0f) / 128.0f;   // [-4,4)
        float y;
        if (x >= 0.0f)
        {
            const float a = 1.35f * x;
            y = a / (1.0f + 0.42f * a * a);          // softer positive half
        }
        else
        {
            const float a = -x;
            const float n = 1.55f * a;
            y = -(n / (1.0f + 0.30f * a * a));       // stiffer negative half
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
    // Split drive into integer 2x stages + a 14-bit fractional remainder.
    // The remainder is kept in [0.5,1) — q14() clamps any c >= 1.0 to unity
    // (the OLD [1,2) split was therefore pinned to 1.0x and the Drive knob
    // collapsed to a powers-of-two staircase: 1/2/4/8/16x only, every
    // intermediate position dead — caught by the subagent audit 2026-08-21).
    driveShift_ = 0;
    while (drive / static_cast<float> (1 << (driveShift_ + 1)) >= 0.5f) ++driveShift_;
    drive14_ = q14 (drive / static_cast<float> (1 << driveShift_));

    biasIdx_  = static_cast<int> (std::lround ((p1 - 0.5f) * 0.6f * 128.0f));  // ±0.3 domain = ±38 idx
    toneLp_.setCutoff (700.0f * std::pow (15000.0f / 700.0f, p2));
    // Level 0..2 via the ki/kf split (q14 alone tops out at ~1.0 and would
    // clamp the whole upper half of the knob to unity — see the header).
    float lvl = p3 * 2.0f;
    if (lvl < 0.0f) lvl = 0.0f;
    if (lvl > 2.0f) lvl = 2.0f;
    levelShift_ = (lvl >= 1.0f) ? 1 : 0;
    level14_    = q14 (lvl - static_cast<float> (levelShift_));
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
    // Report the 6x OS pair's group delay (8 internal samples) in HOST
    // samples so the chain's dry/wet blend stays comb-free (the Wavefolder /
    // RingModulator contract — both run their SRC at host rate and return the
    // raw 8; here the SRC lives INSIDE the 32.768 kHz domain, so the value
    // must be scaled). @48 kHz: lround(8 * 48000/32768) = 12.
    latencyHost_ = static_cast<int> (std::lround (
        static_cast<double> (srcUpL_.delay() + srcDownL_.delay() / 6)
        * rate / kInternalRate));
}

void Fv1Overdrive::resetInternal()
{
    toneLp_.clear();
    dcX1_ = 0.0f;
    dcY1_ = 0.0f;
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

    // Tone + Level at the 1x internal rate (linear stages do not alias), then
    // the DC blocker on the wet output (curve asymmetry / Bias DC removal).
    for (int i = 0; i < m; ++i)
    {
        int32_t lo = f24_fromFloat (il[static_cast<size_t> (i)]);
        const int32_t toned = toneLp_.process (lo);
        int32_t lv = f24_mulk (toned, level14_);
        if (levelShift_ != 0)
            lv = f24_addSat (lv, toned);         // the Level > 1 x2 stage
        const float x  = f24_toFloat (lv);
        const float y  = x - dcX1_ + kDcPole * dcY1_;   // ~10 Hz HP
        dcX1_ = x;
        dcY1_ = y;
        il[static_cast<size_t> (i)] = y;
        ir[static_cast<size_t> (i)] = y;   // mono effect: L -> L/R
    }
    bridge().internalToHost (L, R, numSamples);
}

void Fv1Overdrive::processSampleFx (int32_t lin, int32_t /*rin*/,
                                    int32_t& lout, int32_t& rout)
{
    // PURE shaper (no Tone/Level — they run at 1x in process()). Drive:
    // integer 2x stages (saturating adds) + 14-bit fractional gain, then the
    // LUT index over [-4,4). The Q.23 sample maps 1:1 onto the table domain:
    //     v >> 16 = 128 * x      (2^23 / 2^16 = 128 entries per unit)
    //     idx = 128 * (D*x + bias) + 512
    // so the curve is read at xT = D*x — the documented 1..16x Drive.
    // (The old >>13 read the table at 8*D*x: every documented gain and the
    // "low-Drive transparent" slope were 8x hot — see audit rev_dyn.md.)
    // UNSATURATED gain ladder (2026-08-21: re-applied — an earlier fix was
    // lost to a git checkout during red-validation; the subagent audit caught
    // it). f24_addSat doublings rail-clamped v at 2^23, collapsing the table
    // read domain to x_table in [-1,1]+bias for EVERY drive setting — the
    // outer 3/4 of the [-4,4) curve (the positive droop tail, the far
    // negative region) was unreachable. int64 ladder: |v| <= 16*2^23 = 2^27.
    int64_t v = f24_mulk (lin, drive14_);
    for (int s = 0; s < driveShift_; ++s)
        v += v;                                    // x * 2^shift (unsaturated)
    int idx = static_cast<int> ((v >> 16) + 512 + biasIdx_);
    if (idx < 0)   idx = 0;
    if (idx > 1023) idx = 1023;
    const int32_t y = static_cast<int32_t> (table_[static_cast<size_t> (idx)]) * 512;   // Q.14 -> Q.23
    lout = y;
    rout = y;
}

} // namespace parvati::fv1

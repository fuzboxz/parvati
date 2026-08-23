// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// HostRateBridge — bridges Parvati's host-rate de-interleaved (L[], R[]) audio
// to/from the vendored Clouds engines' FIXED 32 kHz interleaved (FloatFrame[])
// domain via linear resampling. Running the engines at a fixed 32 kHz leaves
// every Clouds/stmlib numeric constant (delay lengths, LFO rates, decay tuning)
// untouched, so the character is bit-faithful to upstream at any host rate.
//
// The internal FloatFrame scratch doubles as the (de)interleave boundary: there
// is no separate conversion step. All scratch is reserved in prepare(); process
// paths are allocation-free.
//
// Drift-free rate: hostToInternal() carries a PERSISTENT fractional read phase
// across blocks, so the per-block internal count `m` alternates (e.g. 171/170/171
// at 48 kHz) and the long-run average is exactly n*ratio -> the engine runs at
// EXACTLY 32000 Hz, with no systematic pitch offset. (A fixed per-block count
// would run the engine ~0.2% fast, ~+3.4 cents at 48 kHz.)

#pragma once

#include "clouds/dsp/frame.h"   // clouds::FloatFrame (vendored, SYSTEM include)
#include "stmlib/dsp/filter.h"  // stmlib::Svf (anti-alias / reconstruction LP)

#include <algorithm>
#include <cmath>
#include <vector>

class HostRateBridge
{
public:
    static constexpr double kInternalRate = 32000.0;

    // Reserve the worst-case internal scratch for a host block of @p maxBlock
    // samples at @p hostRate. Idempotent; safe on a rate / block-size change.
    void prepare (double hostRate, int maxBlock) noexcept
    {
        ratio_    = static_cast<float> (kInternalRate / hostRate);   // internal samples per host sample
        invRatio_ = static_cast<float> (hostRate / kInternalRate);   // host samples per internal sample
        // Worst-case internal samples produced from a host block of n is ceil(n*ratio)+headroom.
        const int worst = static_cast<int> (std::ceil (static_cast<float> (maxBlock) * ratio_)) + 2;
        scratch_.assign (static_cast<size_t> (std::max (4, worst)), clouds::FloatFrame {});
        maxM_           = worst;
        hostWritePhase_ = 0.0f;
        phaseStart_     = 0.0f;
        m_              = 0;
        hasTail_        = false;
        prevTail_       = clouds::FloatFrame {};

        // Anti-alias + reconstruction filtering: active only when host > 32 kHz
        // (the DOWNSAMPLE host->32k aliases content above 16 kHz; the UPSAMPLE
        // 32k->host creates images). At 32 k-or-lower host there is no aliasing.
        aaActive_ = hostRate > kInternalRate;
        if (aaActive_)
        {
            // Cutoff just below the 32 kHz Nyquist (16 kHz). Two cascaded Svf
            // (4th-order, ~24 dB/oct) attenuate the 16-Nyquist alias/image band
            // enough for the worst case (96 k = 3:1 downsample).
            const float freqNorm = static_cast<float> (14000.0 / hostRate);
            const float q        = 0.707f;   // Butterworth (no resonance peak)
            for (int i = 0; i < 2; ++i)
            {
                aaL_[i].Init();
                aaR_[i].Init();
                reconL_[i].Init();
                reconR_[i].Init();
                aaL_[i].set_f_q<stmlib::FREQUENCY_FAST> (freqNorm, q);
                aaR_[i].set_f_q<stmlib::FREQUENCY_FAST> (freqNorm, q);
                reconL_[i].set_f_q<stmlib::FREQUENCY_FAST> (freqNorm, q);
                reconR_[i].set_f_q<stmlib::FREQUENCY_FAST> (freqNorm, q);
            }
            const size_t sz = static_cast<size_t> (std::max (4, maxBlock));
            hostScratchL_.assign (sz, 0.0f);
            hostScratchR_.assign (sz, 0.0f);
        }
    }

    void reset() noexcept
    {
        for (auto& f : scratch_)
        {
            f.l = 0.0f;
            f.r = 0.0f;
        }
        hostWritePhase_ = 0.0f;
        phaseStart_     = 0.0f;
        m_              = 0;
        hasTail_        = false;
        prevTail_       = clouds::FloatFrame {};
        if (aaActive_)
        {
            for (int i = 0; i < 2; ++i)
            {
                aaL_[i].Reset();
                aaR_[i].Reset();
                reconL_[i].Reset();
                reconR_[i].Reset();
            }
        }
    }

    // The internal-rate interleaved scratch: filled by hostToInternal(), consumed
    // in place by the engine, then read back by internalToHost().
    clouds::FloatFrame* internal() noexcept { return scratch_.data(); }

    // Downsample + interleave host L/R into the internal FloatFrame scratch.
    // Returns the number of internal samples produced this block (m); a
    // persistent fractional phase makes m alternate so the long-run engine rate
    // is EXACTLY 32000 Hz (drift-free). Pass the returned m to the engine.
    int hostToInternal (const float* L, const float* R, int n) noexcept
    {
        const float span = static_cast<float> (n);
        const int   last = n - 1;
        // Record the START phase of this call (the carried remainder). The
        // up-sampler (internalToHost) needs it to place its read index so the
        // resampling is seamless across sub-chunk / block boundaries (see below).
        phaseStart_     = hostWritePhase_;

        // Anti-alias pre-filter: when host > 32 kHz, lowpass the host input
        // BEFORE decimation so content above 16 kHz (the 32 kHz Nyquist) does
        // not alias into 0-16 kHz. Two cascaded Svf (24 dB/oct) at 14 kHz.
        const float* srcL = L;
        const float* srcR = R;
        if (aaActive_)
        {
            for (int i = 0; i < n; ++i)
            {
                float l = aaL_[0].Process<stmlib::FILTER_MODE_LOW_PASS> (L[i]);
                hostScratchL_[static_cast<size_t> (i)] =
                    aaL_[1].Process<stmlib::FILTER_MODE_LOW_PASS> (l);
                float r = aaR_[0].Process<stmlib::FILTER_MODE_LOW_PASS> (R[i]);
                hostScratchR_[static_cast<size_t> (i)] =
                    aaR_[1].Process<stmlib::FILTER_MODE_LOW_PASS> (r);
            }
            srcL = hostScratchL_.data();
            srcR = hostScratchR_.data();
        }

        float       phase = hostWritePhase_;
        int         m = 0;
        while (phase < span && m < maxM_)
        {
            // phase is in [0, span) here, so the integer index never exceeds last.
            const int   i0   = static_cast<int> (phase);
            const float frac = phase - static_cast<float> (i0);
            const int   i1   = i0 < last ? i0 + 1 : i0;
            const float la   = srcL[i0], lb = srcL[i1];
            const float ra   = srcR[i0], rb = srcR[i1];
            scratch_[static_cast<size_t> (m)].l = la + (lb - la) * frac;
            scratch_[static_cast<size_t> (m)].r = ra + (rb - ra) * frac;
            phase += invRatio_;
            ++m;
        }
        hostWritePhase_ = phase - span;   // carry the fractional remainder into the next block
        m_              = m;
        return m;
    }

    // Upsample + de-interleave the (engine-processed) internal scratch to host L/R.
    // The read index for host sample i is vj = (i - phaseStart_) * ratio_, which
    // places output sample i at exactly the internal sample for host time i —
    // NOT at i*ratio_ (which would ignore phaseStart_ and apply a per-call
    // fractional delay equal to phaseStart_, alternating across sub-chunks and
    // clicking at every boundary). Because phaseStart_ is in [0, invRatio_), the
    // leading samples (i < phaseStart_) have vj in [-1, 0): they are produced by
    // blending the PREVIOUS call's last processed sample (prevTail_, at vj = -1)
    // with this call's first processed sample (scratch_[0], at vj = 0). This
    // 1-sample head overlap makes the resampling seamless across sub-chunk AND
    // host-block boundaries — eliminating the rhythmic click heard when a Clouds
    // FX (Diffuser/Reverb/PitchShifter/modes) is processed in 980 Hz sub-chunks.
    // Drift correction (hostWritePhase_ carry) is unchanged, so the engine still
    // runs at exactly 32000 Hz.
    void internalToHost (float* L, float* R, int n) noexcept
    {
        const int m = m_;
        if (m <= 0)
        {
            // No internal sample was produced this call (can happen when the
            // caller passes a 1-sample block whose phase carry did not cross an
            // internal boundary -- e.g. renderPartFx's drift-corrected sub-
            // chunking emits a 1-sample sub-chunk). Zeroing here would be a
            // full-amplitude dropout (the bug); instead HOLD the last processed
            // internal sample (zero-order hold) so the output is continuous.
            const float hl = hasTail_ ? prevTail_.l : 0.0f;
            const float hr = hasTail_ ? prevTail_.r : 0.0f;
            for (int i = 0; i < n; ++i) { L[i] = hl; R[i] = hr; }
            return;
        }
        const int lastM = m - 1;
        for (int i = 0; i < n; ++i)
        {
            const float vj = (static_cast<float> (i) - phaseStart_) * ratio_;
            if (vj < 0.0f)
            {
                // Leading overlap sample: blend prevTail_ (vj = -1) and
                // scratch_[0] (vj = 0). frac = vj + 1 is in (0, 1].
                const float frac = vj + 1.0f;
                const auto& a = hasTail_ ? prevTail_ : scratch_[0];
                const auto& b = scratch_[0];
                L[i] = a.l + (b.l - a.l) * frac;
                R[i] = a.r + (b.r - a.r) * frac;
            }
            else
            {
                float pos = vj;
                if (pos > static_cast<float> (lastM))
                    pos = static_cast<float> (lastM);
                const int   j0   = static_cast<int> (pos);
                const float frac = pos - static_cast<float> (j0);
                const int   j1   = j0 < lastM ? j0 + 1 : j0;
                const auto& a    = scratch_[static_cast<size_t> (j0)];
                const auto& b    = scratch_[static_cast<size_t> (j1)];
                L[i] = a.l + (b.l - a.l) * frac;
                R[i] = a.r + (b.r - a.r) * frac;
            }
        }
        // Capture the last processed sample as the head-overlap tail for the
        // next call (internal[] holds the engine-processed output here).
        prevTail_ = scratch_[static_cast<size_t> (lastM)];
        hasTail_  = true;

        // Reconstruction post-filter: when host > 32 kHz, lowpass the upsampled
        // host output to remove images above 16 kHz created by the linear
        // interpolation upsample. Same 24 dB/oct LP as the anti-alias stage.
        if (aaActive_)
        {
            for (int i = 0; i < n; ++i)
            {
                float l = reconL_[0].Process<stmlib::FILTER_MODE_LOW_PASS> (L[i]);
                L[i] = reconL_[1].Process<stmlib::FILTER_MODE_LOW_PASS> (l);
                float r = reconR_[0].Process<stmlib::FILTER_MODE_LOW_PASS> (R[i]);
                R[i] = reconR_[1].Process<stmlib::FILTER_MODE_LOW_PASS> (r);
            }
        }
    }

private:
    std::vector<clouds::FloatFrame> scratch_;
    float ratio_          = 0.6666667f;   // 32000 / hostRate
    float invRatio_       = 1.5f;         // hostRate / 32000
    float hostWritePhase_ = 0.0f;         // persistent fractional host index (carried across blocks)
    float phaseStart_     = 0.0f;         // START phase of the last hostToInternal() (= hostWritePhase_ before it advanced)
    int   maxM_           = 0;            // scratch capacity (worst-case m)
    int   m_              = 0;            // internal samples produced by the last hostToInternal()
    clouds::FloatFrame prevTail_ {};      // last PROCESSED internal sample from the previous call (head-overlap)
    bool  hasTail_        = false;        // prevTail_ is valid

    // ---- Anti-alias + reconstruction filtering (active when host > 32 kHz) ----
    bool  aaActive_       = false;        // gate: true when hostRate > kInternalRate
    stmlib::Svf aaL_[2] {};               // pre-decimation AA LP, left (2-stage cascade)
    stmlib::Svf aaR_[2] {};               // pre-decimation AA LP, right
    stmlib::Svf reconL_[2] {};            // post-interpolation reconstruction LP, left
    stmlib::Svf reconR_[2] {};            // post-interpolation reconstruction LP, right
    std::vector<float> hostScratchL_ {};  // host-rate filtered input (AA pre-filter dest)
    std::vector<float> hostScratchR_ {};  // host-rate filtered input (right)
};

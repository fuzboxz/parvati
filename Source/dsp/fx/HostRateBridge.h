// Copyright (c) 2026 805Labs Kft. / Hellcat.
//
// HostRateBridge — bridges Hellcat's host-rate de-interleaved (L[], R[]) audio
// to/from the vendored Clouds engines' FIXED 32 kHz interleaved (FloatFrame[])
// domain. Running the engines at a fixed 32 kHz leaves every Clouds/stmlib
// numeric constant (delay lengths, LFO rates, decay tuning) untouched, so the
// character is bit-faithful to upstream at any host rate.
//
// The resampler transport (phase carry, drift-free internal rate, head
// overlap, empty-block hold) lives in LinearResamplerCore. This class owns
// the rate constant, the interleaved FloatFrame scratch and the anti-alias /
// reconstruction filters.
//
// The internal FloatFrame scratch doubles as the (de)interleave boundary: there
// is no separate conversion step. All scratch is reserved in prepare(); process
// paths are allocation-free.

#pragma once

#include "dsp/fx/LinearResamplerCore.h"   // shared resampler transport (CRTP base)

#include "clouds/dsp/frame.h"   // clouds::FloatFrame (vendored, SYSTEM include)
#include "stmlib/dsp/filter.h"  // stmlib::Svf (anti-alias / reconstruction LP)

#include <algorithm>
#include <cmath>
#include <vector>

class HostRateBridge : public hellcat::dsp::LinearResamplerCore<HostRateBridge>
{
public:
    static constexpr double kInternalRate = 32000.0;

    // Reserve the worst-case internal scratch for a host block of @p maxBlock
    // samples at @p hostRate. Idempotent; safe on a rate / block-size change.
    void prepare (double hostRate, int maxBlock) noexcept
    {
        const float ratio    = static_cast<float> (kInternalRate / hostRate);   // internal samples per host sample
        const float invRatio = static_cast<float> (hostRate / kInternalRate);   // host samples per internal sample
        // Worst-case internal samples produced from a host block of n is ceil(n*ratio)+headroom.
        const int worst = static_cast<int> (std::ceil (static_cast<float> (maxBlock) * ratio)) + 2;
        scratch_.assign (static_cast<size_t> (std::max (4, worst)), clouds::FloatFrame {});
        configure (ratio, invRatio, worst);

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
        LinearResamplerCore<HostRateBridge>::reset();
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
    // Returns the number of internal samples produced this block (m); the
    // shared core carries the fractional phase, so the long-run engine rate is
    // EXACTLY 32000 Hz (drift-free). Pass the returned m to the engine.
    int hostToInternal (const float* L, const float* R, int n) noexcept
    {
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
        return downsample (srcL, srcR, n);
    }

    // Upsample + de-interleave the (engine-processed) internal scratch to host
    // L/R, then lowpass the upsampled output when host > 32 kHz (reconstruction
    // of the images the linear interpolation creates). The shared core places
    // the read index and blends the head overlap, so sub-chunk boundaries stay
    // click-free; the engine still runs at exactly 32000 Hz.
    void internalToHost (float* L, float* R, int n) noexcept
    {
        upsample (L, R, n);
        // Reconstruction post-filter runs after a produced block only. The
        // held-sample path (no internal sample) returns unfiltered.
        if (count() > 0 && aaActive_)
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
    friend hellcat::dsp::LinearResamplerCore<HostRateBridge>;

    // ---- Internal-domain storage hooks of the shared resampler core ----
    void storeSample (int m, float l, float r) noexcept
    {
        scratch_[static_cast<size_t> (m)].l = l;
        scratch_[static_cast<size_t> (m)].r = r;
    }
    float loadL (int j) const noexcept { return scratch_[static_cast<size_t> (j)].l; }
    float loadR (int j) const noexcept { return scratch_[static_cast<size_t> (j)].r; }

    std::vector<clouds::FloatFrame> scratch_;

    // ---- Anti-alias + reconstruction filtering (active when host > 32 kHz) ----
    bool  aaActive_       = false;        // gate: true when hostRate > kInternalRate
    stmlib::Svf aaL_[2] {};               // pre-decimation AA LP, left (2-stage cascade)
    stmlib::Svf aaR_[2] {};               // pre-decimation AA LP, right
    stmlib::Svf reconL_[2] {};            // post-interpolation reconstruction LP, left
    stmlib::Svf reconR_[2] {};            // post-interpolation reconstruction LP, right
    std::vector<float> hostScratchL_ {};  // host-rate filtered input (AA pre-filter dest)
    std::vector<float> hostScratchR_ {};  // host-rate filtered input (right)
};

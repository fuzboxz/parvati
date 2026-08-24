// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// LinearResamplerCore — the shared linear host<->internal resampler of the
// two FX rate bridges. HostRateBridge (32 kHz, interleaved frames) and
// fv1::RateBridge (32.768 kHz, split floats) inherit it. The core owns the
// transport state: ratios, carried write phase, read phase, sample count,
// head-overlap tail. Each bridge keeps its own rate constant, its filters
// and its scratch sizing.
//
// The transport stays drift-free. downsample() carries a persistent
// fractional host read phase across blocks. The per-block internal count m
// alternates, so the long-run average equals n*ratio exactly. upsample()
// places its read index against the phase start of the matching downsample()
// call. A one-sample head overlap blends the previous tail. Sub-chunk and
// block boundaries therefore stay seamless.
//
// A block that produces no internal sample holds the last sample instead.
// That zero-order hold prevents a full-amplitude dropout.
//
// The core is DELIBERATELY include-free. Fv1Engine.h pulls it in without
// gaining a single dependency. The float expressions below come from the
// two bridges unchanged. Any edit changes render bytes: fx_render_golden_test
// pins them. A change to these loops needs an approved digest update.

#ifndef PARVATI_DSP_FX_LINEAR_RESAMPLER_CORE_H
#define PARVATI_DSP_FX_LINEAR_RESAMPLER_CORE_H

namespace parvati::dsp
{

// CRTP base. The derived bridge supplies the internal-domain storage hooks:
//   void storeSample (int m, float l, float r) noexcept;
//   float loadL (int j) const noexcept;
//   float loadR (int j) const noexcept;
// The hooks compile to a plain store or load after inlining.
template <typename Derived>
class LinearResamplerCore
{
public:
    // Set the rates and the scratch capacity. Resets the transport state.
    // Call from the derived prepare(). @p ratio is internal samples per host
    // sample; @p invRatio is the reciprocal; @p maxM caps the produced count.
    void configure (float ratio, float invRatio, int maxM) noexcept
    {
        ratio_         = ratio;
        invRatio_      = invRatio;
        maxM_          = maxM;
        hostWritePhase_ = 0.0f;
        phaseStart_     = 0.0f;
        m_              = 0;
        hasTail_        = false;
        tailL_          = 0.0f;
        tailR_          = 0.0f;
    }

    // Clear the transport state only. The derived reset() clears its own
    // buffers and filters, then calls this.
    void reset() noexcept
    {
        hostWritePhase_ = 0.0f;
        phaseStart_     = 0.0f;
        m_              = 0;
        hasTail_        = false;
        tailL_          = 0.0f;
        tailR_          = 0.0f;
    }

    // Internal samples produced by the last downsample() call.
    int count() const noexcept { return m_; }

    // Linear downsample of n host samples into the internal scratch.
    // srcL/srcR hold HOST-rate input; the derived bridge filters them first.
    // Returns the internal sample count m of this call.
    int downsample (const float* srcL, const float* srcR, int n) noexcept
    {
        // Record the START phase of this call (the carried remainder). The
        // up-sampler needs it to place its read index across boundaries.
        phaseStart_     = hostWritePhase_;
        const float span = static_cast<float> (n);
        const int   last = n - 1;
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
            derived().storeSample (m, la + (lb - la) * frac, ra + (rb - ra) * frac);
            phase += invRatio_;
            ++m;
        }
        hostWritePhase_ = phase - span;   // carry the fractional remainder into the next block
        m_              = m;
        return m;
    }

    // Linear upsample of the internal scratch to n host samples. The derived
    // bridge filters the output afterwards. Read index for host sample i:
    // vj = (i - phaseStart_) * ratio_. Sample i lands at the internal sample
    // of host time i, not at i*ratio_. The leading samples (i < phaseStart_)
    // blend the previous tail with this call's first sample.
    void upsample (float* L, float* R, int n) noexcept
    {
        const int m = m_;
        if (m <= 0)
        {
            // No internal sample was produced this call. HOLD the last
            // processed internal sample (zero-order hold). Zeroing would be
            // a full-amplitude dropout.
            const float hl = hasTail_ ? tailL_ : 0.0f;
            const float hr = hasTail_ ? tailR_ : 0.0f;
            for (int i = 0; i < n; ++i) { L[i] = hl; R[i] = hr; }
            return;
        }
        const int lastM = m - 1;
        for (int i = 0; i < n; ++i)
        {
            const float vj = (static_cast<float> (i) - phaseStart_) * ratio_;
            if (vj < 0.0f)
            {
                // Leading overlap sample: blend the tail (vj = -1) and the
                // first internal sample (vj = 0). frac = vj + 1 is in (0, 1].
                const float frac = vj + 1.0f;
                const float al   = hasTail_ ? tailL_ : derived().loadL (0);
                const float bl   = derived().loadL (0);
                L[i] = al + (bl - al) * frac;
                const float ar   = hasTail_ ? tailR_ : derived().loadR (0);
                const float br   = derived().loadR (0);
                R[i] = ar + (br - ar) * frac;
            }
            else
            {
                float pos = vj;
                if (pos > static_cast<float> (lastM))
                    pos = static_cast<float> (lastM);
                const int   j0   = static_cast<int> (pos);
                const float frac = pos - static_cast<float> (j0);
                const int   j1   = j0 < lastM ? j0 + 1 : j0;
                const float al   = derived().loadL (j0), bl = derived().loadL (j1);
                L[i] = al + (bl - al) * frac;
                const float ar   = derived().loadR (j0), br = derived().loadR (j1);
                R[i] = ar + (br - ar) * frac;
            }
        }
        // Keep the last processed internal sample as the next head-overlap
        // tail. The derived internal scratch holds processed output here.
        tailL_   = derived().loadL (lastM);
        tailR_   = derived().loadR (lastM);
        hasTail_ = true;
    }

protected:
    ~LinearResamplerCore() = default;   // CRTP base; delete through the derived type only

    Derived&       derived()       noexcept { return static_cast<Derived&> (*this); }
    const Derived& derived() const noexcept { return static_cast<const Derived&> (*this); }

    float ratio_          = 1.0f;   // internal samples per host sample
    float invRatio_       = 1.0f;   // host samples per internal sample
    float hostWritePhase_ = 0.0f;   // persistent fractional host index (carried across blocks)
    float phaseStart_     = 0.0f;   // START phase of the last downsample() call
    int   maxM_           = 0;      // scratch capacity (worst-case m)
    int   m_              = 0;      // internal samples produced by the last downsample()
    float tailL_ = 0.0f, tailR_ = 0.0f;   // last PROCESSED internal sample (head overlap)
    bool  hasTail_        = false;  // tail holds a valid sample
};

} // namespace parvati::dsp

#endif // PARVATI_DSP_FX_LINEAR_RESAMPLER_CORE_H

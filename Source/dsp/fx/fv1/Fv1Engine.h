// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Fv1Engine — the shared Spin FV-1 hardware-emulation framework for the
// Clocked Delay / Ensemble / Plate Reverb / Vinyl Compressor / Phaser FX family.
// Header-only and DELIBERATELY JUCE-FREE (std headers plus the include-free
// dsp/fx/LinearResamplerCore.h shard) so every FV-1 effect and its unit test
// compiles standalone in seconds WITHOUT the JUCE build:
// `clang++ -std=c++17 -I Source test.cpp fx.cpp`.
//
// It implements the FV-1 hardware constraint set the family obeys:
//  * 24-bit fixed-point (Q.23 signed) audio path with saturation clipping.
//  * 14-bit coefficient / control-parameter quantization.
//  * 32.768 kHz internal rate with basic linear host<->internal resampling and
//    NO modern anti-alias filter (only a steep simple-IIR BW-limit at 15 kHz).
//  * <= 32,768 samples of total delay memory per effect (kMaxMemorySamples).
//
// This is NOT a port of the FV-1 instruction set; it is a faithful-enough
// emulation of the chip's *signal characteristics* (fixed-point saturation,
// coefficient quantization, band-limited I/O, the ~1 s RAM budget) sized to the
// Parvati per-part FX slot contract (one FxProcessor per slot).

#ifndef PARVATI_DSP_FX_FV1_FV1ENGINE_H
#define PARVATI_DSP_FX_FV1_FV1ENGINE_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "dsp/fx/LinearResamplerCore.h"   // shared resampler transport (CRTP base, include-free)

namespace parvati::fv1
{

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// The FV-1 internal sample rate this whole family runs at.
constexpr double kInternalRate = 32768.0;

// Total delay RAM budget per effect (1.0 s at 32.768 kHz = 2^15 samples).
// Effects static_assert their summed buffer capacities stay under-or-equal this.
// (The largest single delay VALUE any effect may use is kMaxDelaySamples, one
// less than a power-of-two ring capacity, to avoid the read==write degeneracy.)
constexpr int kMaxMemorySamples = 32768;
constexpr int kMaxDelaySamples  = 32767;

// ---------------------------------------------------------------------------
// 24-bit fixed-point audio path (Q.23 signed)
// ---------------------------------------------------------------------------
//
// Representation: an audio sample x in [-1.0, +1.0) is stored as the int32_t
//   x * 2^23      (clamped to the 24-bit signed range [-2^23, 2^23 - 1]).
// The 24-bit *data* lives in an int32_t (the top 8 bits are integer headroom /
// sign extension). Saturation is explicit on every add/mul that can overflow so
// the path clips like the FV-1's saturating accumulator rather than wrapping.

constexpr int32_t kOneQ23 = 1 << 23;          // +1.0 (clip target high+1)
constexpr int32_t kMaxQ23 = (1 << 23) - 1;    // max representable (+0.99999988)
constexpr int32_t kMinQ23 = -(1 << 23);       // min representable (-1.0)

// float [-1,1) -> Q.23 (saturating). Rounded to nearest.
inline int32_t f24_fromFloat (float x) noexcept
{
    if (x >= 1.0f)  return kMaxQ23;
    if (x <= -1.0f) return kMinQ23;
    // Round to nearest, ties away from zero (cheap, deterministic).
    return static_cast<int32_t> (std::lround (x * static_cast<float> (kOneQ23)));
}

// Q.23 -> float [-1,1).
inline float f24_toFloat (int32_t x) noexcept
{
    return static_cast<float> (x) * (1.0f / static_cast<float> (kOneQ23));
}

// Saturate a raw int32 to the 24-bit signed range (the FV-1 clip).
inline int32_t f24_sat (int32_t x) noexcept
{
    if (x > kMaxQ23) return kMaxQ23;
    if (x < kMinQ23) return kMinQ23;
    return x;
}

// Saturating add (FV-1 ADD with saturation).
inline int32_t f24_addSat (int32_t a, int32_t b) noexcept
{
    return f24_sat (static_cast<int64_t> (a) + b > kMaxQ23    ? kMaxQ23
                    : static_cast<int64_t> (a) + b < kMinQ23 ? kMinQ23
                                                             : a + b);
}

// Q.23 * Q.23 -> Q.23 saturating (FV-1 multiplication; 64-bit intermediate,
// scale back by 2^23). Used for sample*sample products.
inline int32_t f24_mul (int32_t a, int32_t b) noexcept
{
    return f24_sat (static_cast<int32_t> (
        (static_cast<int64_t> (a) * static_cast<int64_t> (b)) >> 23));
}

// Q.23 * (14-bit coefficient) -> Q.23 saturating. The coefficient is the raw
// int16 from q14() below (sign + 13 fractional bits). Scale back by 2^13. This
// is the path that quantizes filter coefficients / feedback to 14-bit exactly
// like the FV-1 coefficient register.
inline int32_t f24_mulk (int32_t a, int16_t k14) noexcept
{
    return f24_sat (static_cast<int32_t> (
        (static_cast<int64_t> (a) * static_cast<int64_t> (k14)) >> 13));
}

// Quantize a float coefficient in [-1,1) to a signed 14-bit value
// (sign + 13 fractional bits; range [-8192, 8191]). Feed the result to f24_mulk.
inline int16_t q14 (float c) noexcept
{
    if (c >= 1.0f)  return 8191;
    if (c <= -1.0f) return -8192;
    return static_cast<int16_t> (std::lround (c * 8191.0f));
}

// Bit-truncation "grit": reduce a Q.23 sample to an effective `bits`-bit
// resolution (bits in [1,24]) by zeroing the low (24-bits) fractional bits.
// E.g. bits=8 zeros 16 bits (keeps sign + top 7) -> coarse 8-bit quantization.
// This is a pure mask (no dither) — the FV-1 "AND-MASK" grit.
inline int32_t f24_quantBits (int32_t x, int bits) noexcept
{
    if (bits >= 24) return x;
    if (bits < 1)   bits = 1;
    const int zeros = 24 - bits;
    const uint32_t mask = (~static_cast<uint32_t> (0)) << zeros;   // unsigned shift is defined
    return x & static_cast<int32_t> (mask);
}

// ---------------------------------------------------------------------------
// Fixed-point filters (1-pole LP + 1st-order allpass). Coefficients quantized
// to 14-bit via q14() exactly as the FV-1 stores them.
// ---------------------------------------------------------------------------

// One-pole low-pass: y[n] = (1-a) y[n-1] + a x[n]. a in [0,1] is the pole
// position (a ~ 1 - exp(-2*pi*fc/fs)). Both a and (1-a) are kept as 14-bit.
struct OnePoleLpFx
{
    int32_t y  = 0;
    int16_t a14    = 0;   // q14(a)
    int16_t ainv14 = 4096; // q14(1-a)  (4096 = q14(0.5) default)

    void clear() noexcept { y = 0; }
    // Set the pole from a float coefficient a in [0,1] (recompute 1-a too).
    void setA (float a) noexcept
    {
        if (a < 0.0f) a = 0.0f;
        if (a > 1.0f) a = 1.0f;
        a14    = q14 (a);
        ainv14 = q14 (1.0f - a);
    }
    // Convenience: set from a desired cutoff fc at the internal rate.
    void setCutoff (float fc) noexcept
    {
        const float a = 1.0f - std::exp (-6.28318530718f * fc / static_cast<float> (kInternalRate));
        setA (a);
    }
    int32_t process (int32_t x) noexcept
    {
        // y = a*x + (1-a)*y   (both muls saturating, 14-bit coeffs)
        y = f24_addSat (f24_mulk (x, a14), f24_mulk (y, ainv14));
        return y;
    }
};

// 1st-order allpass used by the phaser (and as a cheap diffusion element):
//   y[n] = c*x[n] + x[n-1] - c*y[n-1]
// implemented in the FV-1 SOF/1st-order-AP form with a 14-bit coefficient.
// A single shared coefficient (set once per sample) is applied across all six
// phaser stages to respect the 128-instruction budget.
struct Allpass1Fx
{
    int32_t x1 = 0;   // x[n-1]
    int32_t y1 = 0;   // y[n-1]
    int16_t c14 = 0;  // q14(c), c in (-1,1)

    void clear() noexcept { x1 = y1 = 0; }
    void setCoef (float c) noexcept
    {
        if (c < -0.999f) c = -0.999f;
        if (c > 0.999f)  c = 0.999f;
        c14 = q14 (c);
    }
    int32_t process (int32_t x) noexcept
    {
        // y = c*x + x1 - c*y1 ; saturating.
        int32_t y = f24_addSat (f24_mulk (x, c14), x1);
        y = f24_addSat (y, -f24_mulk (y1, c14));
        x1 = x;
        y1 = y;
        return y;
    }
};

// ---------------------------------------------------------------------------
// LOOP DC KILLER (2026-08-21): a one-pole ~10 Hz high-pass at the internal
// rate, placed in a feedback loop's return path. Any near-unity regen loop
// (delay/echo/combs) is a DC integrator with DC gain 1/(1-g) — up to 200x
// (echo fb 0.995) or ~1000x (plate decay 0.999). Residual input DC or
// saturation asymmetry accumulates until the loop parks near a rail; the
// DC-heavy output then makes any following shaper pin constant (its own DC
// blocker strips it -> gated silence: the delay->reverb->shaper "complete
// voice dropout" chain, measured dc -0.22..-0.28 at the shaper input). The
// killer drops loop DC gain to ~0 while leaving audio-band regen untouched.
struct LoopDcKiller
{
    float x1 = 0.0f, y1 = 0.0f;
    void clear() noexcept { x1 = y1 = 0.0f; }
    int32_t process (int32_t x) noexcept
    {
        constexpr float kPole = 1.0f - 6.28318530718f * 10.0f
                                    / static_cast<float> (kInternalRate);
        const float xf = f24_toFloat (x);
        const float y  = xf - x1 + kPole * y1;
        x1 = xf;
        y1 = y;
        return f24_fromFloat (y);
    }
};

// Q.16 read-pointer glide (the tap-glide idiom the Echo / Flanger /
// ClockedDelay base delays share): one-pole k = 1/256 per internal sample,
// capped at ~0.25 sample/sample (a tape-speed pitch bend, click-free), with
// the sub-1/16-sample tail snapped and a +/-1 Q.16 minimum step so the glide
// can never stall below one quantum.
inline void glideTapQ16 (int32_t& cur, int32_t target) noexcept
{
    constexpr int32_t kQOne      = 65536;   // 1.0 sample in Q.16
    constexpr int32_t kGlideCapQ = 16384;   // ~0.25 sample per internal sample
    constexpr int     kGlideShift = 8;      // one-pole k = 1/256 per sample
    const int32_t deltaQ = target - cur;
    const int32_t distQ  = (deltaQ < 0) ? -deltaQ : deltaQ;
    if (distQ <= kQOne / 16)
    {
        cur = target;   // settle the inaudible tail instantly
        return;
    }
    int32_t stepQ = deltaQ >> kGlideShift;
    if (stepQ >  kGlideCapQ) stepQ =  kGlideCapQ;
    if (stepQ < -kGlideCapQ) stepQ = -kGlideCapQ;
    if (stepQ == 0) stepQ = (deltaQ > 0) ? 1 : -1;   // never stall below 1 Q16
    cur += stepQ;
}

// Soft-saturation knee (the Flanger "crackle" fix, 2026-08-21): transparent
// up to +/-0.6, then a tanh ease into the rail. The Flanger / Ensemble /
// Phaser feedback writes share it.
inline float softKneeTanh (float f) noexcept
{
    if (f > 0.6f)  return  0.6f + 0.4f * std::tanh (( f - 0.6f) * 2.5f);
    if (f < -0.6f) return -0.6f - 0.4f * std::tanh ((-f - 0.6f) * 2.5f);
    return f;
}

// ---------------------------------------------------------------------------
// Fixed-point delay line (power-of-two capacity). Integer + fractional reads.
// ---------------------------------------------------------------------------

template <int N>
class DelayLine
{
    static_assert (N > 0 && (N & (N - 1)) == 0, "DelayLine capacity must be a power of two");
    std::array<int32_t, N> buf_ {};
    int wpos_ = 0;

public:
    static constexpr int capacity = N;
    void clear() noexcept { buf_.fill (0); wpos_ = 0; }

    void write (int32_t x) noexcept { buf_[static_cast<size_t> (wpos_)] = x; wpos_ = (wpos_ + 1) & (N - 1); }

    // Integer read at delay d (1..N). d==1 is the most recent write.
    int32_t read (int d) const noexcept
    {
        if (d < 1) d = 1;
        return buf_[(wpos_ - d) & (N - 1)];
    }

    // Fractional read with linear interpolation (for modulated read pointers).
    // d >= 1.0. Keeps the fixed-point path (14-bit interp coefficients).
    int32_t readFrac (float d) const noexcept
    {
        if (d < 1.0f) d = 1.0f;
        const int di   = static_cast<int> (std::floor (d));
        const float fr = d - static_cast<float> (di);   // [0,1)
        const int32_t a = read (di);
        const int32_t b = read (di + 1);
        // a*(1-fr) + b*fr  via 14-bit interp coefficients.
        return f24_addSat (f24_mulk (a, q14 (1.0f - fr)), f24_mulk (b, q14 (fr)));
    }
};

// ---------------------------------------------------------------------------
// 32-value sine + triangle lookup tables with linear interpolation.
// FV-1 LFOs are table-driven; NO per-sample trig. Tables have a 33rd wrap entry
// so linear interp never indexes out of range. Thread-safe lazy init (function-
// local static); computed once at first use (off the audio thread via prepare()).
// ---------------------------------------------------------------------------

namespace detail
{
    inline const std::array<float, 33>& sineLut32()
    {
        static const std::array<float, 33> t = []()
        {
            std::array<float, 33> a {};
            for (int i = 0; i < 33; ++i)
                a[static_cast<size_t> (i)] = std::sin (6.28318530718f * static_cast<float> (i) / 32.0f);
            return a;
        }();
        return t;
    }
    // Bipolar triangle in [-1,1]: tri(p) = 1 - 4*|p-0.5|, p in [0,1).
    inline const std::array<float, 33>& triLut32()
    {
        static const std::array<float, 33> t = []()
        {
            std::array<float, 33> a {};
            for (int i = 0; i < 33; ++i)
            {
                const float p = static_cast<float> (i) / 32.0f;
                const float v = 1.0f - 4.0f * std::fabs (p - 0.5f);
                a[static_cast<size_t> (i)] = (v > 1.0f) ? 1.0f : (v < -1.0f ? -1.0f : v);
            }
            return a;
        }();
        return t;
    }
} // namespace detail

// Read the sine LUT at phase p in [0,1) with linear interpolation.
inline float lutSine32 (float p) noexcept
{
    p = p - std::floor (p);                          // wrap to [0,1)
    const float idx = p * 32.0f;
    const int i0 = static_cast<int> (idx);
    const float fr = idx - static_cast<float> (i0);
    const auto& t = detail::sineLut32();
    const auto i0u = static_cast<size_t> (i0);
    return t[i0u] + (t[i0u + 1] - t[i0u]) * fr;
}

// Read the triangle LUT at phase p in [0,1) with linear interpolation.
inline float lutTri32 (float p) noexcept
{
    p = p - std::floor (p);
    const float idx = p * 32.0f;
    const int i0 = static_cast<int> (idx);
    const float fr = idx - static_cast<float> (i0);
    const auto& t = detail::triLut32();
    const auto i0u = static_cast<size_t> (i0);
    return t[i0u] + (t[i0u + 1] - t[i0u]) * fr;
}

// ---------------------------------------------------------------------------
// Host <-> 32.768 kHz rate bridge with steep 15 kHz BW-limiting.
// ---------------------------------------------------------------------------
//
// Signal path (per process() block of n host samples):
//   host L/R
//     -> [input BW LP: 2x RBJ biquad @ 15 kHz, 4th-order Butterworth]
//     -> linear DOWNSAMPLE to 32.768 kHz (persistent fractional read phase so the
//        internal rate is exactly 32.768 kHz, drift-free)
//     -> internalL[]/internalR[] (float, read by the effect core)
//     ... effect runs the 24-bit fixed-point core per internal sample ...
//     -> linear UPSAMPLE to host rate
//     -> [output BW LP: 2x RBJ biquad @ 15 kHz]
//     -> host L/R
//
// "Do not use modern anti-alias filters" is honored: the BW limit is a steep
// simple-IIR cascade (RBJ cookbook biquads), NOT a polyphase FIR. The -3 dB
// cutoff is 15.0 kHz (inside the required [14.5, 15.5] band; just under the
// 32.768 kHz/2 = 16.384 kHz internal Nyquist so it doubles as the anti-image
// filter for the mild ~1.5:1 downsample).

class BiquadLP
{
    // Direct Form II Transposed, RBJ cookbook low-pass, a0 normalized out.
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

public:
    void clear() noexcept { z1 = z2 = 0.0f; }
    // Design a Butterworth (Q=0.7071) low-pass at fc Hz for sample rate fs.
    void design (double fc, double fs) noexcept
    {
        const double w0 = 6.283185307179586 * fc / fs;
        const double cw = std::cos (w0), sw = std::sin (w0);
        const double alpha = sw / (2.0 * 0.7071067811865476);
        const double a0 = 1.0 + alpha;
        b0 = (float) (((1.0 - cw) * 0.5) / a0);
        b1 = (float) ((1.0 - cw) / a0);
        b2 = b0;
        a1 = (float) ((-2.0 * cw) / a0);
        a2 = (float) ((1.0 - alpha) / a0);
    }
    float process (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        // Denormal flush: a stable IIR fed silence (a reverb/delay tail, a
        // paused track, or the gap between notes) decays its z-state toward 0
        // but never reaches it in finite steps, passing through the subnormal
        // range (~1e-38..1e-45) where x86 CPUs stall ~50x (a hazard for a
        // real-time audio thread). Subnormals are numerically indistinguishable from 0
        // here (the biquad's target is 15 kHz BW-limiting; 1e-40 is ~280 dB
        // below full scale), so zeroing them is inaudible and removes the
        // stall. (Direct Form II Transposed z1/z2 are the only float state.)
        if (std::fabs (z1) < std::numeric_limits<float>::min()) z1 = 0.0f;
        if (std::fabs (z2) < std::numeric_limits<float>::min()) z2 = 0.0f;
        return y;
    }
};

class RateBridge : public parvati::dsp::LinearResamplerCore<RateBridge>
{
public:
    static constexpr double kRate = kInternalRate;

    void prepare (double hostRate, int maxBlock) noexcept
    {
        hostRate_ = hostRate > 0.0 ? hostRate : 44100.0;
        const float ratio    = static_cast<float> (kRate / hostRate_);        // internal per host
        const float invRatio = static_cast<float> (hostRate_ / kRate);        // host per internal
        const int worst = static_cast<int> (std::ceil (static_cast<float> (std::max (1, maxBlock)) * ratio)) + 4;
        iL_.assign (static_cast<size_t> (std::max (8, worst)), 0.0f);
        iR_.assign (static_cast<size_t> (std::max (8, worst)), 0.0f);
        configure (ratio, invRatio, worst);

        // Steep 15 kHz BW-limit biquads (2 cascaded each -> 4th-order Butterworth).
        // CLAMP the cutoff below the host Nyquist: at host rates < ~30 kHz the
        // 15 kHz target exceeds fs/2 and a digital biquad there is UNSTABLE
        // (poles leave the unit circle -> inf/NaN). At e.g. 22050 Hz this drops
        // the cutoff to ~10.8 kHz, still a sane ADC/DAC emulation. Also clear the
        // z-state so a re-prepare on a live rate change never keeps stale history
        // (mirrors HostRateBridge, which re-Inits its Svfs in prepare).
        const double bwFc = std::min (kBwCutoffHz, 0.49 * hostRate_);
        for (int i = 0; i < 2; ++i)
        {
            inLpL_[i].design (bwFc, hostRate_);  inLpR_[i].design (bwFc, hostRate_);
            outLpL_[i].design (bwFc, hostRate_); outLpR_[i].design (bwFc, hostRate_);
            inLpL_[i].clear();  inLpR_[i].clear();
            outLpL_[i].clear(); outLpR_[i].clear();
        }
        hostTmpL_.assign (static_cast<size_t> (std::max (1, maxBlock)), 0.0f);
        hostTmpR_.assign (static_cast<size_t> (std::max (1, maxBlock)), 0.0f);
    }

    void reset() noexcept
    {
        std::fill (iL_.begin(), iL_.end(), 0.0f);
        std::fill (iR_.begin(), iR_.end(), 0.0f);
        parvati::dsp::LinearResamplerCore<RateBridge>::reset();
        for (int i = 0; i < 2; ++i)
        {
            inLpL_[i].clear();  inLpR_[i].clear();
            outLpL_[i].clear(); outLpR_[i].clear();
        }
    }

    float* internalL() noexcept { return iL_.data(); }
    float* internalR() noexcept { return iR_.data(); }
    int internalCount() const noexcept { return count(); }

    // Downsample host -> internal. Returns the number of internal samples (m).
    int hostToInternal (const float* L, const float* R, int n) noexcept
    {
        // 1) Input BW-limit (steep LP) at host rate.
        for (int i = 0; i < n; ++i)
        {
            float l = inLpL_[0].process (L[i]);
            hostTmpL_[static_cast<size_t> (i)] = inLpL_[1].process (l);
            float r = inLpR_[0].process (R[i]);
            hostTmpR_[static_cast<size_t> (i)] = inLpR_[1].process (r);
        }
        // 2) Linear downsample with persistent fractional phase (drift-free,
        //    shared transport in LinearResamplerCore).
        return downsample (hostTmpL_.data(), hostTmpR_.data(), n);
    }

    // Upsample internal -> host (and apply the output BW-limit).
    void internalToHost (float* L, float* R, int n) noexcept
    {
        upsample (L, R, n);
        if (count() <= 0)
            return;   // held-sample path (no internal sample) stays unfiltered
        // Output BW-limit (steep LP) at host rate.
        for (int i = 0; i < n; ++i)
        {
            float l = outLpL_[0].process (L[i]);
            L[i] = outLpL_[1].process (l);
            float r = outLpR_[0].process (R[i]);
            R[i] = outLpR_[1].process (r);
        }
    }

private:
    friend parvati::dsp::LinearResamplerCore<RateBridge>;

    // ---- Internal-domain storage hooks of the shared resampler core ----
    void storeSample (int m, float l, float r) noexcept
    {
        const auto mi = static_cast<size_t> (m);
        iL_[mi] = l;
        iR_[mi] = r;
    }
    float loadL (int j) const noexcept { return iL_[static_cast<size_t> (j)]; }
    float loadR (int j) const noexcept { return iR_[static_cast<size_t> (j)]; }

    static constexpr double kBwCutoffHz = 15000.0;   // within [14.5, 15.5] kHz
    double hostRate_ = 44100.0;
    std::vector<float> iL_, iR_, hostTmpL_, hostTmpR_;
    BiquadLP inLpL_[2], inLpR_[2], outLpL_[2], outLpR_[2];
};

} // namespace parvati::fv1

#endif // PARVATI_DSP_FX_FV1_FV1ENGINE_H

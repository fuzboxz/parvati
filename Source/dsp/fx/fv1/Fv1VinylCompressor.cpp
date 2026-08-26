// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.
//
// Fv1VinylCompressor implementation. See the header for the signal path and
// parameter mapping. The entire audio-path multiply/add is 24-bit fixed-point
// (Q.23) with saturation; only the compressor *detector* (envelope + gain)
// and the per-block coefficient setup run in float, as the per-effect spec
// allows. Tuned after the Roland SP-303/SP-404 "Vinyl Sim" COMP character
// (deep squash + heavy makeup + analog glue + warpy wow + subtle noise).

#include "dsp/fx/fv1/Fv1VinylCompressor.h"

#include <algorithm>
#include <cmath>

namespace hellcat::fv1
{

// Total delay memory: one DelayLine<2048> (no extra buffer). Well within the
// FV-1 32,768-sample RAM budget; the largest single delay VALUE in use
// (1638 + 300 wow + 24 flutter + interpolation headroom ~= 1963) is far under
// kMaxDelaySamples (32767) and inside the 2048 ring (< 2046, no write alias).
static_assert (DelayLine<2048>::capacity <= kMaxMemorySamples,
               "Fv1VinylCompressor total delay memory within the FV-1 budget");

void Fv1VinylCompressor::setParams (const std::array<float, kNumFxSlotParams>& param)
{
    pCompress_ = std::clamp (param[0], 0.0f, 1.0f);
    pWow_      = std::clamp (param[1], 0.0f, 1.0f);
    pCrackle_  = std::clamp (param[2], 0.0f, 1.0f);
    pAge_      = std::clamp (param[3], 0.0f, 1.0f);
    // param[4] is the chain Dry/Wet — NEVER read here.

    // Compress macro (SP-303/404 "COMP — the compression feel of an analog
    // record"): threshold sweeps 1.0 -> 0.04 (deep), ratio 4:1 -> ~16:1
    // (gain exponent 0.75 -> 0.94), makeup 1 -> 6 (the SP restores density
    // after the squash, pulling quiet material UP).
    th_       = 1.0f - 0.96f * pCompress_;
    ratioExp_ = 0.75f + 0.19f * pCompress_;
    makeup_   = 1.0f + 5.0f * pCompress_;

    // Saturation "lathe" drive: nearly transparent at rest, warm/crunchy at
    // full Compress. a = 0.06..0.31; knee xK = 1/sqrt(3a); flat top yMax =
    // (2/3)*min(xK, clamp). The quantized stages keep the FV-1 contract.
    const float a       = 0.06f + 0.25f * pCompress_;
    const float xKnee   = 1.0f / std::sqrt (3.0f * a);
    const float xClamp  = std::min (xKnee, 1.6f);
    satA14_    = q14 (a);
    satClampQ_ = static_cast<int32_t> (xClamp * static_cast<float> (kOneQ23));
    satMaxQ_   = static_cast<int32_t> ((2.0f / 3.0f) * xClamp * static_cast<float> (kOneQ23));

    // Age low-pass cutoff (700..15000 Hz). setCutoff quantizes to 14-bit.
    ageLp_.setCutoff (700.0f * std::pow (15000.0f / 700.0f, pAge_));
}

void Fv1VinylCompressor::prepareInternal (double /*sampleRate*/, int /*maxBlock*/)
{
    // Attack/release are fixed at the FV-1 internal rate (32.768 kHz):
    // fast attack (0.8 ms) catches transients like a vinyl mastering limiter;
    // the slow release (250 ms) is the SP pump-and-glide.
    attackA_  = 1.0f - std::exp (-1.0f / (0.0008f * 32768.0f));
    releaseA_ = 1.0f - std::exp (-1.0f / (0.250f * 32768.0f));
    resetInternal();
}

void Fv1VinylCompressor::resetInternal()
{
    delay_.clear();
    ageLp_.clear();
    env_     = 0.0f;
    ph1_     = 0.0f;
    ph2_     = 0.0f;
    lcgState_ = 0u;
    tickEnv_  = 0.0f;
    // Re-apply the coefficients from the cached params (clear() preserves
    // coeffs, but this keeps the filter sane even before the first setParams()).
    ageLp_.setCutoff (700.0f * std::pow (15000.0f / 700.0f, pAge_));
}

FxType Fv1VinylCompressor::type() const noexcept
{
    return FxType::VinylCompressor;
}

void Fv1VinylCompressor::processSampleFx (int32_t lin, int32_t /*rin*/,
                                          int32_t& lout, int32_t& rout)
{
    // ---- Compressor sidechain (float detector) ----
    const float xf = std::fabs (f24_toFloat (lin));
    const float a  = (xf > env_) ? attackA_ : releaseA_;
    env_ += a * (xf - env_);

    float gain = 1.0f;
    if (env_ > th_)
        gain = std::pow (th_ / env_, ratioExp_);   // 4:1 .. ~16:1
    float g = gain * makeup_;
    if (g < 0.0f) g = 0.0f;
    if (g > 6.5f) g = 6.5f;

    // Apply compressor+makeup gain g (in [0,6.5]) as saturating fixed-point.
    // Decompose g = ki + kf (ki in {0..5}, kf in [0,1)) so a single 14-bit
    // fractional multiply plus up to five saturating adds cover the full
    // range; saturation into the "lathe" below is part of the lo-fi glue.
    int ki = static_cast<int> (g);
    if (ki > 5) ki = 5;
    const float kf = g - static_cast<float> (ki);
    int32_t comp = f24_mulk (lin, q14 (kf));   // kf*lin
    for (int i = 0; i < ki; ++i)
        comp = f24_addSat (comp, lin);        // + ki*lin  (saturating)
    comp = f24_sat (comp);

    // ---- Saturation "lathe" (fixed-point cubic soft-clip + flat top) ----
    // y = x - a*x^3 for |x| <= xKnee (unity slope at 0 -> transparent when
    // Compress is low), flat ceiling yMax beyond — the analog glue/warmth,
    // driven by the Compress macro.
    {
        int32_t x = comp;
        if (x > satClampQ_)  x = satClampQ_;
        if (x < -satClampQ_) x = -satClampQ_;
        const int32_t x2  = f24_mul (x, x);
        const int32_t x3  = f24_mul (x2, x);
        int32_t y = f24_addSat (x, -f24_mulk (x3, satA14_));
        if (y > satMaxQ_)  y = satMaxQ_;
        if (y < -satMaxQ_) y = -satMaxQ_;
        comp = y;
    }

    // ---- Pitch-warp modulated delay: slow heavy WOW (0.4 Hz, <= 300 samples
    // = ~2.3 % pitch deviation — the audible "thumb on the record" warble;
    // 24 samples was only 0.18 % = sub-audible) + fast flutter (3.1 Hz,
    // <= 24 samples = ~1.4 %). Swing stays inside the 2048 ring
    // (1638 + 300 + 24 + interp headroom < 2046).
    delay_.write (comp);
    const float wow  = pWow_ * 300.0f;
    const float flut = pWow_ * 24.0f;
    const float readDelay = 1638.0f + lutSine32 (ph1_) * wow + lutSine32 (ph2_) * flut;
    const int32_t warped = delay_.readFrac (readDelay);
    ph1_ += 0.4f / 32768.0f;
    ph2_ += 3.1f / 32768.0f;
    if (ph1_ >= 1.0f) ph1_ -= 1.0f;   // keeps float precision within range over long runs
    if (ph2_ >= 1.0f) ph2_ -= 1.0f;

    // ---- Age low-pass ----
    const int32_t aged = ageLp_.process (warped);

    // ---- Vinyl noise floor: sparse SOFT ticks + low hiss ----
    // LCG shared by both. Ticks: ~0.6 % of samples trigger a decaying 2-3
    // sample envelope (a band-limited "tick", not a 1-sample pop), amplitude
    // u^2-skewed toward small, ceiling ~0.18 * Crackle — quiet, always under
    // the music. Hiss: ~-54 dB bipolar noise, the analog surface floor.
    lcgState_ = lcgState_ * 1103515245u + 12345u;   // unsigned: wraps mod 2^32
    const float v = static_cast<float> ((lcgState_ >> 16) & 0xFFFFu) / 65535.0f;
    if (v > 0.994f)
    {
        const float u = (v - 0.994f) / 0.006f;             // 0..1, uniform
        const float sign = ((lcgState_ & 0x100u) != 0u) ? 1.0f : -1.0f;
        tickEnv_ = sign * 0.18f * pCrackle_ * u * u;       // skewed small
    }
    else
        tickEnv_ *= 0.55f;                                  // decay the tick
    const float hissBits = static_cast<float> ((lcgState_ >> 8) & 0xFFu) * (2.0f / 255.0f) - 1.0f;
    const int32_t noise = f24_fromFloat (tickEnv_ + hissBits * 0.002f * pCrackle_);

    // ---- Output (mono-in / stereo-out; dry/wet is the chain's job) ----
    const int32_t out = f24_addSat (aged, noise);
    lout = out;
    rout = out;
}

} // namespace hellcat::fv1

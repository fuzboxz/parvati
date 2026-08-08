// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See FxChain.h.

#include "dsp/fx/FxChain.h"

#include <cmath>

#include "dsp/fx/FxProcessors.h"   // createFxProcessor

FxChain::FxChain()
{
    slotType_.fill (static_cast<uint8_t> (FxType::None));
    enabled_.fill (false);
    dryWet_.fill (0.0f);
    for (auto& p : params_)
        p.fill (0.0f);
}

FxChain::~FxChain() = default;

void FxChain::prepare (double rate, int maxBlock)
{
    rate_     = rate;
    maxBlock_ = juce::jmax (1, maxBlock);

    for (auto& s : slots_)
        if (s)
            s->prepare (rate, maxBlock_);

    // Reserve scratch buffers once (never on the audio thread).
    const size_t n = (size_t) maxBlock_;
    for (auto& b : wetL_) b.assign (n, 0.0f);
    for (auto& b : wetR_) b.assign (n, 0.0f);
    dryL_.assign (n, 0.0f);
    dryR_.assign (n, 0.0f);

    // Master-section state: clear the EQ filters + tail fades, recompute EQ
    // coeffs for the (possibly new) sample rate.
    for (auto& b : eqLow_)  b.reset();
    for (auto& b : eqMid_)  b.reset();
    for (auto& b : eqHigh_) b.reset();
    prevEnabled_.fill (false);
    wetFade_.fill (0.0f);
    updateEqCoeffs();
}

void FxChain::setSlotType (int slot, FxType t)
{
    if (slot < 0 || slot >= kNumFxSlots)
        return;
    const auto tv = static_cast<uint8_t> (t);
    if (slotType_[(size_t) slot] == tv && slots_[(size_t) slot] != nullptr)
        return;   // unchanged

    slotType_[(size_t) slot] = tv;
    slots_[(size_t) slot].reset();
    if (t != FxType::None && t < FxType::Count)
    {
        slots_[(size_t) slot] = createFxProcessor (t);
        if (slots_[(size_t) slot])
        {
            slots_[(size_t) slot]->prepare (rate_, maxBlock_);
            slots_[(size_t) slot]->reset();
        }
    }
}

void FxChain::setSlotEnabled (int slot, bool e) noexcept
{
    if (slot >= 0 && slot < kNumFxSlots)
        enabled_[(size_t) slot] = e;
}

void FxChain::setSlotDryWet (int slot, float dw) noexcept
{
    if (slot >= 0 && slot < kNumFxSlots)
        dryWet_[(size_t) slot] = juce::jlimit (0.0f, 1.0f, dw);
}

void FxChain::setSlotParam (int slot, int idx, float v) noexcept
{
    if (slot >= 0 && slot < kNumFxSlots && idx >= 0 && idx < kNumFxSlotParams)
        params_[(size_t) slot][(size_t) idx] = juce::jlimit (0.0f, 1.0f, v);
}

void FxChain::setTopology (FxTopology t) noexcept
{
    topology_ = static_cast<uint8_t> (t);
}

void FxChain::setOrder (const std::array<int, 3>& ord) noexcept
{
    order_ = ord;
}

void FxChain::setMasterMix (float g01) noexcept
{
    masterMix_ = juce::jlimit (0.0f, 1.0f, g01);
}

void FxChain::setKeepTails (bool keep) noexcept
{
    keepTails_ = keep;
}

void FxChain::setMasterEqLow (uint8_t v) noexcept
{
    v = (uint8_t) juce::jlimit (0, 127, (int) v);
    if (eqLowV_ == v) return;
    eqLowV_ = v;
    updateEqCoeffs();
}

void FxChain::setMasterEqMid (uint8_t v) noexcept
{
    v = (uint8_t) juce::jlimit (0, 127, (int) v);
    if (eqMidV_ == v) return;
    eqMidV_ = v;
    updateEqCoeffs();
}

void FxChain::setMasterEqHigh (uint8_t v) noexcept
{
    v = (uint8_t) juce::jlimit (0, 127, (int) v);
    if (eqHighV_ == v) return;
    eqHighV_ = v;
    updateEqCoeffs();
}

void FxChain::EqBiquad::process (float* io, int numSamples) noexcept
{
    // Direct Form II Transposed (numerically well-behaved, allocation-free).
    for (int i = 0; i < numSamples; ++i)
    {
        const float x = io[i];
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        io[i] = y;
    }
}

void FxChain::updateTailState (int numSamples) noexcept
{
    if (! keepTails_)
    {
        // Hard-cut (default): wetFade is binary; prevEnabled tracks enabled.
        for (int s = 0; s < kNumFxSlots; ++s)
        {
            wetFade_[(size_t) s] = enabled_[(size_t) s] ? 1.0f : 0.0f;
            prevEnabled_[(size_t) s] = enabled_[(size_t) s];
        }
        return;
    }
    // Fade a just-bypassed slot's wet over ~40 ms (one-pole block decay) so
    // delay/reverb tails ring out instead of cutting. perSample maps 1.0 ->
    // ~0.001 across a 40 ms window; blockDecay applies it for this block.
    const double tauSamples = 0.040 * rate_;
    const float perSample = tauSamples > 1.0
        ? std::pow (0.001f, 1.0f / static_cast<float> (tauSamples)) : 0.0f;
    const float blockDecay = std::pow (perSample, static_cast<float> (numSamples));
    for (int s = 0; s < kNumFxSlots; ++s)
    {
        const bool en = enabled_[(size_t) s];
        if (en)
        {
            wetFade_[(size_t) s] = 1.0f;
        }
        else if (slots_[(size_t) s] != nullptr && prevEnabled_[(size_t) s])
        {
            wetFade_[(size_t) s] *= blockDecay;
            if (wetFade_[(size_t) s] < 5.0e-4f)
                wetFade_[(size_t) s] = 0.0f;   // tail spent -> stop rendering
        }
        else
        {
            wetFade_[(size_t) s] = 0.0f;
        }
        prevEnabled_[(size_t) s] = en || wetFade_[(size_t) s] > 0.0f;
    }
}

void FxChain::updateEqCoeffs() noexcept
{
    eqActive_ = (eqLowV_ != 0) || (eqMidV_ != 64) || (eqHighV_ != 64);

    // RBJ audio-EQ cookbook coefficients (a0 normalised out), computed for both
    // channels. Flat bands are NOT processed (see applyMasterEq) so the EQ is a
    // bit-identical no-op at the defaults (low=0, mid=64, high=64).
    const double r = rate_ > 0.0 ? rate_ : 44100.0;
    constexpr double kTwoPi = 6.28318530717958647692;

    auto assign = [] (EqBiquad& b, double b0, double b1, double b2,
                      double a0, double a1, double a2)
    {
        const double inv = 1.0 / a0;
        b.b0 = (float) (b0 * inv);  b.b1 = (float) (b1 * inv);  b.b2 = (float) (b2 * inv);
        b.a1 = (float) (a1 * inv);  b.a2 = (float) (a2 * inv);
    };

    // Low-cut: high-pass, 20 Hz..~1.5 kHz exponential across 1..127 (0 = off).
    if (eqLowV_ != 0)
    {
        const double t = (double) (eqLowV_ - 1) / 126.0;
        const double freq = 20.0 * std::pow (1500.0 / 20.0, t);
        const double w0 = kTwoPi * freq / r, cw = std::cos (w0), sw = std::sin (w0);
        const double alpha = sw / (2.0 * 0.70710678);
        for (auto& b : eqLow_)
            assign (b, (1.0 + cw) * 0.5, -(1.0 + cw), (1.0 + cw) * 0.5,
                    1.0 + alpha, -2.0 * cw, 1.0 - alpha);
    }

    // Mid: peaking at 1 kHz, Q=1, gain (eqMid-64)/64 * +/-12 dB.
    {
        const double w0 = kTwoPi * 1000.0 / r, cw = std::cos (w0), sw = std::sin (w0);
        const double gainDB = ((double) eqMidV_ - 64.0) / 64.0 * 12.0;
        const double A = std::pow (10.0, gainDB / 40.0);
        const double alpha = sw / 2.0;   // Q=1
        for (auto& b : eqMid_)
            assign (b, 1.0 + alpha * A, -2.0 * cw, 1.0 - alpha * A,
                    1.0 + alpha / A, -2.0 * cw, 1.0 - alpha / A);
    }

    // High: shelf at 5 kHz, gain (eqHigh-64)/64 * +/-12 dB (slope S=1).
    {
        const double w0 = kTwoPi * 5000.0 / r, cw = std::cos (w0), sw = std::sin (w0);
        const double gainDB = ((double) eqHighV_ - 64.0) / 64.0 * 12.0;
        const double A = std::pow (10.0, gainDB / 40.0);
        const double sqA = std::sqrt (A);
        const double alpha = sw * 0.70710678;   // S=1 => sw/2 * sqrt(2)
        const double a0 = (A + 1.0) - (A - 1.0) * cw + 2.0 * sqA * alpha;
        for (auto& b : eqHigh_)
        {
            assign (b,
                    A * ((A + 1.0) + (A - 1.0) * cw + 2.0 * sqA * alpha),
                    -2.0 * A * ((A - 1.0) + (A + 1.0) * cw),
                    A * ((A + 1.0) + (A - 1.0) * cw - 2.0 * sqA * alpha),
                    a0,
                    2.0 * ((A - 1.0) - (A + 1.0) * cw),
                    (A + 1.0) - (A - 1.0) * cw - 2.0 * sqA * alpha);
        }
    }
}

void FxChain::applyMasterEq (float* L, float* R, int numSamples) noexcept
{
    // Only non-flat bands run; flat bands are an exact passthrough.
    if (eqLowV_  != 0)  { eqLow_[0].process (L, numSamples);  eqLow_[1].process (R, numSamples); }
    if (eqMidV_  != 64) { eqMid_[0].process (L, numSamples);  eqMid_[1].process (R, numSamples); }
    if (eqHighV_ != 64) { eqHigh_[0].process (L, numSamples); eqHigh_[1].process (R, numSamples); }
}

bool FxChain::anyEnabled() const noexcept
{
    for (int s = 0; s < kNumFxSlots; ++s)
        if (enabled_[(size_t) s] && slots_[(size_t) s] != nullptr)
            return true;
    return false;
}

void FxChain::process (const float* inL, const float* inR,
                       float* outL, float* outR, int numSamples)
{
    // Advance per-slot tail fades (keepTails) so a just-bypassed slot keeps
    // rendering; in hard-cut mode (default) wetFade is binary (= enabled).
    updateTailState (numSamples);

    const bool anyAct = slotActive (0) || slotActive (1) || slotActive (2);

    // Fast bypass: nothing active AND master section at no-op defaults => the
    // chain is a transparent dry copy (audibly- and bit-identical to the
    // pre-master path). This is the default (all slots disabled) fast path.
    if (! anyAct && masterMix_ >= 1.0f && ! eqActive_)
    {
        juce::FloatVectorOperations::copy (outL, inL, numSamples);
        juce::FloatVectorOperations::copy (outR, inR, numSamples);
        return;
    }

    // ---- Render the chain (topology) into outL/outR ----
    if (! anyAct)
    {
        juce::FloatVectorOperations::copy (outL, inL, numSamples);
        juce::FloatVectorOperations::copy (outR, inR, numSamples);
    }
    else if (topology_ == static_cast<uint8_t> (FxTopology::Series))
    {
        // ---- Series ----
        // Walk the order permutation. The running signal starts as the dry
        // input; each enabled slot processes it in place (writing WET), then we
        // blend dry*(1-dw) + wet*dw. A disabled slot is a passthrough. The dry
        // snapshot is captured before the in-place process() overwrites it.
        juce::FloatVectorOperations::copy (outL, inL, numSamples);
        juce::FloatVectorOperations::copy (outR, inR, numSamples);

        for (int oi = 0; oi < kNumFxSlots; ++oi)
        {
            const int s = order_[(size_t) oi];
            auto& proc = slots_[(size_t) s];
            if (! proc || ! slotActive (s))
                continue;   // passthrough

            // Snapshot the pre-process (dry) signal.
            juce::FloatVectorOperations::copy (dryL_.data(), outL, numSamples);
            juce::FloatVectorOperations::copy (dryR_.data(), outR, numSamples);

            proc->setParams (params_[(size_t) s].data());
            proc->process (outL, outR, numSamples);   // outL/outR now hold WET

            const float dw  = dryWet_[(size_t) s] * wetFade_[(size_t) s];
            const float dry = 1.0f - dw;
            for (int i = 0; i < numSamples; ++i)
            {
                outL[i] = dryL_[(size_t) i] * dry + outL[i] * dw;
                outR[i] = dryR_[(size_t) i] * dry + outR[i] * dw;
            }
        }
    }
    else if (topology_ == static_cast<uint8_t> (FxTopology::Parallel12to3))
    {
        // ---- Parallel12to3:  (A || B) -> C ----
        // A and B each process a COPY of the dry input (equal-gain sum into
        // parallelOut), then C processes parallelOut in series. A disabled slot
        // is a passthrough. order_[0..2] = A,B,C.
        const int A = order_[0];
        const int B = order_[1];
        const int C = order_[2];

        // parallelOut into outL/outR: equal-gain blend of {A,B} over the input.
        renderParallel (inL, inR, outL, outR, numSamples, A, B);

        // Series C over parallelOut: snapshot, process in place, dry/wet blend.
        auto& procC = slots_[(size_t) C];
        if (procC && slotActive (C))
        {
            juce::FloatVectorOperations::copy (dryL_.data(), outL, numSamples);
            juce::FloatVectorOperations::copy (dryR_.data(), outR, numSamples);

            procC->setParams (params_[(size_t) C].data());
            procC->process (outL, outR, numSamples);   // outL/outR now hold WET

            const float dw  = dryWet_[(size_t) C] * wetFade_[(size_t) C];
            const float dry = 1.0f - dw;
            for (int i = 0; i < numSamples; ++i)
            {
                outL[i] = dryL_[(size_t) i] * dry + outL[i] * dw;
                outR[i] = dryR_[(size_t) i] * dry + outR[i] * dw;
            }
        }
        // C disabled => passthrough: outL/outR keep parallelOut unchanged.
    }
    else
    {
        // ---- Parallel1to23:  A -> (B || C) ----
        // A processes the dry input in series (stage1), then B and C each process
        // a COPY of stage1 (equal-gain sum). A disabled => stage1 = dry
        // passthrough. order_[0..2] = A,B,C.
        const int A = order_[0];
        const int B = order_[1];
        const int C = order_[2];

        // Stage 1 (series A) into outL/outR, starting from the dry input.
        juce::FloatVectorOperations::copy (outL, inL, numSamples);
        juce::FloatVectorOperations::copy (outR, inR, numSamples);

        auto& procA = slots_[(size_t) A];
        if (procA && slotActive (A))
        {
            juce::FloatVectorOperations::copy (dryL_.data(), outL, numSamples);
            juce::FloatVectorOperations::copy (dryR_.data(), outR, numSamples);

            procA->setParams (params_[(size_t) A].data());
            procA->process (outL, outR, numSamples);   // outL/outR now hold WET

            const float dw  = dryWet_[(size_t) A] * wetFade_[(size_t) A];
            const float dry = 1.0f - dw;
            for (int i = 0; i < numSamples; ++i)
            {
                outL[i] = dryL_[(size_t) i] * dry + outL[i] * dw;
                outR[i] = dryR_[(size_t) i] * dry + outR[i] * dw;
            }
        }

        // Stage 2 (parallel {B,C} over stage1). Snapshot stage1 into dryL_/dryR_
        // (the parallel input reference), then blend into outL/outR.
        juce::FloatVectorOperations::copy (dryL_.data(), outL, numSamples);
        juce::FloatVectorOperations::copy (dryR_.data(), outR, numSamples);

        renderParallel (dryL_.data(), dryR_.data(), outL, outR, numSamples, B, C);
    }

    // ---- Global wet/dry mix (no-op at masterMix_ == 1.0) ----
    if (masterMix_ < 1.0f)
    {
        const float g = masterMix_, dry = 1.0f - g;
        for (int i = 0; i < numSamples; ++i)
        {
            outL[i] = inL[i] * dry + outL[i] * g;
            outR[i] = inR[i] * dry + outR[i] * g;
        }
    }

    // ---- Master EQ (skipped entirely when flat) ----
    if (eqActive_)
        applyMasterEq (outL, outR, numSamples);
}

void FxChain::renderParallel (const float* inL, const float* inR,
                              float* outL, float* outR, int numSamples,
                              int slotA, int slotB)
{
    // Equal-gain parallel blend of TWO slots over @p inL/inR into @p outL/outR,
    // reusing the former full-sum Parallel formula: sum the wet outputs of the
    // active slots, divide by the active count, and blend against the dry input
    // by the mean dry/wet W (out = dry*(1-W) + (sum wet)/activeCount * W).
    // outL/outR are CLEARED first; with BOTH slots disabled/None the input is
    // copied through unchanged. Allocation-free (uses pre-sized wetL_/wetR_).
    juce::FloatVectorOperations::clear (outL, numSamples);
    juce::FloatVectorOperations::clear (outR, numSamples);

    const int pair[2] = { slotA, slotB };
    int activeCount = 0;
    float dwSum = 0.0f;

    for (int p = 0; p < 2; ++p)
    {
        const int s = pair[p];
        auto& proc = slots_[(size_t) s];
        if (! proc || ! slotActive (s))
            continue;

        juce::FloatVectorOperations::copy (wetL_[(size_t) s].data(), inL, numSamples);
        juce::FloatVectorOperations::copy (wetR_[(size_t) s].data(), inR, numSamples);
        proc->setParams (params_[(size_t) s].data());
        proc->process (wetL_[(size_t) s].data(), wetR_[(size_t) s].data(), numSamples);

        juce::FloatVectorOperations::add (outL, wetL_[(size_t) s].data(), numSamples);
        juce::FloatVectorOperations::add (outR, wetR_[(size_t) s].data(), numSamples);
        ++activeCount;
        dwSum += dryWet_[(size_t) s] * wetFade_[(size_t) s];
    }

    if (activeCount > 0)
    {
        const float inv = 1.0f / (float) activeCount;
        const float W   = (dwSum * inv);   // mean dry/wet, 0..1
        const float dry = 1.0f - W;
        for (int i = 0; i < numSamples; ++i)
        {
            outL[i] = inL[i] * dry + outL[i] * inv * W;
            outR[i] = inR[i] * dry + outR[i] * inv * W;
        }
    }
    else
    {
        // Both slots disabled/None: transparent passthrough of the input.
        juce::FloatVectorOperations::copy (outL, inL, numSamples);
        juce::FloatVectorOperations::copy (outR, inR, numSamples);
    }
}

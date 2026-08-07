// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See FxChain.h.

#include "dsp/fx/FxChain.h"

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
    // Dry-copy bypass: with no enabled slot the chain is transparent, so the
    // part's main contribution is the dry summed signal (audibly-identical to
    // the pre-FX path). This is the default (fx*_enabled=0) fast path.
    if (! anyEnabled())
    {
        juce::FloatVectorOperations::copy (outL, inL, numSamples);
        juce::FloatVectorOperations::copy (outR, inR, numSamples);
        return;
    }

    if (topology_ == static_cast<uint8_t> (FxTopology::Series))
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
            if (! proc || ! enabled_[(size_t) s])
                continue;   // passthrough

            // Snapshot the pre-process (dry) signal.
            juce::FloatVectorOperations::copy (dryL_.data(), outL, numSamples);
            juce::FloatVectorOperations::copy (dryR_.data(), outR, numSamples);

            proc->setParams (params_[(size_t) s].data());
            proc->process (outL, outR, numSamples);   // outL/outR now hold WET

            const float dw  = dryWet_[(size_t) s];
            const float dry = 1.0f - dw;
            for (int i = 0; i < numSamples; ++i)
            {
                outL[i] = dryL_[(size_t) i] * dry + outL[i] * dw;
                outR[i] = dryR_[(size_t) i] * dry + outR[i] * dw;
            }
        }
    }
    else
    {
        // ---- Parallel ----
        // Each enabled slot processes a COPY of the input; the wet outputs are
        // summed (equal-gain, / activeCount) and blended with the dry signal.
        // The blend level W is the mean dry/wet of the active slots, so W=0
        // (all drywet 0) => fully dry, consistent with the series path + default.
        juce::FloatVectorOperations::clear (outL, numSamples);
        juce::FloatVectorOperations::clear (outR, numSamples);

        int activeCount = 0;
        float dwSum = 0.0f;
        for (int s = 0; s < kNumFxSlots; ++s)
        {
            auto& proc = slots_[(size_t) s];
            if (! proc || ! enabled_[(size_t) s])
                continue;

            juce::FloatVectorOperations::copy (wetL_[(size_t) s].data(), inL, numSamples);
            juce::FloatVectorOperations::copy (wetR_[(size_t) s].data(), inR, numSamples);
            proc->setParams (params_[(size_t) s].data());
            proc->process (wetL_[(size_t) s].data(), wetR_[(size_t) s].data(), numSamples);

            juce::FloatVectorOperations::add (outL, wetL_[(size_t) s].data(), numSamples);
            juce::FloatVectorOperations::add (outR, wetR_[(size_t) s].data(), numSamples);
            ++activeCount;
            dwSum += dryWet_[(size_t) s];
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
            // (anyEnabled() already handled the all-off case, but keep this safe.)
            juce::FloatVectorOperations::copy (outL, inL, numSamples);
            juce::FloatVectorOperations::copy (outR, inR, numSamples);
        }
    }
}

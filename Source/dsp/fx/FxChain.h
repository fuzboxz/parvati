// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// FxChain — the per-part 3-slot FX chain. Owns up to 3 FxProcessor instances
// (rebuilt when a slot's FxType changes), holds per-slot enabled/dryWet/4-params
// (AT-local floats) plus topology + process order, and renders a mono-in /
// stereo-out block.
//
// Topology:
//  - Series:   order permutation applied; each slot: wet = process(dry);
//              out = dry*(1-dw) + wet*dw. A disabled slot is a passthrough.
//  - Parallel: each slot processes a COPY of the input; the slot outputs are
//              summed (equal-gain, scaled by 1/activeCount) then dry/wet-blended.
//  - Bypass:   if !anyEnabled(), process() copies in->out (dry), so with all FX
//              disabled the part's main contribution is the dry summed signal
//              (audibly-identical to the pre-FX path).
//
// FxType/FxTopology are forward-declared via FxProcessor.h; the chain caches
// the current slot types as uint8_t to avoid requiring the enum to be complete
// in this header (SynthEngine.h includes this file before defining the enum).
// FxChain.cpp includes SynthEngine.h for the enumerators.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "dsp/fx/FxProcessor.h"

class FxChain
{
public:
    FxChain();
    ~FxChain();

    // Reserve internal DSP state for up to maxBlock stereo samples at rate.
    // Safe to call on a sample-rate / block-size change.
    void prepare (double rate, int maxBlock);

    // Rebuild this slot's processor for a new effect type. Allocations (factory
    // + processor prepare) happen here — call only when fxDirty_ is serviced
    // (single-threaded on the AT), never inside process().
    void setSlotType (int slot, FxType t);
    void setSlotEnabled (int slot, bool e) noexcept;
    void setSlotDryWet (int slot, float dw) noexcept;      // 0..1 (0 = fully dry)
    void setSlotParam  (int slot, int idx, float v) noexcept;   // 0..1
    void setTopology (FxTopology t) noexcept;
    void setOrder (const std::array<int, 3>& ord) noexcept;

    // Fast bypass test: true if at least one enabled slot with a non-None type.
    bool anyEnabled() const noexcept;

    // Render the mono-in / stereo-out block. When !anyEnabled(), copies in->out
    // (dry). numSamples <= the maxBlock passed to prepare().
    void process (const float* inL, const float* inR,
                  float* outL, float* outR, int numSamples);

private:
    double rate_ = 44100.0;
    int maxBlock_ = 0;

    std::array<std::unique_ptr<FxProcessor>, kNumFxSlots> slots_;
    std::array<uint8_t, kNumFxSlots> slotType_ {};   // FxType as uint8_t (cache)
    std::array<bool,  kNumFxSlots> enabled_  {};     // filled false
    std::array<float, kNumFxSlots> dryWet_   {};     // filled 0.0f
    std::array<std::array<float, kNumFxSlotParams>, kNumFxSlots> params_ {};   // 0.0f

    uint8_t topology_ = 0;   // FxTopology::Series
    std::array<int, 3> order_ { 0, 1, 2 };

    // Per-block scratch buffers (sized once in prepare; never on the AT):
    //  - wetL_/wetR_: one per slot, for parallel topology (each slot processes
    //    a copy of the input).
    //  - dryL_/dryR_: a single dry snapshot, for the series dry/wet blend.
    std::array<std::vector<float>, kNumFxSlots> wetL_, wetR_;
    std::vector<float> dryL_, dryR_;
};

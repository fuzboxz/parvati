// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// FxChain — the per-part 3-slot FX chain. Owns up to 3 FxProcessor instances
// (rebuilt when a slot's FxType changes), holds per-slot enabled/dryWet/4-params
// (AT-local floats) plus topology + process order, and renders a mono-in /
// stereo-out block.
//
// Topology (A=order_[0], B=order_[1], C=order_[2]; a disabled slot is a
// passthrough in every topology):
//  - Series:          A -> B -> C  (each slot processes the running signal;
//                                   out = dry*(1-dw) + wet*dw per slot).
//  - Parallel12to3:   (A || B) -> C  (A and B each process a COPY of the dry
//                                   input, equal-gain sum, then C processes the
//                                   sum in series).
//  - Parallel1to23:   A -> (B || C)  (A processes the dry input in series, then
//                                   B and C each process a copy of A's output,
//                                   equal-gain sum).
//  - Bypass:          if !anyEnabled(), process() copies in->out (dry), so with
//                                   all FX disabled the part's main contribution
//                                   is the dry summed signal (audibly-identical
//                                   to the pre-FX path).
//
// Tail retention is ALWAYS ON (no toggle): a bypassed slot's wet decays toward
// 0 over ~0.30 s so its delay/reverb tail rings out instead of hard-cutting
// (B1), and a just-engaged / freshly type-changed slot fades in over ~5 ms so it
// does not slam in at full wet (B3/B4). The fade is a per-sample one-pole
// (wetFade_), advanced inside each blend loop, not a block multiplier.
//
// The two-branch parallel blend is shared via renderParallel(). FxType/FxTopology are forward-declared via FxProcessor.h; the chain caches
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

    // ---- Master section (v3) ----
    // Global chain wet/dry (0..1; 1 = fully wet = the pre-master default, a
    // no-op).
    void setMasterMix (float g01) noexcept;
    // Master EQ bands (uint8 params): low = 0..127 low-cut amount (0 = off);
    // mid/high = 0..127 where 64 = unity (0 dB).
    void setMasterEqLow  (uint8_t v) noexcept;
    void setMasterEqMid  (uint8_t v) noexcept;
    void setMasterEqHigh (uint8_t v) noexcept;

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
    std::array<float, kNumFxSlots> dryWet_   {};     // TARGET dry/wet 0..1 (set by setSlotDryWet); block-constant target
    std::array<float, kNumFxSlots> dryWetCur_ {};    // per-sample-smoothed current dry/wet (B2-engine/B6)
    std::array<std::array<float, kNumFxSlotParams>, kNumFxSlots> params_ {};   // 0.0f

    uint8_t topology_ = 0;   // FxTopology::Series
    std::array<int, 3> order_ { 0, 1, 2 };

    // Per-block scratch buffers (sized once in prepare; never on the AT):
    //  - wetL_/wetR_: one per slot — each parallel contributor processes a copy
    //    of its stage input here (Parallel12to3 uses A,B; Parallel1to23 uses B,C).
    //  - dryL_/dryR_: a single dry/stage snapshot, for the series dry/wet blend
    //    and as the parallel stage's input reference.
    std::array<std::vector<float>, kNumFxSlots> wetL_, wetR_;
    std::vector<float> dryL_, dryR_;

    // ---- Master section (v3) state ----
    // A hand-rolled biquad (RBJ cookbook, Direct Form II Transposed) keeps this
    // header free of a juce_dsp dependency (FxChain.h is included widely via
    // SynthEngine.h). 3 bands x stereo (index 0 = L, 1 = R).
    struct EqBiquad
    {
        float a1 = 0.0f, a2 = 0.0f, b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;  // coeffs (a0 normalised out)
        float z1 = 0.0f, z2 = 0.0f;                                  // state
        void reset() noexcept { z1 = z2 = 0.0f; }
        void process (float* io, int numSamples) noexcept;          // in-place
    };
    std::array<EqBiquad, 2> eqLow_, eqMid_, eqHigh_;
    uint8_t eqLowV_ = 0, eqMidV_ = 64, eqHighV_ = 64;   // cached params (detect change)
    bool eqActive_ = false;                             // false => skip EQ entirely (bit-identical default)

    float masterMix_ = 1.0f;          // TARGET master mix 0..1 (set by setMasterMix; 1 = no-op default)
    float masterMixCur_ = 1.0f;       // per-sample-smoothed current master mix (B6)

    // ---- Tail retention + click-free enable/disable (always on) ----
    // Tails are now ALWAYS retained (no toggle). Each slot's wet is governed by
    // a per-sample one-pole fade wetFade_[s] in 0..1, driven every sample toward
    // (enabled_[s] ? 1 : 0): a just-bypassed slot's wet decays over the fade-OUT
    // tau so its reverb/delay tail rings out instead of hard-cutting (B1), and a
    // just-engaged / freshly type-changed slot fades IN so it does not slam in
    // at full wet (B3/B4). The fade is advanced per sample inside each blend
    // loop (blendSlotWetFade / renderParallel), NOT as a block multiplier.
    // dryWet_[s] stays a block constant in this step (its smoothing is later).
    static constexpr float kFadeOutTauSec = 0.30f;   // ~0.30 s: long enough for real tails
    static constexpr float kFadeInTauSec  = 0.005f;  // ~5 ms: fast enough to hide the engage click
    float coefIn_  = 1.0f;   // one-pole per-sample coeff while rising toward 1
    float coefOut_ = 1.0f;   // one-pole per-sample coeff while falling toward 0
    std::array<float, kNumFxSlots> wetFade_ {};   // per-sample one-pole wet mult (0..1)

    // ---- Per-sample smoothing of dryWet / masterMix gain-style params (B2-engine, B6) ----
    // The per-slot dry/wet target (dryWet_[s]) and the global master-mix target
    // (masterMix_) are set once per block by setSlotDryWet / setMasterMix (which
    // also fire for FX-mod-matrix dryWet/masterMix destinations). Applying them
    // as block constants makes a knob move / parameter automation / FX-mod
    // modulation step once per block — zipper noise / clicks. These one-pole
    // smoothers ramp the *current* value (dryWetCur_[s] / masterMixCur_) toward
    // the target per sample inside the blend loops, so gain-style transitions
    // are continuous. Per-EFFECT-param smoothing (gain, feedback, reverb size,
    // etc.) is deliberately OUT OF SCOPE here — that lives in FxProcessors.cpp.
    static constexpr double kParamSmoothTauSec = 0.020;   // 20 ms: fast enough to feel instant on knob moves, slow enough to hide block-rate steps and preserve slow LFO wet modulation
    float smoothCoef_ = 1.0f;   // per-sample one-pole coeff toward the target (computed in prepare)

    // Per-sample dry/wet blend for a single series-style slot: blends the
    // pre-process dry snapshot (dryL_/dryR_) against the wet signal in outL/outR
    // and advances this slot's one-pole fade one sample per iteration (persisted
    // back into wetFade_[s]). dryWet_[s] is the block-constant target; the
    // per-sample-smoothed dryWetCur_[s] AND the wet fade are each read +
    // advanced one sample per iteration (both persisted back).
    void blendSlotWetFade (float* outL, float* outR, int numSamples, int s) noexcept;
    // True if slot @p s should render this block (enabled OR still tailing out).
    bool slotActive (int s) const noexcept { return enabled_[(size_t) s] || wetFade_[(size_t) s] > 5.0e-4f; }
    // Recompute the EQ biquad coeffs from eqLowV_/eqMidV_/eqHighV_ (call on change).
    void updateEqCoeffs() noexcept;
    // Apply the 3-band master EQ in place to L+R (skipped when !eqActive_).
    void applyMasterEq (float* L, float* R, int numSamples) noexcept;

    // Render the equal-gain parallel blend of TWO slots over @p inL/inR into
    // @p outL/outR, reusing the blend formula of the former full-sum Parallel
    // path: each active slot (enabled OR still tailing, with a live processor)
    // processes a copy of the input; the wet outputs are summed, divided by the
    // active count, and blended against the dry input by the per-sample mean
    // dry/wet W (out = dry*(1-W) + (sum wet)/activeCount * W). Each active slot's
    // one-pole wetFade_ is advanced per sample within the blend loop and
    // persisted back. outL/outR are CLEARED first. With BOTH slots disabled/None
    // the input is copied through unchanged. Allocation-free.
    void renderParallel (const float* inL, const float* inR,
                         float* outL, float* outR, int numSamples,
                         int slotA, int slotB);
};

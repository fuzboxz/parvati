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
    // Staged-swap + retirement parking (audit F1): mirror the slot-type
    // defaults (None) so the MT no-op check starts coherent.
    mtLastType_.fill (static_cast<uint8_t> (FxType::None));
    pendingType_.fill (static_cast<uint8_t> (FxType::None));
    // F-eng-3: -1 = "no staged swap" (None / factory-refused), so the AT-side
    // latency() snapshot never misreads a value-initialized 0 as a real staged
    // latency before the first stage (state==Empty gates it anyway — belt and
    // braces, matching pendingType_'s defensive fill above).
    for (auto& pl : pendingLatency_)
        pl.store (-1, std::memory_order_relaxed);
    for (auto& st : stageState_)
        st.store (kStageEmpty, std::memory_order_relaxed);
    for (auto& r : retired_)
        r.store (nullptr, std::memory_order_relaxed);
    retiredDirty_.store (false, std::memory_order_relaxed);
}

FxChain::~FxChain()
{
    // The engine (and thus every chain) is destroyed with the audio callback
    // stopped, so claiming any parked processors with a plain exchange is
    // race-free here. pending_ releases via its own destructor on this thread.
    for (auto& r : retired_)
        delete r.exchange (nullptr, std::memory_order_acq_rel);
}

void FxChain::prepare (double rate, int maxBlock)
{
    rate_     = rate;
    maxBlock_ = juce::jmax (1, maxBlock);

    // Apply any staged-but-unconsumed type swaps FIRST (prepare runs with the
    // audio callback stopped, so the shared install routine is safe here).
    // This both honours a swap staged before prepareToPlay and re-sizes a
    // processor staged against a stale maxBlock_ (the loop below re-prepares
    // every slot at the new rate/block, so a swapped-in processor can never
    // keep undersized scratch).
    servicePendingTypeSwaps();
    reapRetired();   // retire what that apply parked (same thread discipline)

    for (auto& s : slots_)
        if (s)
            s->prepare (rate, maxBlock_);

    // Reserve scratch buffers once (never on the audio thread).
    const size_t n = (size_t) maxBlock_;
    for (auto& b : wetL_) b.assign (n, 0.0f);
    for (auto& b : wetR_) b.assign (n, 0.0f);
    dryL_.assign (n, 0.0f);
    dryR_.assign (n, 0.0f);

    // Latency-compensation rings (D1/D2): zero history on a re-prepare so a
    // mid-session rate/block change does not replay stale old-rate audio.
    for (int s = 0; s < kNumFxSlots; ++s)
    {
        dryDelayL_[(size_t) s].fill (0.0f);
        dryDelayR_[(size_t) s].fill (0.0f);
        wetDelayL_[(size_t) s].fill (0.0f);
        wetDelayR_[(size_t) s].fill (0.0f);
        dryDelayPos_[(size_t) s] = 0;
        wetDelayPos_[(size_t) s] = 0;
    }
    parDryL_.fill (0.0f);
    parDryR_.fill (0.0f);
    parDryPos_ = 0;
    masterDryL_.fill (0.0f);
    masterDryR_.fill (0.0f);
    masterDryPos_ = 0;

    // Master-section state: clear the EQ biquad z-state (invalidated by a
    // sample-rate change) and recompute the per-sample fade coefficients + EQ
    // coeffs for the (possibly new) sample rate. The tail fades (wetFade_) and
    // the smoothed currents (dryWetCur_/masterMixCur_) are unitless 0..1 state
    // independent of sample rate, so they are PRESERVED across a re-prepare:
    // a host mid-session sample-rate / buffer-size change must NOT truncate a
    // ringing tail (by zeroing wetFade_) nor dip an enabled effect toward dry
    // (by zeroing wetFade_ / snapping dryWetCur_). Only the rate-dependent
    // coefficients and the EQ z-state are refreshed here (B7).
    for (auto& b : eqLow_)  b.reset();
    for (auto& b : eqMid_)  b.reset();
    for (auto& b : eqHigh_) b.reset();

    // Per-sample one-pole fade coefficients from rate_:
    //   coef = 1 - exp(-1 / (tau * sampleRate)).
    // Fade-OUT (0.30 s) is long enough for a bypassed slot's reverb/delay tail
    // to ring out (B1); fade-IN (5 ms) is short enough to hide the engage click
    // of a just-engaged / freshly type-changed slot (B3/B4).
    const double sr = rate_ > 0.0 ? rate_ : 44100.0;
    coefOut_ = 1.0f - (float) std::exp (-1.0 / ((double) kFadeOutTauSec * sr));
    coefIn_  = 1.0f - (float) std::exp (-1.0 / ((double) kFadeInTauSec  * sr));

    // Per-sample dry/wet + masterMix smoothing coeff (B2-engine, B6). 20 ms tau:
    // fast enough to feel instant on a knob move, slow enough to hide a block-
    // rate step and to preserve slow LFO wet modulation. The smoothed currents
    // (dryWetCur_/masterMixCur_) are NOT snapped to target here: they default to
    // 0/1.0 at construction and any target set before the first prepare ramps in
    // naturally (a gentle 5-20 ms fade-in on first audio, no click); on a re-
    // prepare they are preserved so an enabled effect does not dip (B7).
    smoothCoef_ = 1.0f - (float) std::exp (-1.0 / (kParamSmoothTauSec * sr));

    updateEqCoeffs();
}

//==========================================================================
// Staged type swap (audit F1: build on the message thread, install on the AT
// with pointer moves only).
bool FxChain::acquireStagingSlot (int slot) noexcept
{
    // Spin over the 4-way stage state until this thread owns Filling. The
    // contending transitions are in-flight AT install work: the AT holds
    // kStageConsuming for the microseconds its pointer moves take, then
    // stores Empty. Neither CAS below matches kStageConsuming, so while the
    // AT owns the install this loop yields and retries until that final
    // Empty store -- bounded by microseconds, not by audio blocks. An
    // audio-stopped engine never spins here because the take-back CAS below
    // succeeds immediately.
    for (;;)
    {
        int cur = kStageEmpty;
        if (stageState_[(size_t) slot].compare_exchange_strong (
                cur, kStageFilling, std::memory_order_acquire, std::memory_order_acquire))
            return true;   // Empty -> Filling
        if (cur == kStageStaged
            && stageState_[(size_t) slot].compare_exchange_strong (
                   cur, kStageFilling, std::memory_order_acquire, std::memory_order_acquire))
            return true;   // take back an unconsumed swap (coalesce: newest wins)
        // cur == kStageFilling is impossible from this thread (single staging
        // thread); the state changed under us -- retry.
        juce::Thread::yield();
    }
}

void FxChain::setSlotType (int slot, FxType t)
{
    if (slot < 0 || slot >= kNumFxSlots)
        return;
    const auto tv = static_cast<uint8_t> (t);

    // No-op check against the MT-side mirror (slotType_ itself is AT-owned):
    // the slot already shows this type AND nothing NEWER is queued -> skip
    // the factory + prepare entirely (every fxDirty_ service re-pushes all
    // three types, so this early-out is what keeps a plain knob turn from
    // rebuilding processors). A queued swap for the SAME type (state==Staged,
    // pendingType_==tv) also short-circuits: re-staging would rebuild +
    // take back an identical object to no effect. pendingType_ is safe to
    // read here: only this thread writes it, and only while it holds Filling.
    if (mtLastType_[(size_t) slot] == tv)
    {
        const int st = stageState_[(size_t) slot].load (std::memory_order_acquire);
        // kStageConsuming deliberately does NOT match either arm: the AT is
        // mid-install of this slot's pending_, so this call must stage a NEW
        // swap and acquireStagingSlot spins until the AT's final Empty store.
        if (st == kStageEmpty
            || (st == kStageStaged && pendingType_[(size_t) slot] == tv))
            return;
    }

    acquireStagingSlot (slot);
    // We own pending_[slot]: build + prepare the replacement HERE (message
    // thread -- the allocation site the audit flagged). A None type stages an
    // empty slot (the install clears the processor). Assigning pending_ also
    // frees an un-consumed older staging on this thread (the coalescing
    // choice: a queued-but-never-installed swap was never audible).
    if (t != FxType::None && t < FxType::Count)
    {
        auto next = createFxProcessor (t);
        if (next)
        {
            next->prepare (rate_, juce::jmax (1, maxBlock_));
            next->reset();
        }
        // F-eng-3: publish the staged latency VALUE (not the pointer) for the
        // audio-thread latency() snapshot; release-order it before kStageStaged.
        pendingLatency_[(size_t) slot].store (next ? next->latency() : -1,
                                              std::memory_order_release);
        pending_[(size_t) slot] = std::move (next);
    }
    else
    {
        pendingLatency_[(size_t) slot].store (-1, std::memory_order_release);
        pending_[(size_t) slot].reset();
    }
    pendingType_[(size_t) slot] = tv;
    mtLastType_[(size_t) slot]  = tv;
    stageState_[(size_t) slot].store (kStageStaged, std::memory_order_release);
}

void FxChain::servicePendingTypeSwaps() noexcept
{
    for (int s = 0; s < kNumFxSlots; ++s)
        consumePendingSwap (s);
}

void FxChain::consumePendingSwap (int slot) noexcept
{
    // AT (or the MT inside prepare, with the callback stopped): claim the
    // published swap, then own pending_ exclusively. Pointer moves only -- no
    // construction, no destruction except the documented parking-full case.
    // The claim goes to kStageConsuming, NOT kStageEmpty: pending_ stays
    // AT-owned until the final store below, otherwise the MT could acquire
    // and fill pending_ while this thread is still between the CAS and its
    // move-out (the release-store of the CAS only orders prior writes).
    int expected = kStageStaged;
    if (! stageState_[(size_t) slot].compare_exchange_strong (
            expected, kStageConsuming, std::memory_order_acq_rel, std::memory_order_relaxed))
        return;

    // Park the displaced processor (ownership passes to the MT reaper).
    parkRetiredProcessor (slots_[(size_t) slot].release());
    slots_[(size_t) slot] = std::move (pending_[(size_t) slot]);   // may be null (None)

    auto tv = pendingType_[(size_t) slot];
    if (slots_[(size_t) slot] == nullptr && tv != static_cast<uint8_t> (FxType::None))
        tv = static_cast<uint8_t> (FxType::None);   // factory refused (no case hits this today)
    slotType_[(size_t) slot] = tv;

    // The freshly-created effect starts from zero state; force its wet fade to
    // 0 so it FADES IN (B4) instead of slamming in at full wet. (The old
    // processor's tail is unavoidably lost on a module change -- the user-
    // accepted "module turned off" exception; only the new module's engage
    // must be click-free.)
    wetFade_[(size_t) slot] = 0.0f;
    clearDelayRings();   // N3: latency may have changed -- flush stale ring history

    // pending_ is free ONLY now (release orders the move-out above before
    // this store, so an MT that observes Empty cannot be racing our reads).
    stageState_[(size_t) slot].store (kStageEmpty, std::memory_order_release);
}

void FxChain::parkRetiredProcessor (FxProcessor* old) noexcept
{
    if (old == nullptr)
        return;
    for (auto& r : retired_)
    {
        FxProcessor* expected = nullptr;
        if (r.compare_exchange_strong (expected, old, std::memory_order_release,
                                       std::memory_order_relaxed))
        {
            retiredDirty_.store (true, std::memory_order_release);
            return;
        }
    }
    // Parking full (4 swaps inside one 60 Hz reaper interval -- practically
    // unreachable; even the concurrency chaos test changes at most ~3 slot
    // types per tick). Free here as the documented fallback.
    delete old;
}

void FxChain::reapRetired() noexcept
{
    // MT: free everything the AT parked. The exchange claims each pointer
    // atomically, so a concurrent park on the same slot resolves ownership to
    // exactly one side (we either get the pointer and delete it here, or the
    // AT parks it again for the next pass).
    if (! retiredDirty_.exchange (false, std::memory_order_acq_rel))
        return;
    for (auto& r : retired_)
        delete r.exchange (nullptr, std::memory_order_acq_rel);
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
    clearDelayRings();   // N3: routing changed — stale ring history is meaningless
}

void FxChain::setOrder (const std::array<int, 3>& ord) noexcept
{
    order_ = ord;
    clearDelayRings();   // N3: order changed — stale ring history is meaningless
}

void FxChain::setTempo (double bpm, bool isPlaying) noexcept
{
    for (auto& s : slots_)
        if (s)
            s->setTransport (bpm, isPlaying);
}

void FxChain::setMasterMix (float g01) noexcept
{
    masterMix_ = juce::jlimit (0.0f, 1.0f, g01);
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

void FxChain::blendSlotWetFade (float* outL, float* outR, int numSamples, int s) noexcept
{
    // Per-sample dry/wet blend + two one-pole advances for a single series-style
    // slot. dryWet_[s] is the block-constant target; the per-sample-smoothed
    // dryWetCur_[s] AND the wet fade (wetFade_[s]) are each read + advanced one
    // sample per iteration and persisted back, so both are continuous across
    // blocks (B2-engine smoothing + the Step-1 wet fade). The pre-process dry
    // snapshot lives in dryL_/dryR_ (captured by the caller before the in-place
    // process() overwrote outL/outR with the wet).
    //
    // D1: latency compensation. The wet (outL[i]) lags the dry by the slot's
    // processing latency L (the 6x OS group delay, 8 for fold/ring-mod, 0 else).
    // The dry is delayed by L through a small per-slot ring so dry and wet are
    // sample-aligned -> kills the fs/16 comb at dw=0.5. L==0 is the direct
    // (bit-identical) path.
    const int L = (slots_[(size_t) s] != nullptr) ? slots_[(size_t) s]->latency() : 0;
    const float dwTarget = dryWet_[(size_t) s];
    float dwCur = dryWetCur_[(size_t) s];
    const float target = enabled_[(size_t) s] ? 1.0f : 0.0f;
    float fade = wetFade_[(size_t) s];
    int pos = dryDelayPos_[(size_t) s];
    for (int i = 0; i < numSamples; ++i)
    {
        // Two independent per-sample one-poles in the same loop: the dry/wet
        // smoother (smoothCoef_) advances dwCur toward dryWet_[s] (target), and
        // the wet fade (coefIn_/coefOut_) advances fade toward 1/0. dw = dwCur*fade.
        const float dw = dwCur * fade;
        const float dry = 1.0f - dw;
        float dL, dR;
        if (L > 0)
        {
            const int rp = pos - L;   // the dry sample written L steps ago
            const size_t ridx = (size_t) (rp >= 0 ? rp : rp + kDelayCap);
            dL = dryDelayL_[(size_t) s][ridx];
            dR = dryDelayR_[(size_t) s][ridx];
            dryDelayL_[(size_t) s][(size_t) pos] = dryL_[(size_t) i];
            dryDelayR_[(size_t) s][(size_t) pos] = dryR_[(size_t) i];
            pos = (pos + 1 >= kDelayCap) ? 0 : pos + 1;
        }
        else
        {
            dL = dryL_[(size_t) i];
            dR = dryR_[(size_t) i];
        }
        outL[i] = dL * dry + outL[i] * dw;
        outR[i] = dR * dry + outR[i] * dw;
        dwCur += (dwTarget - dwCur) * smoothCoef_;
        const float c = (target > fade) ? coefIn_ : coefOut_;
        fade += (target - fade) * c;
    }
    wetFade_[(size_t) s] = fade;
    dryWetCur_[(size_t) s] = dwCur;
    dryDelayPos_[(size_t) s] = pos;
}

void FxChain::delayPassthrough (float* outL, float* outR, int numSamples, int s) noexcept
{
    // N2: impose the slot's latency on its bypass/passthrough output by delaying
    // the running signal through the per-slot dryDelay ring (same read-before-
    // write pattern as blendSlotWetFade). This makes the slot's output latency
    // constant = latency() whether active or bypassed, so toggling enable does
    // NOT snap the dry from dry[i-L] to dry[i] when the tail fades out.
    const int L = slots_[(size_t) s] ? slots_[(size_t) s]->latency() : 0;
    if (L <= 0) return;   // L==0: true passthrough, no delay
    int pos = dryDelayPos_[(size_t) s];
    for (int i = 0; i < numSamples; ++i)
    {
        const int rp = pos - L;
        const size_t ridx = (size_t) (rp >= 0 ? rp : rp + kDelayCap);
        const float dL = dryDelayL_[(size_t) s][ridx];
        const float dR = dryDelayR_[(size_t) s][ridx];
        dryDelayL_[(size_t) s][(size_t) pos] = outL[i];
        dryDelayR_[(size_t) s][(size_t) pos] = outR[i];
        outL[i] = dL;
        outR[i] = dR;
        pos = (pos + 1 >= kDelayCap) ? 0 : pos + 1;
    }
    dryDelayPos_[(size_t) s] = pos;
}

void FxChain::clearDelayRings() noexcept
{
    // N3: zero all delay-ring history + positions so a mid-session topology /
    // order / type change does not replay stale old-routing audio.
    for (int s = 0; s < kNumFxSlots; ++s)
    {
        dryDelayL_[(size_t) s].fill (0.0f);
        dryDelayR_[(size_t) s].fill (0.0f);
        wetDelayL_[(size_t) s].fill (0.0f);
        wetDelayR_[(size_t) s].fill (0.0f);
        dryDelayPos_[(size_t) s] = 0;
        wetDelayPos_[(size_t) s] = 0;
    }
    parDryL_.fill (0.0f);
    parDryR_.fill (0.0f);
    parDryPos_ = 0;
    masterDryL_.fill (0.0f);
    masterDryR_.fill (0.0f);
    masterDryPos_ = 0;
}

void FxChain::updateEqCoeffs() noexcept
{
    eqActive_ = (eqLowV_ != 0) || (eqMidV_ != 64) || (eqHighV_ != 64);

    // RBJ audio-EQ cookbook coefficients (a0 normalised out), computed for both
    // channels. Flat bands are NOT processed (see applyMasterEq) so the EQ is a
    // bit-identical no-op at the defaults (low=0, mid=64, high=64).
    // BAND-CENTER CLAMP: RBJ coefficients leave the unit circle once w0 > pi
    // (sin(w0) < 0 -> negative alpha), i.e. a center above rate/2 — reachable
    // at exotic low host rates (the 5 kHz shelf goes unstable below 10 kHz,
    // the 1 kHz mid below ~2 kHz). Clamp every center to 0.45*rate (below
    // Nyquist with margin), mirroring the FV-1 RateBridge BW clamp
    // (Fv1Engine.h: min(15k, 0.49*hostRate)). No-op at any sane rate.
    const double r = rate_ > 0.0 ? rate_ : 44100.0;
    constexpr double kTwoPi = 6.28318530717958647692;
    const double maxEqFreq = 0.45 * r;

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
        const double freq = juce::jmin (20.0 * std::pow (1500.0 / 20.0, t), maxEqFreq);
        const double w0 = kTwoPi * freq / r, cw = std::cos (w0), sw = std::sin (w0);
        const double alpha = sw / (2.0 * 0.70710678);
        for (auto& b : eqLow_)
            assign (b, (1.0 + cw) * 0.5, -(1.0 + cw), (1.0 + cw) * 0.5,
                    1.0 + alpha, -2.0 * cw, 1.0 - alpha);
    }

    // Mid: peaking at 1 kHz, Q=1, gain (eqMid-64)/64 * +/-12 dB.
    {
        const double w0 = kTwoPi * juce::jmin (1000.0, maxEqFreq) / r, cw = std::cos (w0), sw = std::sin (w0);
        const double gainDB = ((double) eqMidV_ - 64.0) / 64.0 * 12.0;
        const double A = std::pow (10.0, gainDB / 40.0);
        const double alpha = sw / 2.0;   // Q=1
        for (auto& b : eqMid_)
            assign (b, 1.0 + alpha * A, -2.0 * cw, 1.0 - alpha * A,
                    1.0 + alpha / A, -2.0 * cw, 1.0 - alpha / A);
    }

    // High: shelf at 5 kHz, gain (eqHigh-64)/64 * +/-12 dB (slope S=1).
    {
        const double w0 = kTwoPi * juce::jmin (5000.0, maxEqFreq) / r, cw = std::cos (w0), sw = std::sin (w0);
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

int FxChain::latency() const noexcept
{
    // N1: the chain's OUTPUT latency = the accumulated slot latency through the
    // current topology, using each non-None slot's latency() (which ignores
    // enabled_). This is INVARIANT across enable/bypass (only changes on type
    // change), so the masterMix dry-delay never snaps.
    auto slotL = [this] (int s) -> int {
        const auto idx = static_cast<size_t> (s);
        // A staged (pending) swap wins: latency() is a PLANNING query (the
        // dry-delay rings + PDC), and the staged type is what the next block
        // will run — reporting the old slot's latency after a setSlotType
        // would leave the alignment one install behind (the param-coverage
        // test reads it pre-render).
        // F-eng-3: the staged value is read from the pendingLatency_ SNAPSHOT
        // (never by dereferencing pending_, which the MT may be destroying on
        // the take-back path while this runs on the audio thread). -1 encodes
        // a staged None / factory refusal, which falls through to slots_.
        if (stageState_[idx].load (std::memory_order_acquire) == kStageStaged)
        {
            const int pl = pendingLatency_[idx].load (std::memory_order_acquire);
            if (pl >= 0)
                return pl;
        }
        if (slots_[idx] != nullptr
            && slotType_[idx] != static_cast<uint8_t> (FxType::None))
            return slots_[idx]->latency();
        return 0;
    };
    const int A = order_[0], B = order_[1], C = order_[2];
    if (topology_ == static_cast<uint8_t> (FxTopology::Series))
        return slotL (A) + slotL (B) + slotL (C);
    if (topology_ == static_cast<uint8_t> (FxTopology::Parallel12to3))
        return juce::jmax (slotL (A), slotL (B)) + slotL (C);
    return slotL (A) + juce::jmax (slotL (B), slotL (C));   // Parallel1to23
}

void FxChain::process (const float* inL, const float* inR,
                       float* outL, float* outR, int numSamples)
{
    ++processCallCountForTest_;

    // Install any staged type swaps BEFORE the first slot read (latency(),
    // anyEnabled(), the renders). This is the AT's install point in the
    // engine path (renderPartFx also services explicitly before its fxDirty_
    // block; the second consume is an idempotent no-op) and it is what makes
    // DIRECT single-threaded use (tests: setSlotType -> process) observe the
    // staged type on the very next process, exactly like the old inline
    // build. Pointer moves only.
    servicePendingTypeSwaps();

    // ---- Block-size overflow guard (FX audit F3/F6) ----
    // Every scratch this chain touches is sized from prepare()'s maxBlock_:
    // dryL_/dryR_/wetL_/wetR_ hold exactly maxBlock_ samples, and every slot
    // processor's internal scratch follows the same budget (worst case
    // FxWavefolder's 6x-oversampled osL_/osR_, sized maxBlock_*6+8, into which
    // its SRC writes numSamples*6 floats). A host that renders a block LARGER
    // than the one it prepared (buffer-size transitions, offline/freeze
    // renders, some AU/AUv3 hosts -- JUCE does not universally guarantee
    // numSamples <= samplesPerBlock) would overrun all of them: a 6x-amplified
    // heap OOB WRITE. Clamp here and degrade to rendering the first maxBlock_
    // samples (a truncated block with an untouched tail) instead of smashing
    // the heap. renderParallel/blendSlotWetFade/delayPassthrough all receive
    // this already-clamped count from below, so they need no guard of their
    // own; PluginProcessor::processBlock clamps first, making this the second
    // (chain-local) layer.
    if (numSamples > maxBlock_)
    {
        jassertfalse;   // host block > prepared maxBlock_ -- contract violation
        numSamples = maxBlock_;
    }

    // Effect params are passed RAW to each processor (no per-block smoothing) —
    // see FxChain.h: a block-rate one-pole would SLEW audio-rate FX-param
    // modulation, which the FX mod matrix now delivers at the ~980 Hz internal-
    // block cadence. Raw at that cadence matches the synth voice path and does
    // not zipper for continuous modulation.

    // Tail fades are advanced per sample inside the blend loops below (one
    // advance per rendered slot per block), not here. A slot that just got
    // disabled is still slotActive (its wetFade_ is > epsilon) so its blend
    // loop keeps running and decays the fade toward 0; once the fade drops
    // below epsilon the slot stops rendering entirely.
    const bool anyAct = slotActive (0) || slotActive (1) || slotActive (2);
    const int Lc = latency();   // N1: chain output latency (constant across bypass)

    // Fast bypass: nothing active AND no chain latency AND master at no-op AND
    // no EQ => transparent dry copy. When latency()>0 (OS slots present but
    // bypassed) we fall through so delayPassthrough imposes the latency (N2).
    if (! anyAct && Lc == 0 && masterMix_ >= 1.0f && ! eqActive_)
    {
        juce::FloatVectorOperations::copy (outL, inL, numSamples);
        juce::FloatVectorOperations::copy (outR, inR, numSamples);
        return;
    }

    // ---- Render the chain (topology) into outL/outR ----
    // The topology ALWAYS runs (even when all slots bypassed): each non-None
    // bypassed slot imposes its latency via delayPassthrough, so the output is
    // Lc behind the input regardless of which slots are active (N2).
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
            if (! proc)
                continue;   // None type: true passthrough (L=0)
            if (! slotActive (s))
            {
                delayPassthrough (outL, outR, numSamples, s);   // N2: impose L on bypass
                continue;
            }

            // Snapshot the pre-process (dry) signal.
            juce::FloatVectorOperations::copy (dryL_.data(), outL, numSamples);
            juce::FloatVectorOperations::copy (dryR_.data(), outR, numSamples);

            proc->setParams (params_[(size_t) s].data());
            proc->process (outL, outR, numSamples);   // outL/outR now hold WET

            // Per-sample dry/wet blend + one-pole fade advance for this slot.
            blendSlotWetFade (outL, outR, numSamples, s);
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

            // Per-sample dry/wet blend + one-pole fade advance for slot C.
            blendSlotWetFade (outL, outR, numSamples, C);
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

            // Per-sample dry/wet blend + one-pole fade advance for slot A.
            blendSlotWetFade (outL, outR, numSamples, A);
        }

        // Stage 2 (parallel {B,C} over stage1). Snapshot stage1 into dryL_/dryR_
        // (the parallel input reference), then blend into outL/outR.
        juce::FloatVectorOperations::copy (dryL_.data(), outL, numSamples);
        juce::FloatVectorOperations::copy (dryR_.data(), outR, numSamples);

        renderParallel (dryL_.data(), dryR_.data(), outL, outR, numSamples, B, C);
    }

    // ---- Global wet/dry mix (per-sample one-pole toward masterMix_; B6) ----
    // Runs whenever the target is below unity OR the smoothed current has not
    // yet settled at unity, so a knob move / automation / FX-mod master-mix
    // destination ramps continuously instead of stepping once per block. At
    // steady-state unity (target AND current == 1.0) the blend is an exact
    // no-op and is skipped. The fast-bypass dry-copy path at the top of
    // process() stays keyed on the TARGET masterMix_ >= 1.0f, which is what
    // makes the master blend a no-op there.
    if (masterMix_ < 1.0f || masterMixCur_ < 1.0f)
    {
        // N1: delay the master dry (chain input) by Lc so it aligns with the
        // (latency-delayed) chain output -> no fs/16 comb at masterMix<1.
        const float target = masterMix_;
        float g = masterMixCur_;
        int mpos = masterDryPos_;
        for (int i = 0; i < numSamples; ++i)
        {
            const float dry = 1.0f - g;
            float mdL, mdR;
            if (Lc > 0)
            {
                const int rp = mpos - Lc;
                const size_t ridx = (size_t) (rp >= 0 ? rp : rp + kChainDelayCap);
                mdL = masterDryL_[ridx];
                mdR = masterDryR_[ridx];
                masterDryL_[(size_t) mpos] = inL[i];
                masterDryR_[(size_t) mpos] = inR[i];
                mpos = (mpos + 1 >= kChainDelayCap) ? 0 : mpos + 1;
            }
            else
            {
                mdL = inL[i];
                mdR = inR[i];
            }
            outL[i] = mdL * dry + outL[i] * g;
            outR[i] = mdR * dry + outR[i] * g;
            g += (target - g) * smoothCoef_;
        }
        masterMixCur_ = g;
        masterDryPos_ = mpos;
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
    // by the per-sample mean dry/wet W (out = dry*(1-W) + (sum wet)/activeCount
    // * W). Each active slot's one-pole wetFade_ is advanced per sample within
    // the blend loop and persisted back so the fade is continuous across blocks.
    // outL/outR are CLEARED first; with BOTH slots disabled/None the input is
    // copied through unchanged. Allocation-free (uses pre-sized wetL_/wetR_).
    //
    // D2: latency compensation. Each active slot's wet is aligned to the pair's
    // MAX latency (delay the shorter-latency slot by Lmax - itsL) before the
    // equal-gain sum, and the parallel dry is delayed by Lmax so the summed wet
    // and the dry are sample-aligned -> kills the OS-vs-non-OS comb (and the
    // dry-vs-wet comb). Lmax==0 (both slots latency 0) is the direct,
    // bit-identical path (rings untouched).
    juce::FloatVectorOperations::clear (outL, numSamples);
    juce::FloatVectorOperations::clear (outR, numSamples);

    const int pair[2] = { slotA, slotB };
    int activeCount = 0;
    int actSlot[2]       = { 0, 0 };
    int actL[2]          = { 0, 0 };       // per-active-slot latency
    int actWpos[2]       = { 0, 0 };       // per-active-slot wet-delay write pos
    float actDwTarget[2] = { 0.0f, 0.0f };
    float actDwCur[2]    = { 0.0f, 0.0f };
    float actFade[2]     = { 0.0f, 0.0f };

    // N2: Lmax from BOTH slots (constant, not active-dependent) so the parallel
    // stage output latency is invariant across enable/bypass.
    int Lmax = 0;
    for (int p = 0; p < 2; ++p)
    {
        const int s = pair[p];
        if (slots_[(size_t) s] && slotType_[(size_t) s] != static_cast<uint8_t> (FxType::None))
            Lmax = juce::jmax (Lmax, slots_[(size_t) s]->latency());
    }

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

        actSlot[activeCount]      = s;
        actL[activeCount]         = proc->latency();
        actWpos[activeCount]      = wetDelayPos_[(size_t) s];
        actDwTarget[activeCount]  = dryWet_[(size_t) s];
        actDwCur[activeCount]     = dryWetCur_[(size_t) s];
        actFade[activeCount]      = wetFade_[(size_t) s];
        ++activeCount;
    }

    if (activeCount > 0)
    {
        const float inv = 1.0f / (float) activeCount;
        int dpos = parDryPos_;
        for (int i = 0; i < numSamples; ++i)
        {
            // Per-sample mean dry/wet from each active slot's one-pole fade AND
            // one-pole dry/wet smoother (dw = dwCur * fade per slot).
            float dwSum = 0.0f;
            for (int a = 0; a < activeCount; ++a)
                dwSum += actDwCur[a] * actFade[a];
            const float W   = dwSum * inv;   // mean dry/wet, 0..1
            const float dry = 1.0f - W;

            // Sum the active wets, each delayed to Lmax so differing-latency
            // slots are sample-aligned before the equal-gain sum.
            float sumWL = 0.0f, sumWR = 0.0f;
            for (int a = 0; a < activeCount; ++a)
            {
                const int s = actSlot[a];
                const int extra = Lmax - actL[a];   // 0..Lmax
                float wl, wr;
                if (extra > 0)
                {
                    const int rp = actWpos[a] - extra;
                    const size_t ridx = (size_t) (rp >= 0 ? rp : rp + kDelayCap);
                    wl = wetDelayL_[(size_t) s][ridx];
                    wr = wetDelayR_[(size_t) s][ridx];
                    wetDelayL_[(size_t) s][(size_t) actWpos[a]] = wetL_[(size_t) s][(size_t) i];
                    wetDelayR_[(size_t) s][(size_t) actWpos[a]] = wetR_[(size_t) s][(size_t) i];
                    actWpos[a] = (actWpos[a] + 1 >= kDelayCap) ? 0 : actWpos[a] + 1;
                }
                else
                {
                    wl = wetL_[(size_t) s][(size_t) i];
                    wr = wetR_[(size_t) s][(size_t) i];
                }
                sumWL += wl;
                sumWR += wr;
            }

            // Delay the parallel dry by Lmax so it aligns with the Lmax-aligned
            // wet sum (kills the dry-vs-wet comb).
            float dL, dR;
            if (Lmax > 0)
            {
                const int rp = dpos - Lmax;
                const size_t ridx = (size_t) (rp >= 0 ? rp : rp + kDelayCap);
                dL = parDryL_[ridx];
                dR = parDryR_[ridx];
                parDryL_[(size_t) dpos] = inL[i];
                parDryR_[(size_t) dpos] = inR[i];
                dpos = (dpos + 1 >= kDelayCap) ? 0 : dpos + 1;
            }
            else
            {
                dL = inL[i];
                dR = inR[i];
            }

            // outL/outR currently hold the summed wet outputs of the active
            // slots; blend the mean wet against the dry input.
            outL[i] = dL * dry + sumWL * inv * W;
            outR[i] = dR * dry + sumWR * inv * W;
            // Advance each active slot's two per-sample one-poles: the dry/wet
            // smoother (smoothCoef_) toward dryWet_[s] (target), and the wet
            // fade (coefIn_/coefOut_) toward 1/0 (1 if enabled, else 0).
            for (int a = 0; a < activeCount; ++a)
            {
                actDwCur[a] += (actDwTarget[a] - actDwCur[a]) * smoothCoef_;
                const int s = actSlot[a];
                const float target = enabled_[(size_t) s] ? 1.0f : 0.0f;
                const float c = (target > actFade[a]) ? coefIn_ : coefOut_;
                actFade[a] += (target - actFade[a]) * c;
            }
        }
        // Persist the advanced fades + dry/wet currents + wet/parDry delay positions
        // so they are continuous across blocks.
        for (int a = 0; a < activeCount; ++a)
        {
            wetFade_[(size_t) actSlot[a]]      = actFade[a];
            dryWetCur_[(size_t) actSlot[a]]    = actDwCur[a];
            wetDelayPos_[(size_t) actSlot[a]]  = actWpos[a];
        }
        parDryPos_ = dpos;
    }
    else
    {
        // Both slots disabled/None: impose Lmax latency (N2) so the stage
        // output latency is constant, or transparent copy when Lmax==0.
        if (Lmax > 0)
        {
            int dpos = parDryPos_;
            for (int i = 0; i < numSamples; ++i)
            {
                const int rp = dpos - Lmax;
                const size_t ridx = (size_t) (rp >= 0 ? rp : rp + kDelayCap);
                const float dL = parDryL_[ridx];
                const float dR = parDryR_[ridx];
                parDryL_[(size_t) dpos] = inL[i];
                parDryR_[(size_t) dpos] = inR[i];
                outL[i] = dL;
                outR[i] = dR;
                dpos = (dpos + 1 >= kDelayCap) ? 0 : dpos + 1;
            }
            parDryPos_ = dpos;
        }
        else
        {
            juce::FloatVectorOperations::copy (outL, inL, numSamples);
            juce::FloatVectorOperations::copy (outR, inR, numSamples);
        }
    }
}

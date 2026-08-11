// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// FxProcessor — abstract base for one per-part FX-slot effect instance. The
// concrete effects live in FxProcessors.h/.cpp. Each effect renders
// an in-place STEREO block (L/R interleaved at block granularity); the per-slot
// dry/wet blend + topology routing is applied by FxChain, NOT here, so an effect
// only produces its "wet" output.
//
// All effects run at the HOST sample rate (the FX stage is post-render, fed by
// the host-rate voicecard buffers) and must never allocate on the audio thread:
// prepare() reserves everything, process() is allocation-free.
//
// `FxType` is defined in dsp/fx/FxTypes.h (a dependency-free shard), NOT here,
// to avoid a circular include: SynthEngine.h includes FxChain.h -> FxProcessor.h.
// FxProcessors.cpp includes SynthEngine.h for the engine integration but the
// FxType enumerators themselves come from FxTypes.h.

#pragma once

#include <memory>

#include "dsp/fx/FxTypes.h"   // FxType (dependency-free shard)

// One effect instance per FX slot. process() operates in-place on the stereo
// pair; the chain blends dry/wet around it.
class FxProcessor
{
public:
    virtual ~FxProcessor() = default;

    // Reserve internal buffers/state for up to maxBlock stereo samples at rate.
    // Idempotent; safe to call on a sample-rate / block-size change.
    virtual void prepare (double sampleRate, int maxBlock) = 0;

    // Clear internal state (delay lines, reverb tank, etc.). Called once after
    // prepare and whenever the slot is freshly (re)built.
    virtual void reset() = 0;

    // In-place stereo block. Reads/writes L[numSamples] and R[numSamples] as the
    // effect's wet output. Dry/wet is NOT applied here (the chain does it).
    virtual void process (float* L, float* R, int numSamples) = 0;

    // Map the five generic 0..1 slot params (param[0..4]) to this effect's
    // controls. Called single-threaded on the audio thread when the FX state is
    // serviced (fxDirty_), before process().
    virtual void setParams (const float param[5]) = 0;

    // Processing latency this effect introduces (in BASE-rate samples), so the
    // chain can delay-compensate its dry/wet + parallel blends. 0 for all
    // effects except the oversampled ones (Wavefolder/RingModulator report the
    // 6x SRC group delay). MUSICAL delays (a reverb's pre-delay, a delay's
    // time) are NOT reported here — only uncompensated PROCESSING latency.
    virtual int latency() const noexcept { return 0; }

    virtual FxType type() const = 0;
};

// Factory: build a fresh effect instance for the given type, or nullptr for
// FxType::None (the chain treats a None slot as passthrough regardless).
std::unique_ptr<FxProcessor> createFxProcessor (FxType t);

// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Fv1FxProcessor — abstract base for the FV-1 hardware-emulation FX family
// (Clocked Delay / Ensemble / Plate Reverb / Vinyl Compressor / Phaser). It owns
// the RateBridge (host<->32.768 kHz + steep 15 kHz BW-limiting) and drives the
// per-internal-sample 24-bit fixed-point core implemented by each subclass.
//
// Subclasses implement ONLY:
//   * processSampleFx(lin, rin, lout, rout)  — one internal (32.768 kHz) sample,
//     pure 24-bit fixed-point (convert float<->Q.23 at the boundary, here).
//   * prepareInternal(sampleRate, maxBlock)  — reserve fixed-point state (optional).
//   * resetInternal()                        — clear fixed-point state (optional).
//   * setParams(const float[5]) / type()     — as for every FxProcessor.
//
// The float<->Q.23 conversion happens at the bridge boundary so the entire audio
// path inside processSampleFx is fixed-point with saturation, exactly as on the
// FV-1. Dry/wet + topology routing are still the chain's job (this only emits
// the wet signal). latency() is 0 (no uncompensated processing latency; musical
// delays are NOT reported).

#ifndef PARVATI_DSP_FX_FV1_FV1FXPROCESSOR_H
#define PARVATI_DSP_FX_FV1_FV1FXPROCESSOR_H

#include <cstdint>

#include "dsp/fx/FxProcessor.h"
#include "dsp/fx/fv1/Fv1Engine.h"

namespace parvati::fv1
{

class Fv1FxProcessor : public FxProcessor
{
public:
    void prepare (double sampleRate, int maxBlock) override
    {
        bridge_.prepare (sampleRate, maxBlock);
        prepareInternal (sampleRate, maxBlock);
    }

    void reset() override
    {
        bridge_.reset();
        resetInternal();
    }

    void process (float* L, float* R, int numSamples) override
    {
        const int m = bridge_.hostToInternal (L, R, numSamples);
        float* il = bridge_.internalL();
        float* ir = bridge_.internalR();
        for (int i = 0; i < m; ++i)
        {
            const int32_t lin = f24_fromFloat (il[i]);
            const int32_t rin = f24_fromFloat (ir[i]);
            int32_t lo = 0, ro = 0;
            processSampleFx (lin, rin, lo, ro);
            il[i] = f24_toFloat (lo);
            ir[i] = f24_toFloat (ro);
        }
        bridge_.internalToHost (L, R, numSamples);
    }

    int latency() const noexcept override { return 0; }

protected:
    // The fixed-point core for one 32.768 kHz sample. lin/rin are Q.23; write
    // the Q.23 wet outputs to lout/rout (saturating; the helpers clip).
    virtual void processSampleFx (int32_t lin, int32_t rin,
                                  int32_t& lout, int32_t& rout) = 0;

    // Optional fixed-point state reservation / clearing (default: nothing).
    virtual void prepareInternal (double /*sampleRate*/, int /*maxBlock*/) {}
    virtual void resetInternal() {}

    RateBridge& bridge() noexcept { return bridge_; }

private:
    RateBridge bridge_;
};

} // namespace parvati::fv1

#endif // PARVATI_DSP_FX_FV1_FV1FXPROCESSOR_H

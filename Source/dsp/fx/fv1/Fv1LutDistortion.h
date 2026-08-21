// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Fv1LutDistortion — the "super digital" wavetable distortion: Drive into ONE
// OF SIXTEEN 1024-entry weird-distortion wavetables (the FV-1 external-EEPROM
// table idiom taken seriously), a CLOCK JITTER stage (a shared-clock timing
// wobble on the input — both channels drift together, like a wobbling crystal),
// and a Tone LP. No bitcrushing (the Clocked Delay's Grit owns that).
//
// The 16 shapes (selected stepped by the Shape knob):
//   0 Clip    1 Soft    2 Tube    3 Wrap    4 OctUp   5 Fuzz    6 Square
//   7 Steps   8 SFold   9 Cheby2 10 Cheby3 11 Asym   12 Mirror 13 HGate
//  14 Crush4 15 Sparse
//
// Params (param[4] is UNUSED; Mix is the chain Dry/Wet):
//   * Drive  (p0): 1..8x pre-gain into the table index.
//   * Shape  (p1): 16 stepped waveshapes (x/16 of the knob).
//   * Jitter (p2): 0..±12 samples of band-limited clock wobble (0 = off, a
//                  fixed 1-sample read = inaudible).
//   * Tone   (p3): 700..15000 Hz post-LP.
//
// ANTI-CRACKLE OVERSAMPLING (2026-08-17): like Fv1Overdrive, the nonlinear
// table stage runs inside a 6x oversampled domain (the vendored Warps
// polyphase FIR the Wavefolder uses). Several of the 16 shapes are HARD
// nonlinearities (Clip/Wrap/Steps/HGate/Crush4) whose generated harmonics
// fold at the 32.768 kHz internal Nyquist into inharmonic crackle, worst on
// note attacks (envelope peak = deepest excursion). The Jitter stage and the
// shape-crossfade counter stay at the 1x internal rate (they are defined in
// internal samples); only the memoryless table evaluation runs at 6x, in the
// same Q.23 saturating fixed-point path.

#ifndef PARVATI_DSP_FX_FV1_FV1LUTDISTORTION_H
#define PARVATI_DSP_FX_FV1_FV1LUTDISTORTION_H

#include <cstdint>
#include <vector>

#include "dsp/fx/fv1/Fv1FxProcessor.h"
#include "warps/dsp/sample_rate_converter.h"   // 6x polyphase FIR (shaper OS)

namespace parvati::fv1
{

class Fv1LutDistortion : public Fv1FxProcessor
{
public:
    Fv1LutDistortion();

    void setParams (const float param[5]) override;
    void prepareInternal (double sampleRate, int maxBlock) override;
    void resetInternal() override;
    // 6x-oversampled table path (see the file header): 1x jitter pre-pass,
    // then upsample 6x, run the Q.23 table shaper per oversampled sample,
    // downsample, then Tone at 1x.
    void process (float* L, float* R, int numSamples) override;
    // 6x OS pair group delay (8 internal samples) scaled to HOST samples in
    // prepareInternal — same dry/wet comb-free contract as FxWavefolder /
    // FxRingModulator (and Fv1Overdrive). 0 before the first prepare.
    int latency() const noexcept override { return latencyHost_; }
    FxType type() const noexcept override { return FxType::LutDistortion; }

protected:
    // The PURE table shaper (Drive + Shape crossfade blend; no jitter/tone —
    // those run at 1x in process()). Used at the 6x rate by process().
    void processSampleFx (int32_t lin, int32_t rin, int32_t& lout, int32_t& rout) override;

private:
    static constexpr int kShapes    = 16;
    static constexpr int kTableSize = 1024;   // 10-bit index, domain [-4,4)

    // 16 x 1024 int16 Q.14 wavetables, built once in the ctor ("EEPROM").
    int16_t tables_[kShapes][kTableSize];

    // Clock jitter: short delay per channel read at a wobbled position; ONE
    // noise source smooths to one wobble shared by both channels (common clock).
    DelayLine<64> jitL_, jitR_;
    uint32_t lcg_   = 0u;
    float jitLp_    = 0.0f;     // one-pole-smoothed wobble
    float jitAmt_   = 0.0f;     // 0..12 samples
    float jitCoef_  = 0.25f;    // wobble smoothing coeff (band-limits the noise)

    OnePoleLpFx toneLp_;
    int16_t drive14_ = 4096;    // q14 fractional pre-gain
    int driveShift_  = 0;       // integer 2x stages (1..8x)
    const int16_t* shape_ = tables_[0];   // active (target) wavetable
    int shapeIdx_ = 0;                     // its index (periodicity lookup below)

    // OUT-OF-DOMAIN policy per shape (2026-08-21 — the "distortion dropouts"
    // fix): the table spans x in [-4,4); driven peaks past the rails must not
    // read a ZERO. Wrap (3) and SFold (8) wrap their input by construction —
    // period 2 in x = 256 entries — so wrapping the INDEX modulo 1024 is the
    // EXACT continuation of the curve (loud peaks fold over, the intended
    // weird-shaper character). Every other shape SATURATES at its edge entry
    // (clip/tube-family clamped tails). The old blanket clamp read the wrap
    // shapes' edge entries, which are ZERO (sin(±π) = 0): every out-of-domain
    // peak came out as literal silence — measured RMS collapse to 0.2-1.4% of
    // the running median mid-note (the "full voice dropouts and horrible
    // audio quality" report; the chain input is hotter than the main bus, so
    // even moderate drive trips it).
    static constexpr bool kShapeIsPeriodic[kShapes] = {
        false, false, false, true,  false, false, false, false,
        true,  false, false, false, false, false, false, false };

    // One-pole ~10 Hz high-pass DC blocker on the wet output: several shapes
    // (Cheby2/OctUp/Asym) are NOT re-referenced to 0 at x=0 and emit large
    // sustained DC at silence (Cheby2: -0.71, OctUp: -0.34 post the 0.75
    // table scaling); the Tone LP passes it, so it must be removed here.
    float dcX1_ = 0.0f, dcY1_ = 0.0f;

    // 6x OS group delay in HOST samples (captured in prepareInternal).
    int latencyHost_ = 0;

    // 6x oversampling of the nonlinear table stage (see the file header).
    warps::SampleRateConverter<warps::SRC_UP, 6, 48>   srcUpL_, srcUpR_;
    warps::SampleRateConverter<warps::SRC_DOWN, 6, 48> srcDownL_, srcDownR_;
    std::vector<float> osL_, osR_;   // 6x scratch (sized in prepareInternal)

    // The per-sample table evaluation (Drive -> index -> shape crossfade
    // blend). Shared by the 6x path; reads fade14_/fadeFrom_ but never
    // advances them (the 1x pre-pass in process() owns the fade clock).
    int32_t lutShape (int32_t x);

    // Click-free shape change: swapping the table pointer instantly jumps
    // between two transfer curves at the same sample value -> an audible
    // click. On a shape change setParams keeps the previous table in
    // fadeFrom_ and processSampleFx crossfades old->new over kShapeFade
    // internal samples (~3.9 ms) with a per-sample Q.14 fade counter:
    //   out = f24_addSat (f24_mulk (lutOld (x), q14 (1-f)),
    //                     f24_mulk (lutNew (x), q14 (f)))
    // fadeFrom_ == nullptr means "no fade in flight". Trivially lock-free
    // (two ints + two pointers; setParams runs on the audio thread).
    static constexpr int kShapeFade = 128;          // samples @ 32.768 kHz
    static constexpr int kFadeStep14 = 8191 / kShapeFade + 1;   // Q.14/sample
    const int16_t* fadeFrom_ = nullptr;             // previous table (or null)
    int16_t fade14_ = 8191;                          // Q.14 gain toward shape_
    bool shapeSet_ = false;                          // first setParams: no fade
};

} // namespace parvati::fv1

#endif // PARVATI_DSP_FX_FV1_FV1LUTDISTORTION_H

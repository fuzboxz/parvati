// Copyright (c) 2026 805Labs Kft. / Hellcat.
//
// FxProcessors — the per-slot FX effects: six ports of the Mutable Instruments
// Clouds DSP — Diffuser / Pitch Shifter / Reverb (the dsp/fx chain) and
// Looping Delay / WSOLA Stretch / Spectral (the buffer-based playback modes) —
// plus three ports of the Mutable Instruments Warps DSP — Wavefolder (memoryless
// waveshaper) / Frequency Shifter (quadrature Hilbert) / Ring Modulator (diode
// model). None is the no-op slot. Each maps the five generic 0..1 slot params to
// its own controls in setParams() and renders an in-place stereo wet block in
// process(). Dry/wet + topology routing live in FxChain.
//
// The Clouds modules run their vendored DSP at a fixed 32 kHz and resample at
// the FxProcessor boundary (HostRateBridge), so their tuning is bit-faithful to
// upstream at any host rate. The Warps modules run NATIVELY at the host rate (no
// bridge): the Wavefolder is memoryless, and the Frequency Shifter / Ring
// Modulator oscillators init at the host rate (the Hilbert allpass network is
// normalized-frequency, valid at any rate). All effects are allocation-free on
// the audio thread (prepare reserves; the clouds engines' fixed-size buffers and
// the warps per-block scratch live in the object).

#pragma once

#include <juce_dsp/juce_dsp.h>

#include "dsp/fx/FxProcessor.h"
#include "dsp/fx/HostRateBridge.h"

#include "clouds/dsp/fx/diffuser.h"
#include "clouds/dsp/fx/pitch_shifter.h"
#include "clouds/dsp/fx/reverb.h"
#include "clouds/dsp/looping_sample_player.h"
#include "clouds/dsp/pvoc/phase_vocoder.h"
#include "clouds/dsp/wsola_sample_player.h"
#include "warps/dsp/quadrature_oscillator.h"
#include "warps/dsp/quadrature_transform.h"
#include "warps/dsp/sample_rate_converter.h"   // 6x oversampling SRC (Wavefolder/RingMod anti-aliasing)
#include "rings/dsp/resonator.h"     // Rings modal resonator (Resonator FX)
#include "rings/dsp/limiter.h"        // Rings output limiter (bounds resonator build-up)

//==========================================================================
// Clouds FX modules — ports of the Mutable Instruments Clouds `dsp/fx` chain.
// Each owns its clouds engine + a fixed-size delay buffer (passed to Init) + a
// HostRateBridge. setParams() caches the 0..1 params; process() applies them to
// the engine (keeps the engines' internal smoothing advancing) then resamples.
//
// Diffuser — the Clouds AP diffusion network (FxEngine<2048, 32-bit float>).
// No user params: the internal amount is pinned full-wet (1.0). The wet/dry
// mix is the chain Dry/Wet (the old Amount knob was a duplicate of it).
class FxDiffuser : public FxProcessor
{
public:
    void prepare (double sampleRate, int maxBlock) override;
    void reset() override;
    void process (float* L, float* R, int numSamples) override;
    void setParams (const std::array<float, kNumFxSlotParams>& param) override;
    FxType type() const override;

private:
    clouds::Diffuser diffuser_;
    HostRateBridge   bridge_;
    float            diffuserBuffer_[2048] {};   // FxEngine<2048, FORMAT_32_BIT>
    float            amount_ = 0.0f;
};

// Pitch Shifter — the Clouds dual-tap pitch shifter (FxEngine<4096, 16-bit>).
// param0 = Pitch (Ratio 0..1 -> -12..+12 semitones, 0.5 = unison), param1 = Size,
// param2 = Spread (0..1 offsets the right-channel tap for stereo width).
class FxPitchShifter : public FxProcessor
{
public:
    void prepare (double sampleRate, int maxBlock) override;
    void reset() override;
    void process (float* L, float* R, int numSamples) override;
    void setParams (const std::array<float, kNumFxSlotParams>& param) override;
    FxType type() const override;

private:
    clouds::PitchShifter pitch_;
    HostRateBridge       bridge_;
    uint16_t             pitchBuffer_[4096] {};   // FxEngine<4096, FORMAT_16_BIT>
    float                ratioParam_ = 0.5f;       // cached 0..1
    float                sizeParam_  = 0.5f;
    float                spreadParam_ = 0.0f;      // 0..1 (R tap offset)
};

// Reverb — the Clouds Griesinger/Dattorro reverb (FxEngine<16384, 12-bit>).
// Signal path: Pre-Delay -> input diffusers (Diffusion) -> tank loop (Time) ->
// LP damping (Tone) -> post Low-Cut (HP) to remove low-frequency buildup. param0 = Pre-Delay
// (0..200 ms), param1 = Diffusion, param2 = Time, param3 = Tone (LP/damping),
// param4 = Low-Cut (post HP). The internal amount is pinned full-wet (1.0); the
// wet/dry mix is the chain Dry/Wet (the old Amount knob was a duplicate of it).
// input_gain is fixed internally (0.5) to prevent the L+R sum from clipping.
// Pre-Delay is a MUSICAL delay (not reported via latency()).
class FxReverb : public FxProcessor
{
public:
    void prepare (double sampleRate, int maxBlock) override;
    void reset() override;
    void process (float* L, float* R, int numSamples) override;
    void setParams (const std::array<float, kNumFxSlotParams>& param) override;
    FxType type() const override;

private:
    clouds::Reverb  reverb_;
    HostRateBridge  bridge_;
    uint16_t        reverbBuffer_[16384] {};   // FxEngine<16384, FORMAT_12_BIT>
    float           amount_    = 0.0f;
    float           timeParam_ = 0.0f;
    float           lpParam_   = 0.0f;
    float           diffParam_ = 0.0f;
    float           preDelayParam_ = 0.0f;   // 0..1 (-> 0..200 ms)
    float           lowCutParam_   = 0.0f;   // 0..1 (-> post HP cutoff)
    double          sampleRate_     = 44100.0;
    // Pre-delay ring (host rate): capacity = ceil(0.2*sr)+1, sized in prepare().
    std::vector<float> preDelayL_;
    std::vector<float> preDelayR_;
    int             preDelaySamples_ = 0;     // 0..(capacity-1), from preDelayParam_
    int             preDelayCap_     = 0;     // ring capacity (set in prepare)
    int             preDelayPos_     = 0;     // ring write position
    // Post low-cut one-pole HP stage (L/R).
    OnePoleTone     lowCut_;
};

//==========================================================================
// Clouds "mode" effects — ports of the Clouds playback modes (NOT synthesis:
// granular is excluded). Unlike the dsp/fx chain these are BUFFER-BASED: each
// records its dry input into a clouds::AudioBuffer (the Clouds "tape loop") and
// plays back from the recorded past, so they re-texture the sound constantly.
// They run the vendored DSP at the fixed 32 kHz (HostRateBridge) and chunk the
// internal block at <=32 samples (the players' per-call state is tuned for
// kMaxBlockSize=32). Dry/wet + topology stay the chain's job.
//
// Looping Delay — the Clouds looping sample player. Records stereo dry into a
// ~4 s AudioBuffer, then plays overlapping loops (Hermite-interpolated) from the
// recorded past; freeze holds the loop. Pure sample-by-sample (no background
// tick). param0 = Position, param1 = Size, param2 = Pitch (+/-24 st, 0.5 =
// unison), param3 = Freeze (>0.5 holds the loop).
class FxLoopingDelay : public FxProcessor
{
public:
    void prepare (double sampleRate, int maxBlock) override;
    void reset() override;
    void process (float* L, float* R, int numSamples) override;
    void setParams (const std::array<float, kNumFxSlotParams>& param) override;
    FxType type() const override;

private:
    // ~4 s of stereo capture at the 32 kHz internal rate. AudioBuffer::Init wants
    // size = usable + kInterpolationTail(8); the crossfade tail is kCrossFadeSize(256).
    static constexpr int kBufferSamples = 128000;

    clouds::AudioBuffer<clouds::RESOLUTION_16_BIT> buf_[2];
    clouds::LoopingSamplePlayer                     looper_;
    clouds::Parameters                              params_ {};
    HostRateBridge                                  bridge_;
    int16_t bufMem_[2][kBufferSamples + 8] {};   // usable + kInterpolationTail
    int16_t tailMem_[2][256] {};                // kCrossFadeSize

    float positionParam_ = 0.5f;
    float sizeParam_     = 0.5f;
    float pitchParam_    = 0.5f;   // 0.5 = unison
    float freezeParam_   = 0.0f;
};

// WSOLA Stretch — the Clouds WSOLA (waveform-similarity overlap-add) sample
// player: time/pitch manipulation of the recorded past by splicing at
// correlation-maximizing points. Records stereo dry into a ~4 s AudioBuffer
// (like the looper) and plays overlapping windows from the recorded past.
// REQUIRES a per-chunk "background tick": the firmware runs the correlator
// splice-point search in its background main loop, but Hellcat FX slots have no
// such thread, so after each Play we run it inline (LoadCorrelator +
// EvaluateSomeCandidates); without it the search stalls and WSOLA never
// advances. Signal path: Pitch -> Position -> Size -> Freeze -> Tone.
// param0 = Pitch (+/-24 st, 0.5 = unison), param1 = Position, param2 = Size,
// param3 = Freeze (>0.5 holds the recorded loop — the same WriteFade write-gate
// the looper uses), param4 = Tone (post one-pole LP).
class FxWSOLAStretch : public FxProcessor
{
public:
    void prepare (double sampleRate, int maxBlock) override;
    void reset() override;
    void process (float* L, float* R, int numSamples) override;
    void setParams (const std::array<float, kNumFxSlotParams>& param) override;
    FxType type() const override;

private:
    static constexpr int kBufferSamples   = 128000;
    // Correlator sign-bit scratch, sized as the firmware allocates it:
    // (kMaxWSOLASize/32 + 2) words, x3 (source | destination | unused tail).
    static constexpr int kCorrelatorWords = (4096 / 32) + 2;

    clouds::AudioBuffer<clouds::RESOLUTION_16_BIT> buf_[2];
    clouds::WSOLASamplePlayer                          ws_;
    clouds::Correlator                                 correlator_;
    clouds::Parameters                                 params_ {};
    HostRateBridge                                     bridge_;

    uint32_t corr_[kCorrelatorWords * 3] {};      // correlator source/destination
    int16_t  bufMem_[2][kBufferSamples + 8] {};   // usable + kInterpolationTail
    int16_t  tailMem_[2][256] {};                // kCrossFadeSize

    float pitchParam_    = 0.5f;   // 0.5 = unison
    float positionParam_ = 0.5f;
    float sizeParam_     = 0.5f;
    float freezeParam_   = 0.0f;   // >0.5 holds the recorded loop (no buffer write)
    float toneParam_     = 1.0f;   // 1 = bright (near-bypass); 0 = dark one-pole LP
    double sampleRate_   = 44100.0;
    OnePoleTone toneLp_;              // post one-pole LP stage (L/R)
};

// Spectral — the Clouds phase vocoder (STFT + overlap-add + spectral frame
// transformation). Unlike the looper/WSOLA it is NOT buffer-based: it processes
// the live signal in place through an FFT pipeline (analysis/synthesis are
// separate internal buffers, so in-place Process is safe). REQUIRES a per-chunk
// "background tick": the firmware drains the STFT pipeline in its background
// main loop, but Hellcat FX slots have no such thread, so after each Process we
// call Buffer() inline; without it the FFT frames never drain -> silence.
// freeze/gate are off and spectral quantization/phase-randomization are zero
// (Blur = spectral.refresh_rate). param0 = Pitch (+/-24 st, 0.5 = unison),
// param1 = Warp, param2 = Position, param3 = Blur (refresh_rate),
// param4 = Freeze (>0.5 holds the current spectral frame).
class FxSpectral : public FxProcessor
{
public:
    void prepare (double sampleRate, int maxBlock) override;
    void reset() override;
    void process (float* L, float* R, int numSamples) override;
    void setParams (const std::array<float, kNumFxSlotParams>& param) override;
    FxType type() const override;

private:
    // Per-channel workspace for PhaseVocoder's BufferAllocator. Each channel must
    // hold the FFT buffer, the analysis/synthesis buffer, and the texture memory
    // for the full kMaxNumTextures(7); the engine self-limits num_textures to
    // free space, so 128 KiB/ch comfortably yields all 7 textures.
    static constexpr int kPvocWorkspaceBytes = 131072;

    void initPvoc();

    clouds::PhaseVocoder pvoc_;
    clouds::Parameters   params_ {};
    HostRateBridge       bridge_;
    uint8_t              workspace_[2][kPvocWorkspaceBytes] {};

    float pitchParam_    = 0.5f;   // 0.5 = unison
    float warpParam_     = 0.5f;
    float positionParam_ = 0.5f;
    float blurParam_     = 0.5f;
    float freezeParam_   = 0.0f;   // >0.5 holds the current spectral frame
};

// Wavefolder — the Mutable Instruments Warps bipolar wavefolder (memoryless LUT
// waveshaper). Runs NATIVELY at the host base rate (no HostRateBridge) but wraps
// the fold in the Warps hardware's OWN 6x polyphase-FIR oversampling
// (SampleRateConverter<SRC_UP/DOWN,6,48>, kOversampling=6) so the sharp fold
// corners anti-alias exactly as the hardware does. Per channel: upsample 6x ->
// (Drive pre-gain x) fold each oversampled sample -> downsample 6x -> Tone LP.
// Signal path: Drive (pre-gain into the fold) -> Fold -> Bias -> Tone (post-fold
// one-pole LP that reduces the harsh upper harmonics the fold generates).
// param0 = Drive (1x..4x pre-gain), param1 = Fold (fold amount), param2 = Bias
// // (a small constant second input for an asymmetric fold), param3 = Tone
// (post-fold LP; 1 = bright/near-bypass). ~8 base samples of group delay from
// the SRC filters.
class FxWavefolder : public FxProcessor
{
public:
    void prepare (double sampleRate, int maxBlock) override;
    void reset() override;
    void process (float* L, float* R, int numSamples) override;
    void setParams (const std::array<float, kNumFxSlotParams>& param) override;
    int latency() const noexcept override;
    FxType type() const override;

private:
    warps::SampleRateConverter<warps::SRC_UP, 6, 48>   srcUp_[2];    // [0]=L, [1]=R
    warps::SampleRateConverter<warps::SRC_DOWN, 6, 48> srcDown_[2];
    std::vector<float> osL_;   // oversampled scratch (maxBlock*6 + headroom)
    std::vector<float> osR_;

    double sampleRate_  = 44100.0;   // for the post-fold Tone one-pole LP
    OnePoleTone toneLp_;             // post-fold one-pole LP stage (L/R)


    float driveParam_ = 0.0f;   // 0..1 (1x..4x pre-gain; 0 = unity = bit-identical)
    float foldParam_  = 0.0f;   // 0..1 (fold amount)
    float biasParam_  = 0.5f;   // 0..1 (0.5 = no bias / symmetric fold)
    float toneParam_  = 1.0f;   // 0..1 (1 = bright/near-bypass; 0 = dark LP)
};

// Frequency Shifter — the Warps quadrature (Hilbert) frequency shifter (the
// Warps hidden "cross-fade" algorithm). NATIVE host rate (the Hilbert allpass network
// is normalized-frequency, so its 90 deg band scales with the host rate; the
// carrier QuadratureOscillator inits at the host rate). True-stereo: each channel
// through its own QuadratureTransform, one shared carrier; the Spread knob
// blends the right channel toward the opposite sideband for width. Signal path:
// Shift (carrier freq) -> Shape (carrier wavetable timbre, sine->harmonics) ->
// Feedback (regen) -> Spread (R sideband blend).
// param0 = Shift (center 0.5 = 0 Hz; reuses the upstream frequency_shift_pot Hz
// formula), param1 = Shape (osc shape 0..1, fed straight to Render — 0 = sine,
// the original hardcoded carrier), param2 = Feedback, param3 = Spread
class FxFrequencyShifter : public FxProcessor
{
public:
    void prepare (double sampleRate, int maxBlock) override;
    void reset() override;
    void process (float* L, float* R, int numSamples) override;
    void setParams (const std::array<float, kNumFxSlotParams>& param) override;
    FxType type() const override;

private:
    warps::QuadratureOscillator osc_;
    warps::QuadratureTransform   qtL_;
    warps::QuadratureTransform   qtR_;

    std::vector<float> carrierI_;   // sine carrier I/Q (reserved in prepare)
    std::vector<float> carrierQ_;

    float feedbackL_ = 0.0f;        // smoothed shifted output (feedback path)
    float feedbackR_ = 0.0f;

    float shiftParam_    = 0.5f;    // 0..1 (0.5 = 0 Hz)
    float shapeParam_    = 0.0f;    // 0..1 (0 = sine = the original hardcoded carrier)
    float feedbackParam_ = 0.0f;    // 0..1
    float spreadParam_   = 0.0f;    // 0..1 (R sideband blend)
};

// Ring Modulator — the Warps analog (diode-model) ring modulator against an
// internal QuadratureOscillator carrier. NATIVE host base rate, but the diode
// product runs inside the Warps hardware's OWN 6x polyphase-FIR oversampling
// (SampleRateConverter<SRC_UP/DOWN,6,48>, kOversampling=6): BOTH the signal and
// the internal carrier are rendered at the base rate then upsampled 6x through
// src_up_, so the Diode() product (signal +/- carrier) happens entirely in the
// oversampled domain with the carrier band-limited to fs/2 and time-aligned with
// the signal (faithful to upstream src_up_[0]=carrier, src_up_[1]=modulator).
// Uses the Parker DAFx-11 diode model. param0 = Carrier (Hz), param1 = Shape
// (osc shape 0..1.9999: sine -> harmonics -> buzzy), param2 = Amount (ring
// intensity).
class FxRingModulator : public FxProcessor
{
public:
    void prepare (double sampleRate, int maxBlock) override;
    void reset() override;
    void process (float* L, float* R, int numSamples) override;
    void setParams (const std::array<float, kNumFxSlotParams>& param) override;
    int latency() const noexcept override;
    FxType type() const override;

private:
    warps::QuadratureOscillator                        carrier_;
    warps::SampleRateConverter<warps::SRC_UP, 6, 48>   srcUp_[2];        // [0]=L, [1]=R (signal)
    warps::SampleRateConverter<warps::SRC_UP, 6, 48>   srcUpCarrier_;    // mono (carrier) — D3
    warps::SampleRateConverter<warps::SRC_DOWN, 6, 48> srcDown_[2];

    std::vector<float> carrierBaseI_;   // carrier at the BASE rate (n) — rendered then upsampled (D3)
    std::vector<float> carrierBaseQ_;   // Q (Render writes both; unused by the diode, base-rate waste only — D5)
    std::vector<float> carrierOs_;      // carrier upsampled to the 6x rate (n*6)
    std::vector<float> osL_;            // oversampled signal scratch (maxBlock*6 + headroom)
    std::vector<float> osR_;

    float carrierParam_ = 0.0f;     // 0..1 (-> Hz)
    float shapeParam_   = 0.0f;     // 0..1 (-> osc shape)
    float amountParam_  = 0.5f;     // 0..1
};

// Resonator — the Mutable Instruments Rings modal resonator (a bank of up to
// 64 resonant band-pass SVFs tuned to harmonic/inharmonic partials). NATIVE host
// rate (the SVF coefficients are computed from the normalized frequency each
// block via set_f_q, so they track the host rate — no resampler/oversampling).
// Rings-faithful stereo: ONE resonator processes a mono sum (0.5*(L+R)) of the
// input; its two outputs — out (odd modes) and aux (even modes) — map to L and R
// (matching Rings' mono path: part.cc out->L, aux->R). The position parameter
// rebalances odd vs even modes, acting as both a timbral "pickup position"
// control and a stereo-width control. Structure is fixed at the Rings default
// (0.25, slightly inharmonic). The resonator's Process attenuates input by 0.125
// (-18 dB); the Rings output limiter (drive = 1.4, the modal model_gains_) bounds
// the sustained on-resonance build-up to ~0.8 peak (SoftLimit toward ~1.0),
// exactly as upstream Rings (part.cc applies limiter_.Process with
// model_gains_[MODAL]=1.4). param0 = Pitch (base pitch C1..C7), param1 = Decay
// (damping / ring time), param2 = Bright (brightness), param3 = Position
// (odd/even mode balance = pickup position + stereo width), param4 = Structure
// (inharmonicity / modal layout 0..1; 0.25 = the Rings default). Note: at Position
// ~=0.5 the even-mode (R) channel vanishes — the center-pluck node (a string
// picked at its centre excites only odd harmonics); textbook modal physics,
// identical to hardware Rings. The default 0.25 keeps both channels active.
// latency()==0 (LTI filter group delay is the effect's sound, not processing
// latency).
class FxResonator : public FxProcessor
{
public:
    void prepare (double sampleRate, int maxBlock) override;
    void reset() override;
    void process (float* L, float* R, int numSamples) override;
    void setParams (const std::array<float, kNumFxSlotParams>& param) override;
    FxType type() const override;

private:
    rings::Resonator res_;    // single modal resonator (mono in, out/aux stereo out)
    rings::Limiter  limiter_; // Rings output limiter (bounds resonant build-up)

    std::vector<float> inMono_;   // mono-sum excitation (reserved in prepare)
    std::vector<float> wetL_;     // out (odd modes) -> L  (reserved in prepare)
    std::vector<float> wetR_;     // aux (even modes) -> R (reserved in prepare)

    double sampleRate_ = 44100.0;

    float pitchParam_    = 0.5f;   // 0..1 (-> C1..C7)
    float decayParam_    = 0.3f;   // 0..1 (-> damping)
    float brightParam_   = 0.5f;   // 0..1 (-> brightness)
    float positionParam_ = 0.25f;   // 0..1 (-> odd/even mode balance; 0.5 = even-mode null)
    float structureParam_ = 0.25f;   // 0..1 (-> inharmonicity/modal layout; 0.25 = Rings default)
};

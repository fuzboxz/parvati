// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See FxProcessors.h.

#include "dsp/fx/FxProcessors.h"

#include "clouds/dsp/looping_sample_player.h"
#include "clouds/dsp/pvoc/phase_vocoder.h"
#include "warps/resources.h"        // warps::lut_bipolar_fold / lut_ap_poles (Wavefolder/FreqShifter)
#include "stmlib/dsp/units.h"       // stmlib::SemitonesToRatio (FreqShifter/Resonator Hz mapping)

#include "dsp/fx/fv1/Fv1ClockedDelay.h"     // FV-1 hardware-emulation family
#include "dsp/fx/fv1/Fv1Ensemble.h"
#include "dsp/fx/fv1/Fv1PlateReverb.h"
#include "dsp/fx/fv1/Fv1VinylCompressor.h"
#include "dsp/fx/fv1/Fv1Phaser.h"
#include "dsp/fx/fv1/Fv1Overdrive.h"
#include "dsp/fx/fv1/Fv1LutDistortion.h"
#include "dsp/fx/fv1/Fv1Compressor.h"
#include "dsp/fx/fv1/Fv1Gate.h"
#include "dsp/fx/fv1/Fv1Chorus.h"
#include "dsp/fx/fv1/Fv1Flanger.h"
#include "dsp/fx/fv1/Fv1Echo.h"
#include "dsp/fx/fv1/Fv1Room.h"
#include "dsp/fx/fv1/Fv1Spring.h"

#include "SynthEngine.h"   // FxType enumerators (factory switch)

//==========================================================================
// FxDiffuser — Clouds AP diffusion network (FxEngine<2048, 32-bit float>).
void FxDiffuser::prepare (double sampleRate, int maxBlock)
{
    diffuser_.Init (diffuserBuffer_);
    bridge_.prepare (sampleRate, maxBlock);
    diffuser_.set_amount (amount_);
}

void FxDiffuser::reset()
{
    bridge_.reset();
    diffuser_.Init (diffuserBuffer_);   // Diffuser exposes no Clear(); Init() zeroes the tank
    diffuser_.set_amount (amount_);
}

void FxDiffuser::process (float* L, float* R, int numSamples)
{
    diffuser_.set_amount (amount_);     // keep the knob live every block
    const int m = bridge_.hostToInternal (L, R, numSamples);
    diffuser_.Process (bridge_.internal(), static_cast<size_t> (m));
    bridge_.internalToHost (L, R, numSamples);
}

void FxDiffuser::setParams (const float /*param*/[5])
{
    // Amount is no longer a user param: the internal amount is pinned full-wet
    // (1.0) so the diffuser always emits a fully diffused signal. The chain
    // Dry/Wet is the sole wet/dry mix (it was a duplicate of this crossfade).
    amount_ = 1.0f;
}

FxType FxDiffuser::type() const { return FxType::Diffuser; }

//==========================================================================
// FxPitchShifter — Clouds dual-tap pitch shifter (FxEngine<4096, 16-bit>).
void FxPitchShifter::prepare (double sampleRate, int maxBlock)
{
    pitch_.Init (pitchBuffer_);
    bridge_.prepare (sampleRate, maxBlock);
}

void FxPitchShifter::reset()
{
    bridge_.reset();
    pitch_.Clear();
}

void FxPitchShifter::process (float* L, float* R, int numSamples)
{
    // Apply the cached 0..1 params every block. size_ is per-sample
    // rate-limited inside pitch_shifter.h (ONE_POLE per-sample, ~20 ms)
    // to eliminate read-position jumps under 980 Hz modulation.
    const float semis = (ratioParam_ - 0.5f) * 24.0f;             // -12..+12 st, 0.5 = unison
    pitch_.set_ratio (std::pow (2.0f, semis / 12.0f));
    pitch_.set_size (sizeParam_);
    pitch_.set_spread (spreadParam_);   // R-tap offset for stereo width (Parvati add)
    const int m = bridge_.hostToInternal (L, R, numSamples);
    pitch_.Process (bridge_.internal(), static_cast<size_t> (m));
    bridge_.internalToHost (L, R, numSamples);
}

void FxPitchShifter::setParams (const float param[5])
{
    ratioParam_  = juce::jlimit (0.0f, 1.0f, param[0]);
    sizeParam_   = juce::jlimit (0.0f, 1.0f, param[1]);
    spreadParam_ = juce::jlimit (0.0f, 1.0f, param[2]);
}

FxType FxPitchShifter::type() const { return FxType::PitchShifter; }

//==========================================================================
// FxReverb — Clouds Griesinger/Dattorro reverb (FxEngine<16384, 12-bit>).
void FxReverb::prepare (double sampleRate, int maxBlock)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    reverb_.Init (reverbBuffer_);
    bridge_.prepare (sampleRate, maxBlock);
    // Pre-delay ring (host rate): capacity for up to 200 ms — a MUSICAL delay,
    // not reported via latency(). +1 so a delay of cap-1 still reads in range.
    preDelayCap_ = static_cast<int> (std::ceil (0.20 * sampleRate_)) + 1;
    preDelayL_.assign (static_cast<size_t> (preDelayCap_), 0.0f);
    preDelayR_.assign (static_cast<size_t> (preDelayCap_), 0.0f);
    preDelayPos_ = 0;
}

void FxReverb::reset()
{
    bridge_.reset();
    reverb_.Init (reverbBuffer_);   // Reverb exposes no Clear(); Init() zeroes the tank
    std::fill (preDelayL_.begin(), preDelayL_.end(), 0.0f);
    std::fill (preDelayR_.begin(), preDelayR_.end(), 0.0f);
    preDelayPos_ = 0;
    lowCutLpL_ = 0.0f;
    lowCutLpR_ = 0.0f;
}

void FxReverb::process (float* L, float* R, int numSamples)
{
    // Reverb-only. For the full Clouds reverb chain (diffuser -> [pitch] -> reverb),
    // put a standalone Diffuser (and optional PitchShifter) in earlier series slots —
    // this keeps the diffuser as a single shared block, not duplicated internally.
    reverb_.set_amount (amount_);
    reverb_.set_input_gain (0.5f);                            // fixed: prevents the L+R sum clipping
    reverb_.set_time (juce::jmap (timeParam_, 0.30f, 0.95f));
    // Tone -> one-pole LP coefficient. A raw 0 is a FULL MUTE, not "darkest":
    // c.Lp is `state += c*(acc-state); acc = state` with the state init 0, so
    // klp=0 freezes the state and forces the accumulator to 0 — discarding
    // the tank output entirely (audit rev_clouds_spec, CVerb Tone=0). Map the
    // knob onto [0.05, 1] so 0 is a genuinely dark (5% leak per pass) filter.
    reverb_.set_lp (juce::jmap (lpParam_, 0.0f, 1.0f, 0.05f, 1.0f));
    reverb_.set_diffusion (diffParam_);                      // reverb's internal allpass diffusion

    // PRE-DELAY: delay the input feeding the tank by preDelaySamples_ (host
    // rate). A musical delay (not processing latency; latency()==0). Bypassed
    // entirely at 0 for bit-identical behaviour. Read-before-write on a fixed
    // ring: ring[wp-D] is the sample written D steps ago. The chain's dry path
    // uses the UN-delayed signal, so the wet onset lags the dry by preDelay.
    if (preDelaySamples_ > 0 && preDelayCap_ > preDelaySamples_)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = L[i];
            const float inR = R[i];
            int rp = preDelayPos_ - preDelaySamples_;
            if (rp < 0) rp += preDelayCap_;
            L[i] = preDelayL_[static_cast<size_t> (rp)];
            R[i] = preDelayR_[static_cast<size_t> (rp)];
            preDelayL_[static_cast<size_t> (preDelayPos_)] = inL;
            preDelayR_[static_cast<size_t> (preDelayPos_)] = inR;
            preDelayPos_ = (preDelayPos_ + 1 < preDelayCap_) ? preDelayPos_ + 1 : 0;
        }
    }

    const int m = bridge_.hostToInternal (L, R, numSamples);
    reverb_.Process (bridge_.internal(), static_cast<size_t> (m));
    bridge_.internalToHost (L, R, numSamples);

    // LOW-CUT: one-pole HP on the tail to shed mud (HP = input - one-pole LP).
    // lowCut=0 ~ flat (15 Hz, inaudible); increasing raises the cutoff
    // (15..450 Hz). Bypassed near 0.
    if (lowCutParam_ > 0.001f)
    {
        const float fc = 15.0f * std::pow (30.0f, lowCutParam_);   // 15..450 Hz
        const float a  = 1.0f - std::exp (-6.28318530718f * fc / static_cast<float> (sampleRate_));
        for (int i = 0; i < numSamples; ++i)
        {
            lowCutLpL_ += a * (L[i] - lowCutLpL_);
            L[i] -= lowCutLpL_;
            lowCutLpR_ += a * (R[i] - lowCutLpR_);
            R[i] -= lowCutLpR_;
        }
    }
}

void FxReverb::setParams (const float param[5])
{
    // Signal path: Pre-Delay -> Diffusion -> Time -> Tone(LP) -> Low-Cut(HP).
    // Amount is pinned full-wet (1.0) so the chain Dry/Wet is the sole wet/dry.
    amount_        = 1.0f;
    preDelayParam_ = juce::jlimit (0.0f, 1.0f, param[0]);
    diffParam_     = juce::jlimit (0.0f, 1.0f, param[1]);
    timeParam_     = juce::jlimit (0.0f, 1.0f, param[2]);
    lpParam_       = juce::jlimit (0.0f, 1.0f, param[3]);
    lowCutParam_   = juce::jlimit (0.0f, 1.0f, param[4]);

    // Map pre-delay 0..1 to 0..200 ms in samples (clamped to the ring capacity).
    if (preDelayCap_ > 1)
    {
        const int maxD = preDelayCap_ - 1;
        preDelaySamples_ = juce::jlimit (0, maxD,
            static_cast<int> (std::round (preDelayParam_ * 0.20 * sampleRate_)));
    }
}

FxType FxReverb::type() const { return FxType::Reverb; }

//==========================================================================
// FxLoopingDelay — Clouds looping sample player.
void FxLoopingDelay::prepare (double sampleRate, int maxBlock)
{
    bridge_.prepare (sampleRate, maxBlock);
    buf_[0].Init (bufMem_[0], kBufferSamples + 8, tailMem_[0]);
    buf_[1].Init (bufMem_[1], kBufferSamples + 8, tailMem_[1]);
    looper_.Init (2);
    params_ = {};
}

void FxLoopingDelay::reset()
{
    buf_[0].Init (bufMem_[0], kBufferSamples + 8, tailMem_[0]);
    buf_[1].Init (bufMem_[1], kBufferSamples + 8, tailMem_[1]);
    looper_.Init (2);
    bridge_.reset();
    params_ = {};
}

void FxLoopingDelay::process (float* L, float* R, int numSamples)
{
    // Apply the cached 0..1 params to the Clouds Parameters struct each block so
    // the player's internal smoothing advances at host-block rate.
    params_.position = positionParam_;
    params_.size     = sizeParam_;
    params_.pitch    = (pitchParam_ - 0.5f) * 48.0f;   // -24..+24 st, 0.5 = unison
    params_.freeze   = freezeParam_ > 0.5f;
    params_.trigger  = false;

    const int           m     = bridge_.hostToInternal (L, R, numSamples);
    clouds::FloatFrame* scr   = bridge_.internal();
    const bool          write = ! params_.freeze;

    // Chunk at <=32 internal samples (the player's per-call state is tuned for
    // kMaxBlockSize=32). Each chunk: capture the dry input into the record
    // buffers (stride 2 = interleaved FloatFrame), then play the wet output over
    // the scratch (reads the recorded past). Equivalent to the firmware's
    // record-block-then-play per 32-sample block.
    int off = 0;
    while (off < m)
    {
        const int sz = std::min (32, m - off);
        buf_[0].WriteFade (&scr[off].l, sz, 2, write);
        buf_[1].WriteFade (&scr[off].r, sz, 2, write);
        looper_.Play (buf_, params_, &scr[off].l, static_cast<size_t> (sz));
        off += sz;
    }
    bridge_.internalToHost (L, R, numSamples);
}

void FxLoopingDelay::setParams (const float param[5])
{
    positionParam_ = juce::jlimit (0.0f, 1.0f, param[0]);
    sizeParam_     = juce::jlimit (0.0f, 1.0f, param[1]);
    pitchParam_    = juce::jlimit (0.0f, 1.0f, param[2]);
    freezeParam_   = juce::jlimit (0.0f, 1.0f, param[3]);
}

FxType FxLoopingDelay::type() const { return FxType::LoopingDelay; }

//==========================================================================
// FxWSOLAStretch
void FxWSOLAStretch::prepare (double sampleRate, int maxBlock)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    bridge_.prepare (sampleRate, maxBlock);
    buf_[0].Init (bufMem_[0], kBufferSamples + 8, tailMem_[0]);
    buf_[1].Init (bufMem_[1], kBufferSamples + 8, tailMem_[1]);
    correlator_.Init (&corr_[0], &corr_[kCorrelatorWords]);
    ws_.Init (&correlator_, 2);
    params_ = {};
}

void FxWSOLAStretch::reset()
{
    buf_[0].Init (bufMem_[0], kBufferSamples + 8, tailMem_[0]);
    buf_[1].Init (bufMem_[1], kBufferSamples + 8, tailMem_[1]);
    correlator_.Init (&corr_[0], &corr_[kCorrelatorWords]);
    ws_.Init (&correlator_, 2);
    bridge_.reset();
    params_ = {};
    toneLpL_ = 0.0f;
    toneLpR_ = 0.0f;
}

void FxWSOLAStretch::process (float* L, float* R, int numSamples)
{
    // Apply the cached 0..1 params each block so the player's internal smoothing
    // (smoothed_pitch_, window_size_) advances at host-block rate.
    params_.pitch    = (pitchParam_ - 0.5f) * 48.0f;   // -24..+24 st, 0.5 = unison
    params_.position = positionParam_;
    params_.size     = sizeParam_;
    params_.trigger  = false;

    // Freeze (>0.5): stop recording into the buffer so the player keeps looping
    // the last captured material — the same WriteFade write-gate the looper uses
    // (Tier-1 un-hardcode; zero new DSP).
    const bool write = ! (freezeParam_ > 0.5f);

    const int           m   = bridge_.hostToInternal (L, R, numSamples);
    clouds::FloatFrame* scr = bridge_.internal();

    // Chunk at <=32 internal samples (the player's per-call state is tuned for
    // kMaxBlockSize=32). Each chunk: capture the dry input into the record
    // buffers (stride 2 = interleaved FloatFrame), then play the wet output over
    // the scratch (reads the recorded past), then run the correlator splice-point
    // search inline (the firmware does this in a background main loop that Parvati
    // FX slots do not have; without it the splice search stalls and WSOLA never
    // advances).
    int off = 0;
    while (off < m)
    {
        const int sz = std::min (32, m - off);
        buf_[0].WriteFade (&scr[off].l, sz, 2, write);
        buf_[1].WriteFade (&scr[off].r, sz, 2, write);
        ws_.Play (buf_, params_, &scr[off].l, static_cast<size_t> (sz));
        ws_.LoadCorrelator (buf_);
        correlator_.EvaluateSomeCandidates();
        off += sz;
    }
    bridge_.internalToHost (L, R, numSamples);

    // Post Tone: one-pole LP on the stretched output. tone=1 (bright) ~ bypass;
    // tone=0 (dark) heavy LP. Bypassed at full-bright to stay bit-identical to
    // the original WSOLA.
    if (toneParam_ < 0.999f)
    {
        const float fc = 200.0f * std::pow (100.0f, toneParam_);   // 200 Hz..20 kHz
        const float a  = 1.0f - std::exp (-6.28318530718f * fc / static_cast<float> (sampleRate_));
        for (int i = 0; i < numSamples; ++i)
        {
            toneLpL_ += a * (L[i] - toneLpL_);
            L[i] = toneLpL_;
            toneLpR_ += a * (R[i] - toneLpR_);
            R[i] = toneLpR_;
        }
    }
}

void FxWSOLAStretch::setParams (const float param[5])
{
    pitchParam_    = juce::jlimit (0.0f, 1.0f, param[0]);
    positionParam_ = juce::jlimit (0.0f, 1.0f, param[1]);
    sizeParam_     = juce::jlimit (0.0f, 1.0f, param[2]);
    freezeParam_   = juce::jlimit (0.0f, 1.0f, param[3]);
    toneParam_     = juce::jlimit (0.0f, 1.0f, param[4]);
}

FxType FxWSOLAStretch::type() const { return FxType::WSOLAStretch; }

//==========================================================================
// FxSpectral — Clouds phase vocoder (STFT + overlap-add + spectral transformation).
void FxSpectral::initPvoc()
{
    void*  buf[2] = { workspace_[0], workspace_[1] };
    size_t sz[2]  = { sizeof (workspace_[0]), sizeof (workspace_[1]) };
    // lut_sine_window_4096 is the analysis/synthesis window; 4096 = largest FFT;
    // 2 = stereo; the 32 kHz internal rate (HostRateBridge converts host->internal).
    pvoc_.Init (buf, sz, clouds::lut_sine_window_4096, 4096, 2, 16, 32000.0f);
}

void FxSpectral::prepare (double sampleRate, int maxBlock)
{
    bridge_.prepare (sampleRate, maxBlock);
    initPvoc();
    params_ = {};
}

void FxSpectral::reset()
{
    bridge_.reset();
    initPvoc();   // PhaseVocoder exposes no Reset(); Init() zeroes analysis/synthesis
    params_ = {};
}

void FxSpectral::process (float* L, float* R, int numSamples)
{
    // Apply the cached 0..1 params each block (the STFT pipeline reads them via
    // the Parameters pointer stored on Process). Spectral-only fields are held
    // fixed: freeze/gate off, no quantization/phase-randomization (Blur =
    // spectral.refresh_rate, the constantly-evolving texture control).
    params_.pitch                          = (pitchParam_ - 0.5f) * 48.0f;   // -24..+24 st, 0.5 = unison
    params_.position                       = positionParam_;
    params_.spectral.warp                  = warpParam_;
    params_.spectral.refresh_rate          = blurParam_;
    params_.freeze                         = freezeParam_ > 0.5f;
    params_.trigger                        = false;
    params_.gate                           = false;
    params_.spectral.quantization          = 0.0f;
    params_.spectral.phase_randomization   = 0.0f;

    const int           m   = bridge_.hostToInternal (L, R, numSamples);
    clouds::FloatFrame* scr = bridge_.internal();

    // Chunk at <=32 internal samples (the STFT pipeline's per-call state is tuned
    // for kMaxBlockSize=32). In-place Process (input==output) is safe: analysis_
    // and synthesis_ are separate internal buffers. Buffer() MUST be called after
    // each chunk to drain the FFT pipeline (the firmware does this in a background
    // main loop that Parvati FX slots do not have; without it -> silence).
    int off = 0;
    while (off < m)
    {
        const int sz = std::min (32, m - off);
        pvoc_.Process (params_, &scr[off], &scr[off], static_cast<size_t> (sz));
        pvoc_.Buffer();
        off += sz;
    }
    bridge_.internalToHost (L, R, numSamples);
}

void FxSpectral::setParams (const float param[5])
{
    pitchParam_    = juce::jlimit (0.0f, 1.0f, param[0]);
    warpParam_     = juce::jlimit (0.0f, 1.0f, param[1]);
    positionParam_ = juce::jlimit (0.0f, 1.0f, param[2]);
    blurParam_     = juce::jlimit (0.0f, 1.0f, param[3]);
    freezeParam_   = juce::jlimit (0.0f, 1.0f, param[4]);
}

FxType FxSpectral::type() const { return FxType::Spectral; }

//==========================================================================
// FxWavefolder — Warps bipolar wavefolder (memoryless LUT; NATIVE host rate).
void FxWavefolder::prepare (double sampleRate, int maxBlock)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    // Init the per-channel 6x SRC (Warps' polyphase FIR). The fold itself is
    // memoryless; the SRC filter history is the only state.
    srcUp_[0].Init();   srcUp_[1].Init();
    srcDown_[0].Init(); srcDown_[1].Init();
    const auto sz = static_cast<size_t> (juce::jmax (1, maxBlock) * 6 + 8);   // maxBlock*6 + headroom
    osL_.assign (sz, 0.0f);
    osR_.assign (sz, 0.0f);
}

void FxWavefolder::reset()
{
    // Re-Init the SRCs (zeroes filter history) for a clean slate.
    srcUp_[0].Init();   srcUp_[1].Init();
    srcDown_[0].Init(); srcDown_[1].Init();
    std::fill (osL_.begin(), osL_.end(), 0.0f);
    std::fill (osR_.begin(), osR_.end(), 0.0f);
    toneLpL_ = 0.0f;
    toneLpR_ = 0.0f;
}

void FxWavefolder::process (float* L, float* R, int numSamples)
{
    // Faithful to Warps Xmod<ALGORITHM_FOLD>, with the Bias wired as a small
    // CONSTANT second input (x_2) for an asymmetric fold: sum = x_1 + x_2 +
    // x_1*x_2*0.25; sum *= (0.02 + fold); lut_bipolar_fold lookup. Drive scales
    // x_1 BEFORE the fold sum (1x..4x pre-gain; 1 = unity = bit-identical to the
    // original). Wrapped in the Warps 6x oversampling (kOversampling=6) so the
    // sharp fold corners anti-alias exactly as the hardware does. Native host
    // base rate; the 6x is internal to the fold (upsample 6x -> fold each os
    // sample -> downsample 6x). A post-fold one-pole Tone LP tames the harsh
    // upper harmonics the fold generates.
    const float x2    = (biasParam_ - 0.5f) * 0.4f;          // bipolar bias (-0.2..+0.2)
    const float gain  = 0.02f + foldParam_;                  // fold amount
    const float drive = 1.0f + driveParam_ * 3.0f;           // pre-gain into the fold (1x..4x)
    constexpr float kScale = 2048.0f / ((1.0f + 1.0f + 0.25f) * 1.02f);

    srcUp_[0].Process (L, osL_.data(), static_cast<size_t> (numSamples));   // n -> n*6
    srcUp_[1].Process (R, osR_.data(), static_cast<size_t> (numSamples));
    const int os = numSamples * 6;
    for (int i = 0; i < os; ++i)
    {
        const float dl = osL_[i] * drive;
        const float sl = (dl + x2 + dl * x2 * 0.25f) * gain;
        // The fold LUT covers |sl| <= 2048/kScale ~= 2.295; the drive pre-gain
        // (up to 4x) plus the unclamped polyphonic chain input can exceed that
        // by far, and stmlib::Interpolate does NO bounds check - an over-range
        // index reads past lut_bipolar_fold[4097] into other rodata (garbage
        // output) or off the module (hard crash: the reported Wavefolder
        // SIGSEGV). Clamp to the valid domain: the fold saturates there by
        // construction (bipolar fold tables converge), so the clamp is
        // inaudible at the extremes.
        osL_[i] = stmlib::Interpolate (warps::lut_bipolar_fold + 2048,
                                       juce::jlimit (-2.29f, 2.29f, sl), kScale);
        const float dr = osR_[i] * drive;
        const float sr = (dr + x2 + dr * x2 * 0.25f) * gain;
        osR_[i] = stmlib::Interpolate (warps::lut_bipolar_fold + 2048,
                                       juce::jlimit (-2.29f, 2.29f, sr), kScale);
    }
    srcDown_[0].Process (osL_.data(), L, static_cast<size_t> (os));   // n*6 % 6 == 0
    srcDown_[1].Process (osR_.data(), R, static_cast<size_t> (os));

    // Post-fold Tone: one-pole LP. tone=1 (bright) -> ~passthrough; tone=0
    // (dark) heavy LP. Bypassed at full-bright to stay bit-identical to the
    // original wavefolder (no LP at all).
    if (toneParam_ < 0.999f)
    {
        const float fc = 200.0f * std::pow (100.0f, toneParam_);   // 200 Hz..20 kHz
        const float a  = 1.0f - std::exp (-6.28318530718f * fc / static_cast<float> (sampleRate_));
        for (int i = 0; i < numSamples; ++i)
        {
            toneLpL_ += a * (L[i] - toneLpL_);
            L[i] = toneLpL_;
            toneLpR_ += a * (R[i] - toneLpR_);
            R[i] = toneLpR_;
        }
    }
}

void FxWavefolder::setParams (const float param[5])
{
    driveParam_ = juce::jlimit (0.0f, 1.0f, param[0]);
    foldParam_  = juce::jlimit (0.0f, 1.0f, param[1]);
    biasParam_  = juce::jlimit (0.0f, 1.0f, param[2]);
    toneParam_  = juce::jlimit (0.0f, 1.0f, param[3]);
}

int FxWavefolder::latency() const noexcept
{
    // 6x SRC group delay: SRC_UP (filter_size/ratio/2 = 4 base) + SRC_DOWN
    // (filter_size/2 = 24 high-rate = 4 base) = 8 base samples.
    return srcUp_[0].delay() + srcDown_[0].delay() / 6;
}

FxType FxWavefolder::type() const { return FxType::Wavefolder; }

//==========================================================================
// FxFrequencyShifter — Warps quadrature (Hilbert) frequency shifter.
// NATIVE host rate (the lut_ap_poles Hilbert network is normalized-frequency,
// so its ~90 deg band scales with the host rate; the carrier osc inits at the
// host rate). True-stereo: each channel through its own QuadratureTransform, one
// shared sine carrier; Spread blends the right channel toward the opposite
// sideband for width. Faithful to the upstream ProcessEasterEgg core
// (modulator.cc ~62-145), internal-oscillator branch only.
void FxFrequencyShifter::prepare (double sampleRate, int maxBlock)
{
    osc_.Init (static_cast<float> (sampleRate));
    qtL_.Init (warps::lut_ap_poles, LUT_AP_POLES_SIZE);
    qtR_.Init (warps::lut_ap_poles, LUT_AP_POLES_SIZE);
    const auto sz = static_cast<size_t> (juce::jmax (1, maxBlock));
    carrierI_.assign (sz, 0.0f);
    carrierQ_.assign (sz, 0.0f);
    feedbackL_ = 0.0f;
    feedbackR_ = 0.0f;
}

void FxFrequencyShifter::reset()
{
    std::fill (carrierI_.begin(), carrierI_.end(), 0.0f);
    std::fill (carrierQ_.begin(), carrierQ_.end(), 0.0f);
    feedbackL_ = 0.0f;
    feedbackR_ = 0.0f;
}

void FxFrequencyShifter::process (float* L, float* R, int numSamples)
{
    // Shift Hz from the upstream frequency_shift_pot formula (modulator.cc:~70-82),
    // cv=0: pot 0.5 -> 0 Hz, edges -> +/-~2 kHz (cubic below 0.4, semitones above).
    const float pot       = shiftParam_;
    const float direction = pot >= 0.5f ? 1.0f : -1.0f;
    float f = 2.0f * std::fabs (pot - 0.5f);
    f = f <= 0.4f ? f * f * f * 62.5f
                  : 4.0f * stmlib::SemitonesToRatio (180.0f * (f - 0.4f));
    const float freqHz = f * direction;

    // Sine carrier I/Q, OR a richer carrier when Shape > 0 (the shape arg is
    // fed straight to Render — 0 = sine, the original hardcoded carrier; >0
    // gives triangle/saw carriers = grittier sidebands). Pure un-hardcode.
    osc_.Render (shapeParam_, freqHz, carrierI_.data(), carrierQ_.data(),
                 static_cast<size_t> (numSamples));

    // Feedback amount shaping (upstream's amount *= (2-amount) twice).
    float amount = feedbackParam_;
    amount *= (2.0f - amount);
    amount *= (2.0f - amount);
    const float spreadBlend = spreadParam_;   // 0 = R same sideband as L; 1 = opposite

    for (int i = 0; i < numSamples; ++i)
    {
        // Per-sample feedback into the Hilbert input (soft-limited to stay stable).
        const float inL = L[i];
        const float inR = R[i];
        const float modL = inL + amount * (stmlib::SoftClip (inL + feedbackL_) - inL);
        const float modR = inR + amount * (stmlib::SoftClip (inR + feedbackR_) - inR);

        float iL, qL, iR, qR;
        qtL_.Process (modL, &iL, &qL);   // single-sample Hilbert I/Q split
        qtR_.Process (modR, &iR, &qR);

        const float ci = carrierI_[i];
        const float cq = carrierQ_[i];
        const float upL   = ci * iL - cq * qL;   // upper sideband (+shift), L
        const float upR   = ci * iR - cq * qR;   // upper sideband, R
        const float downR = ci * iR + cq * qR;   // lower sideband (-shift), R

        L[i] = upL;
        R[i] = upR + (downR - upR) * spreadBlend;   // spread flips R toward the opposite sideband

        // Smoothed feedback path (upstream ONE_POLE 0.2).
        feedbackL_ += 0.2f * (L[i] - feedbackL_);
        feedbackR_ += 0.2f * (R[i] - feedbackR_);
    }
}

void FxFrequencyShifter::setParams (const float param[5])
{
    shiftParam_    = juce::jlimit (0.0f, 1.0f, param[0]);
    shapeParam_    = juce::jlimit (0.0f, 1.0f, param[1]);
    feedbackParam_ = juce::jlimit (0.0f, 1.0f, param[2]);
    spreadParam_   = juce::jlimit (0.0f, 1.0f, param[3]);
}

FxType FxFrequencyShifter::type() const { return FxType::FrequencyShifter; }

//==========================================================================
// FxRingModulator — Warps analog (diode-model) ring modulator + internal carrier.
namespace
{
    // Parker DAFx-11 diode non-linearity (modulator.cc:347). Memoryless.
    float warpsDiode (float x) noexcept
    {
        const float sign = x > 0.0f ? 1.0f : -1.0f;
        float dead_zone = std::fabs (x) - 0.667f;
        dead_zone += std::fabs (dead_zone);
        dead_zone *= dead_zone;
        return 0.04324765822726063f * dead_zone * sign;
    }
}

void FxRingModulator::prepare (double sampleRate, int maxBlock)
{
    // D3: the carrier is rendered at the BASE rate (n samples) then upsampled
    // through a dedicated mono src_up_ — matching upstream src_up_[0]=carrier
    // (modulator.cc:291). This band-limits the carrier to fs/2 before the diode
    // and time-aligns it with the upsampled signal (both pass src_up_). So the
    // carrier osc inits at the BASE host rate, not 6x.
    carrier_.Init (static_cast<float> (sampleRate));
    srcUp_[0].Init();   srcUp_[1].Init();
    srcUpCarrier_.Init();
    srcDown_[0].Init(); srcDown_[1].Init();
    const auto szOs   = static_cast<size_t> (juce::jmax (1, maxBlock) * 6 + 8);  // 6x oversampled (n*6)
    const auto szBase = static_cast<size_t> (juce::jmax (1, maxBlock) + 8);     // base rate (n)
    carrierBaseI_.assign (szBase, 0.0f);
    carrierBaseQ_.assign (szBase, 0.0f);
    carrierOs_.assign (szOs, 0.0f);
    osL_.assign (szOs, 0.0f);
    osR_.assign (szOs, 0.0f);
}

void FxRingModulator::reset()
{
    srcUp_[0].Init();   srcUp_[1].Init();
    srcUpCarrier_.Init();
    srcDown_[0].Init(); srcDown_[1].Init();
    std::fill (carrierBaseI_.begin(), carrierBaseI_.end(), 0.0f);
    std::fill (carrierBaseQ_.begin(), carrierBaseQ_.end(), 0.0f);
    std::fill (carrierOs_.begin(), carrierOs_.end(), 0.0f);
    std::fill (osL_.begin(), osL_.end(), 0.0f);
    std::fill (osR_.begin(), osR_.end(), 0.0f);
}

void FxRingModulator::process (float* L, float* R, int numSamples)
{
    // Carrier 20 Hz..4 kHz (log); shape 0..1 (Render scales it to 0..1.9999 for
    // the wavetable index: sine -> harmonics -> buzzy). The diode product runs
    // inside the Warps 6x oversampling (kOversampling=6): BOTH the signal and the
    // carrier are rendered at the base rate then upsampled 6x through src_up_
    // (carrier via srcUpCarrier_, signal via srcUp_[0/1]), so the Diode()
    // (signal +/- carrier) product happens entirely in the oversampled domain
    // with the carrier band-limited to fs/2 and time-aligned with the signal
    // (faithful to upstream src_up_[0]=carrier, src_up_[1]=modulator), then
    // downsampled 6x back to the host rate. Native host base rate.
    const float freqHz = 20.0f * std::pow (200.0f, carrierParam_);
    const float shape  = shapeParam_;   // Render() multiplies this by 1.9999 internally
    const float gain   = 4.0f + amountParam_ * 24.0f;   // ring intensity (upstream ring *= (4+param*24))

    // Render the carrier at the BASE rate, then upsample it 6x (D3).
    carrier_.Render (shape, freqHz, carrierBaseI_.data(), carrierBaseQ_.data(),
                     static_cast<size_t> (numSamples));
    srcUpCarrier_.Process (carrierBaseI_.data(), carrierOs_.data(),
                           static_cast<size_t> (numSamples));   // n -> n*6

    // Signal up 6x.
    srcUp_[0].Process (L, osL_.data(), static_cast<size_t> (numSamples));
    srcUp_[1].Process (R, osR_.data(), static_cast<size_t> (numSamples));

    const int os = numSamples * 6;
    for (int i = 0; i < os; ++i)
    {
        const float c = carrierOs_[i] * 2.0f;
        // Input-domain clamp (Wavefolder precedent, :477-482): warpsDiode
        // grows QUADRATICALLY past its dead zone and SoftLimit is NOT
        // bounded (x/9 for large x), so an unclamped hot chain input makes
        // gain*diode-sum explode (|in|=4 @ amount=1 measured ~16x). Upstream
        // Warps fed this stage ADC-bounded +/-1 audio — restore that
        // contract in the oversampled domain. Everything at or below nominal
        // full scale is bit-identical; the clamped path peaks ~2.95 at max
        // amount (the same corner in-range audio already reaches).
        const float xl = juce::jlimit (-1.0f, 1.0f, osL_[i]);
        const float xr = juce::jlimit (-1.0f, 1.0f, osR_[i]);
        osL_[i] = stmlib::SoftLimit (gain * (warpsDiode (xl + c) + warpsDiode (xl - c)));
        osR_[i] = stmlib::SoftLimit (gain * (warpsDiode (xr + c) + warpsDiode (xr - c)));
    }
    srcDown_[0].Process (osL_.data(), L, static_cast<size_t> (os));   // n*6 % 6 == 0
    srcDown_[1].Process (osR_.data(), R, static_cast<size_t> (os));
}

void FxRingModulator::setParams (const float param[5])
{
    carrierParam_ = juce::jlimit (0.0f, 1.0f, param[0]);
    shapeParam_   = juce::jlimit (0.0f, 1.0f, param[1]);
    amountParam_  = juce::jlimit (0.0f, 1.0f, param[2]);
}

int FxRingModulator::latency() const noexcept
{
    // 6x SRC group delay: signal srcUp_ (4 base) + srcDown_ (4 base) = 8 base
    // samples. The carrier's srcUpCarrier_ adds the SAME 4-sample up-delay as
    // the signal's srcUp_, so carrier and signal stay aligned (no extra latency).
    return srcUp_[0].delay() + srcDown_[0].delay() / 6;
}

FxType FxRingModulator::type() const { return FxType::RingModulator; }

//==========================================================================
// FxResonator — Rings modal resonator. NATIVE host rate: the SVF coefficients
// are computed from the normalized frequency (freqHz / sampleRate) each block,
// so they track the host rate exactly (no resampler/oversampling needed — the
// resonator is LTI). Rings-faithful stereo: ONE resonator processes a mono sum
// (0.5*(L+R)); its out (odd modes) -> L and aux (even modes) -> R, matching
// Rings' mono path (part.cc). Position rebalances odd vs even. The upstream
// Process attenuates input by 0.125 (-18 dB); the Rings output limiter (modal
// model_gains_=1.4 drive) bounds sustained on-resonance build-up to ~0.8 peak.
constexpr float kResonatorDrive = 1.4f;   // Rings modal model_gains_ (limiter pre-gain)

void FxResonator::prepare (double sampleRate, int maxBlock)
{
    sampleRate_ = sampleRate;
    res_.Init();
    limiter_.Init();
    const auto sz = static_cast<size_t> (juce::jmax (1, maxBlock));
    inMono_.assign (sz, 0.0f);
    wetL_.assign (sz, 0.0f);
    wetR_.assign (sz, 0.0f);
}

void FxResonator::reset()
{
    res_.Init();
    limiter_.Init();
    std::fill (inMono_.begin(), inMono_.end(), 0.0f);
    std::fill (wetL_.begin(), wetL_.end(), 0.0f);
    std::fill (wetR_.begin(), wetR_.end(), 0.0f);
}

void FxResonator::process (float* L, float* R, int numSamples)
{
    // Map the 0..1 Pitch param to a MIDI note (C1=24 .. C7=96), then to a
    // normalized frequency (fraction of sample rate). SemitonesToRatio handles
    // the exponential pitch curve (MIDI note -> frequency ratio).
    const float note     = 24.0f + pitchParam_ * 72.0f;
    const float freqHz   = stmlib::SemitonesToRatio (note - 69.0f) * 440.0f;
    const float freqNorm = juce::jlimit (0.0001f, 0.49f,
                                          static_cast<float> (freqHz / sampleRate_));

    res_.set_frequency (freqNorm);
    res_.set_structure (structureParam_);   // inharmonicity / modal layout (0.25 = Rings default)
    res_.set_brightness (brightParam_);
    res_.set_damping (decayParam_);
    res_.set_position (positionParam_);  // odd/even mode balance (pickup position)

    const auto n = static_cast<size_t> (numSamples);
    // Sum L+R to mono at -6 dB to avoid doubling, then feed one resonator.
    for (int i = 0; i < numSamples; ++i)
        inMono_[i] = 0.5f * (L[i] + R[i]);

    // Rings-native stereo: out (odd modes) -> L, aux (even modes) -> R.
    // Position rebalances odd vs even, acting as pickup position + stereo width.
    res_.Process (inMono_.data(), wetL_.data(), wetR_.data(), n);

    // Rings output limiter (modal model_gains_=1.4 drive). Bounds each channel
    // to ~0.8 peak (SoftLimit toward ~1.0) so sustained on-resonance build-up
    // no longer hard-clips. In-place on wetL_/wetR_, then copy to the outputs.
    limiter_.Process (wetL_.data(), wetR_.data(), n, kResonatorDrive);
    for (int i = 0; i < numSamples; ++i)
    {
        L[i] = wetL_[i];
        R[i] = wetR_[i];
    }
}

void FxResonator::setParams (const float param[5])
{
    pitchParam_     = juce::jlimit (0.0f, 1.0f, param[0]);
    decayParam_     = juce::jlimit (0.0f, 1.0f, param[1]);
    brightParam_    = juce::jlimit (0.0f, 1.0f, param[2]);
    positionParam_  = juce::jlimit (0.0f, 1.0f, param[3]);
    structureParam_ = juce::jlimit (0.0f, 1.0f, param[4]);
}

FxType FxResonator::type() const { return FxType::Resonator; }

//==========================================================================
// Factory.
std::unique_ptr<FxProcessor> createFxProcessor (FxType t)
{
    switch (t)
    {
        case FxType::Diffuser:     return std::make_unique<FxDiffuser>();
        case FxType::PitchShifter: return std::make_unique<FxPitchShifter>();
        case FxType::Reverb: return std::make_unique<FxReverb>();
        case FxType::LoopingDelay:  return std::make_unique<FxLoopingDelay>();
        case FxType::WSOLAStretch:  return std::make_unique<FxWSOLAStretch>();
        case FxType::Spectral:      return std::make_unique<FxSpectral>();
        case FxType::Wavefolder:        return std::make_unique<FxWavefolder>();
        case FxType::FrequencyShifter:  return std::make_unique<FxFrequencyShifter>();
        case FxType::RingModulator:     return std::make_unique<FxRingModulator>();
        case FxType::Resonator:           return std::make_unique<FxResonator>();
        // FV-1 hardware-emulation family (Source/dsp/fx/fv1/).
        case FxType::ClockedDelay:    return std::make_unique<parvati::fv1::Fv1ClockedDelay>();
        case FxType::Ensemble:        return std::make_unique<parvati::fv1::Fv1Ensemble>();
        case FxType::PlateReverb:     return std::make_unique<parvati::fv1::Fv1PlateReverb>();
        case FxType::VinylCompressor: return std::make_unique<parvati::fv1::Fv1VinylCompressor>();
        case FxType::Phaser:          return std::make_unique<parvati::fv1::Fv1Phaser>();
        // FV-1 family, second wave (2026-08-17).
        case FxType::Overdrive:       return std::make_unique<parvati::fv1::Fv1Overdrive>();
        case FxType::LutDistortion:   return std::make_unique<parvati::fv1::Fv1LutDistortion>();
        case FxType::Compressor:      return std::make_unique<parvati::fv1::Fv1Compressor>();
        case FxType::Gate:            return std::make_unique<parvati::fv1::Fv1Gate>();
        case FxType::Chorus:          return std::make_unique<parvati::fv1::Fv1Chorus>();
        case FxType::Flanger:         return std::make_unique<parvati::fv1::Fv1Flanger>();
        case FxType::Echo:            return std::make_unique<parvati::fv1::Fv1Echo>();
        case FxType::Room:            return std::make_unique<parvati::fv1::Fv1Room>();
        case FxType::Spring:          return std::make_unique<parvati::fv1::Fv1Spring>();
        case FxType::None:
        case FxType::Count:   break;
    }
    return {};
}

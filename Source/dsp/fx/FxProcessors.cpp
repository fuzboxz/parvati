// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See FxProcessors.h.

#include "dsp/fx/FxProcessors.h"

#include "SynthEngine.h"   // FxType enumerators (factory switch)

namespace
{
    constexpr float kMaxDelaySeconds = 1.0f;   // 0..1 s param range
}

//==========================================================================
// FxGainPan
void FxGainPan::prepare (double, int) {}
void FxGainPan::reset() {}

void FxGainPan::process (float* L, float* R, int numSamples)
{
    for (int i = 0; i < numSamples; ++i)
    {
        L[i] *= gainLinear_ * panLGain_;
        R[i] *= gainLinear_ * panRGain_;
    }
}

void FxGainPan::setParams (const float param[4])
{
    // param0: gain 0..1 -> -12..+12 dB.
    const float db      = juce::jmap (param[0], 0.0f, 1.0f, -12.0f, 12.0f);
    gainLinear_         = juce::Decibels::decibelsToGain (db);
    // param1: pan 0..1 -> equal-power L..R (constant total power).
    const float panPos  = juce::jlimit (0.0f, 1.0f, param[1]);
    const float angle   = panPos * juce::MathConstants<float>::halfPi;
    panLGain_           = std::cos (angle);
    panRGain_           = std::sin (angle);
}

FxType FxGainPan::type() const { return FxType::GainPan; }

//==========================================================================
// FxDelay
void FxDelay::prepare (double sampleRate, int maxBlock)
{
    rate_ = sampleRate;
    const juce::dsp::ProcessSpec spec {
        sampleRate,
        juce::jmax (1u, (juce::uint32) maxBlock),
        2u   // stereo (per-channel pushSample/popSample used)
    };
    delayL_.prepare (spec);
    delayR_.prepare (spec);
    delayL_.setMaximumDelayInSamples ((int) std::ceil (kMaxDelaySeconds * sampleRate) + 4);
    delayR_.setMaximumDelayInSamples ((int) std::ceil (kMaxDelaySeconds * sampleRate) + 4);
    delayL_.setDelay (0.0);
    delayR_.setDelay (0.0);
}

void FxDelay::reset()
{
    delayL_.reset();
    delayR_.reset();
    lastWetL_ = lastWetR_ = 0.0f;
}

void FxDelay::process (float* L, float* R, int numSamples)
{
    // Per-channel feedback delay: out = dry + delayed(feedback loop).
    const float fb       = feedback_;
    const float baseTime = juce::jmax (1.0f, timeSec_ * (float) rate_);
    // Stereo spread shortens the R delay line by a fraction of the base time
    // (0 = matched, 1 = R is 50% of L time -> wide haas-ish image).
    const float lDelay = baseTime;
    const float rDelay = juce::jmax (1.0f, baseTime * (1.0f - 0.5f * spreadFrac_));
    delayL_.setDelay (lDelay);
    delayR_.setDelay (rDelay);
    for (int i = 0; i < numSamples; ++i)
    {
        const float wetL = delayL_.popSample (0);
        const float wetR = delayR_.popSample (0);
        delayL_.pushSample (0, L[i] + wetL * fb);
        delayR_.pushSample (0, R[i] + wetR * fb);
        L[i] = wetL;
        R[i] = wetR;
    }
}

void FxDelay::setParams (const float param[4])
{
    timeSec_    = juce::jlimit (0.0f, 1.0f, param[0]) * kMaxDelaySeconds;
    feedback_   = juce::jlimit (0.0f, 1.0f, param[1]);
    spreadFrac_ = juce::jlimit (0.0f, 1.0f, param[2]);
}

FxType FxDelay::type() const { return FxType::Delay; }

//==========================================================================
// FxReverb
void FxReverb::prepare (double sampleRate, int maxBlock)
{
    rate_ = sampleRate;
    const juce::dsp::ProcessSpec spec {
        sampleRate,
        juce::jmax (1u, (juce::uint32) maxBlock),
        2u
    };
    reverb_.prepare (spec);
    reverb_.setParameters (params_);
}

void FxReverb::reset()
{
    reverb_.reset();
}

void FxReverb::process (float* L, float* R, int numSamples)
{
    if (dirty_)
    {
        reverb_.setParameters (params_);
        dirty_ = false;
    }
    // Wrap the in-place L/R stereo pair as a 2-channel AudioBlock (no allocation:
    // AudioBlock only stores pointers + length).
    float* chans[2] = { L, R };
    juce::dsp::AudioBlock<float> block (chans, 2u, (size_t) numSamples);
    juce::dsp::ProcessContextReplacing<float> ctx (block);
    reverb_.process (ctx);
}

void FxReverb::setParams (const float param[4])
{
    // param0..3 -> roomSize / damping / wetLevel / width.
    juce::dsp::Reverb::Parameters p;
    p.roomSize   = juce::jlimit (0.0f, 1.0f, param[0]);
    p.damping    = juce::jlimit (0.0f, 1.0f, param[1]);
    p.wetLevel   = juce::jlimit (0.0f, 1.0f, param[2]);
    p.dryLevel   = 0.0f;   // dry is blended by the chain
    p.width      = juce::jlimit (0.0f, 1.0f, param[3]);
    p.freezeMode = 0.0f;
    params_ = p;
    dirty_  = true;   // applied at the next process() (single-threaded on the AT)
}

FxType FxReverb::type() const { return FxType::Reverb; }

//==========================================================================
// FxChorus
void FxChorus::prepare (double sampleRate, int maxBlock)
{
    rate_ = sampleRate;
    const juce::dsp::ProcessSpec spec {
        sampleRate,
        juce::jmax (1u, (juce::uint32) maxBlock),
        2u
    };
    chorus_.prepare (spec);
    chorus_.setRate (rateHz_);
    chorus_.setDepth (depth_);
    chorus_.setMix (1.0);
    chorus_.setFeedback (0.0);
    chorus_.setCentreDelay (10.0);
}

void FxChorus::reset()
{
    chorus_.reset();
}

void FxChorus::process (float* L, float* R, int numSamples)
{
    if (dirty_)
    {
        chorus_.setRate (rateHz_);
        chorus_.setDepth (depth_);
        dirty_ = false;
    }
    float* chans[2] = { L, R };
    juce::dsp::AudioBlock<float> block (chans, 2u, (size_t) numSamples);
    juce::dsp::ProcessContextReplacing<float> ctx (block);
    chorus_.process (ctx);
}

void FxChorus::setParams (const float param[4])
{
    // param0: rate 0..1 -> 0.1..8 Hz; param1: depth 0..1.
    rateHz_ = juce::jmap (juce::jlimit (0.0f, 1.0f, param[0]), 0.1f, 8.0f);
    depth_  = juce::jlimit (0.0f, 1.0f, param[1]);
    dirty_  = true;   // applied at the next process() (single-threaded on the AT)
}

FxType FxChorus::type() const { return FxType::Chorus; }

//==========================================================================
// Factory.
std::unique_ptr<FxProcessor> createFxProcessor (FxType t)
{
    switch (t)
    {
        case FxType::GainPan: return std::make_unique<FxGainPan>();
        case FxType::Delay:   return std::make_unique<FxDelay>();
        case FxType::Reverb:  return std::make_unique<FxReverb>();
        case FxType::Chorus:  return std::make_unique<FxChorus>();
        case FxType::None:
        case FxType::Count:   break;
    }
    return {};
}

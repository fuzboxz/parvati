// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// FxProcessors — the four placeholder per-slot effects (GainPan / Delay / Reverb
// / Chorus) built on juce::dsp. Each maps the four generic 0..1 slot params to
// its own controls in setParams() and renders an in-place stereo wet block in
// process(). Dry/wet + topology routing live in FxChain.
//
// Placeholder set (v1): functional but simple. A richer effect library is an
// explicit non-goal for this phase. All effects are allocation-free on the audio
// thread (prepare reserves; juce::dsp objects are prepared once).

#pragma once

#include <juce_dsp/juce_dsp.h>

#include "dsp/fx/FxProcessor.h"

// Gain + stereo pan. param0 = gain (0..1 -> -12..+12 dB), param1 = pan
// (0..1 -> hard-left..hard-right, equal-power). Output stays stereo.
class FxGainPan : public FxProcessor
{
public:
    void prepare (double sampleRate, int maxBlock) override;
    void reset() override;
    void process (float* L, float* R, int numSamples) override;
    void setParams (const float param[4]) override;
    FxType type() const override;

private:
    float gainLinear_ = 1.0f;   // applied equally to L+R
    float panLGain_   = 0.7071f;   // equal-power L coefficient
    float panRGain_   = 0.7071f;   // equal-power R coefficient
};

// Feedback delay. param0 = time (0..1 -> 0..1 s), param1 = feedback (0..1),
// param2 = stereo spread (0..1 -> 0..50% offset between L/R delay times).
class FxDelay : public FxProcessor
{
public:
    void prepare (double sampleRate, int maxBlock) override;
    void reset() override;
    void process (float* L, float* R, int numSamples) override;
    void setParams (const float param[4]) override;
    FxType type() const override;

private:
    double rate_ = 44100.0;
    juce::dsp::DelayLine<float> delayL_;
    juce::dsp::DelayLine<float> delayR_;
    float timeSec_    = 0.0f;   // 0..1
    float feedback_   = 0.0f;   // 0..1
    float spreadFrac_ = 0.0f;   // 0..1
    float lastWetL_   = 0.0f, lastWetR_ = 0.0f;   // feedback state
};

// Algorithmic reverb. param0..3 -> roomSize / damping / wetLevel / width.
class FxReverb : public FxProcessor
{
public:
    void prepare (double sampleRate, int maxBlock) override;
    void reset() override;
    void process (float* L, float* R, int numSamples) override;
    void setParams (const float param[4]) override;
    FxType type() const override;

private:
    double rate_ = 44100.0;
    juce::dsp::Reverb reverb_;
    juce::dsp::Reverb::Parameters params_ {};
    bool dirty_ = true;   // re-apply parameters on the next process()
};

// Modulated chorus. param0 = rate (0..1 -> 0.1..8 Hz), param1 = depth (0..1).
class FxChorus : public FxProcessor
{
public:
    void prepare (double sampleRate, int maxBlock) override;
    void reset() override;
    void process (float* L, float* R, int numSamples) override;
    void setParams (const float param[4]) override;
    FxType type() const override;

private:
    double rate_ = 44100.0;
    juce::dsp::Chorus<float> chorus_;
    float rateHz_  = 0.0f;
    float depth_   = 0.0f;
    bool dirty_    = true;
};

// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
// Ambika analog-filter emulation (juce::dsp). See analog_filter.h for details.

#include "dsp/analog_filter.h"

#include <cmath>

namespace ambika::dsp {

namespace {
// Builds a mono ProcessSpec for a given sample rate / max block.
juce::dsp::ProcessSpec makeSpec (double sampleRate, int blockSize) noexcept
{
    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (juce::jmax (1, blockSize));
    spec.numChannels      = 1;
    return spec;
}
} // namespace

void AnalogFilter::prepare (double sampleRate, int blockSize)
{
    sampleRate_ = sampleRate;
    blockSize_  = juce::jmax (1, blockSize);

    const auto spec = makeSpec (sampleRate_, blockSize_);

    // Both 4-pole topologies are lowpass-only in the hardware -> LPF24.
    ladder_.prepare (spec);
    ladder_.setMode (juce::dsp::LadderFilterMode::LPF24);

    // 4-pole "4P": two series lowpass TPT SVFs (cutoff+resonance linked in
    // applyParams()).
    svf4p_[0].prepare (spec);
    svf4p_[1].prepare (spec);
    svf4p_[0].setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    svf4p_[1].setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    svf4p_[0].reset (0.0f);
    svf4p_[1].reset (0.0f);

    svf_.prepare (spec);
    svfNotch_.prepare (spec);
    // svfNotch_ is a fixed highpass; svf_'s type is selected per mode in applyParams().
    svfNotch_.setType (juce::dsp::StateVariableTPTFilterType::highpass);

    prepared_       = true;
    topologyChanged_ = true;
    dirty_          = true;
    commit();
}

void AnalogFilter::setTopology (FilterTopology newTopology)
{
    if (newTopology == topology_)
        return;

    topology_       = newTopology;
    topologyChanged_ = true;
    dirty_          = true;
}

void AnalogFilter::setMode (int newMode)
{
    newMode = juce::jlimit (0, 3, newMode);
    const auto m = static_cast<AnalogFilterMode> (newMode);
    if (m == mode_)
        return;

    mode_  = m;
    dirty_ = true;
}

void AnalogFilter::setCutoffHz (float newCutoffHz)
{
    const float nyq  = 0.49f * static_cast<float> (sampleRate_);
    const float cap  = juce::jmin (kMaxHz, nyq);
    newCutoffHz      = juce::jlimit (kMinHz, cap, newCutoffHz);

    if (newCutoffHz == cutoffHz_)
        return;

    cutoffHz_ = newCutoffHz;
    dirty_    = true;
}

void AnalogFilter::setResonance (float newResonance)
{
    newResonance = juce::jlimit (0.0f, 1.0f, newResonance);
    if (newResonance == resonance_)
        return;

    resonance_ = newResonance;
    dirty_     = true;
}

void AnalogFilter::commit()
{
    if (! prepared_)
        return;

    if (! dirty_)
        return;

    if (topologyChanged_)
    {
        // Reset the newly-active filter so stale state from the previous
        // topology does not produce a click.
        switch (topology_)
        {
            case FilterTopology::FOUR_POLE_SSM2164:
                svf4p_[0].reset (0.0f);
                svf4p_[1].reset (0.0f);
                break;
            case FilterTopology::TWO_POLE_SVF:
                svf_.reset (0.0f);
                svfNotch_.reset (0.0f);
                break;
            case FilterTopology::FOUR_POLE_LADDER:
            default:
                ladder_.reset();
                ladder_.setMode (juce::dsp::LadderFilterMode::LPF24);
                break;
        }
        topologyChanged_ = false;
    }

    applyParams();
    dirty_ = false;
}

void AnalogFilter::applyParams()
{
    const float res      = juce::jlimit (0.0f, 1.0f, resonance_);
    const float safeRes  = juce::jmin (res, kMaxResonance);

    if (topology_ == FilterTopology::FOUR_POLE_LADDER)
    {
        ladder_.setCutoffFrequencyHz (cutoffHz_);
        ladder_.setResonance (safeRes);
        ladder_.setDrive (drive_);   // tanh saturation drive (default 1.2 == JUCE default)
    }
    else if (topology_ == FilterTopology::FOUR_POLE_SSM2164)
    {
        // "4P": two series lowpass TPT SVFs, cutoff + resonance LINKED.
        const float svfRes = juce::jlimit (0.05f, kMaxResonance, res);
        for (int i = 0; i < 2; ++i)
        {
            svf4p_[i].setCutoffFrequency (cutoffHz_);
            svf4p_[i].setResonance (svfRes);
        }
    }
    else // TWO_POLE_SVF
    {
        // JUCE's TPT SVF computes R2 = 1/resonance internally; resonance==0
        // => division by zero => NaN. Floor it so the minimum-resonance case
        // (maximum damping / no resonance peak) stays finite and stable.
        const float svfRes = juce::jlimit (0.05f, kMaxResonance, res);
        svf_.setCutoffFrequency (cutoffHz_);
        svf_.setResonance (svfRes);
        svfNotch_.setCutoffFrequency (cutoffHz_);
        svfNotch_.setResonance (svfRes);

        // svfNotch_ stays highpass; pick svf_'s tap by mode.
        switch (mode_)
        {
            case AnalogFilterMode::Bandpass:
                svf_.setType (juce::dsp::StateVariableTPTFilterType::bandpass);
                break;
            case AnalogFilterMode::Highpass:
                svf_.setType (juce::dsp::StateVariableTPTFilterType::highpass);
                break;
            case AnalogFilterMode::Notch:
                // notch = lowpass(svf_) + highpass(svfNotch_)
                svf_.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
                break;
            case AnalogFilterMode::Lowpass:
            default:
                svf_.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
                break;
        }
    }
}

float AnalogFilter::processSample (float inputValue)
{
    if (! prepared_)
        return inputValue;

    if (topology_ == FilterTopology::FOUR_POLE_SSM2164)
    {
        // Cascade: stage 0 then stage 1 (both lowpass). 24 dB/oct, linear.
        const float a = svf4p_[0].processSample (0, inputValue);
        return svf4p_[1].processSample (0, a);
    }

    if (topology_ == FilterTopology::TWO_POLE_SVF)
    {
        // NOTE: juce::dsp::StateVariableTPTFilter::processSample(channel, input).
        switch (mode_)
        {
            case AnalogFilterMode::Bandpass:
                return svf_.processSample (0, inputValue);
            case AnalogFilterMode::Highpass:
                return svf_.processSample (0, inputValue);
            case AnalogFilterMode::Notch:
                return svf_.processSample (0, inputValue)     // lowpass
                     + svfNotch_.processSample (0, inputValue); // highpass
            case AnalogFilterMode::Lowpass:
            default:
                return svf_.processSample (0, inputValue);
        }
    }

    // 4-pole. Direct per-sample call through the LadderTap: JUCE's public
    // process() runs `updateSmoothers(); processSample (v, ch);` per sample,
    // so this reproduces the exact per-sample sequence of the legacy 1-sample
    // AudioBlock + ProcessContextReplacing routing (bit-identical output —
    // pinned by parvati_analog_filter_batch_test) without the per-sample
    // block/context construction.
    ladder_.updateSmoothers();
    return ladder_.processSample (inputValue, 0);
}

void AnalogFilter::processBlock (float* data, int numSamples)
{
    if (! prepared_ || data == nullptr || numSamples <= 0)
        return;

    for (int i = 0; i < numSamples; ++i)
        data[i] = processSample (data[i]);
}

float AnalogFilter::cutoffByteToHz (uint8_t cutoffByte)
{
    // Exponential sweep across [kMinHz, kMaxHz]. cutoffByte spans the full 8-bit
    // range 0..255 (the firmware's 7-bit value is left-shifted/aligned by the
    // caller if desired); either way this is an exponential Hz mapping.
    const float t = static_cast<float> (cutoffByte) / 255.0f;
    const float hz = kMinHz * std::pow (kMaxHz / kMinHz, t);
    return juce::jlimit (kMinHz, kMaxHz, hz);
}

} // namespace ambika::dsp

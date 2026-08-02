// Copyright (c) 2024 805LABS / Parvati.
// Ambika analog-filter emulation. Original Ambika firmware (C) Emilie Gillet, GPL3.
//
// There is NO filter code in the Ambika firmware: the filter is ANALOG hardware
// (one per voicecard). The voice DSP computes cutoff / resonance (8-bit) and a
// 2-bit filter mode and sends them to a DAC + parallel port. This module
// emulates that analog filter in software using juce::dsp, fresh-written.
//
// Three voicecard topologies (see docs/DSP_PORT_SPEC.md section E):
//   * 4-pole LM13700 (SMR4)        -> juce::dsp::LadderFilter, LPF24
//   * 4-pole SSM2164 cascade       -> juce::dsp::LadderFilter, LPF24  (v1: identical to LM13700)
//   * 2-pole SVF (SSM2164)         -> juce::dsp::StateVariableTPTFilter (LP/BP/HP, NOTCH = low+high)

#pragma once

#include <juce_dsp/juce_dsp.h>

namespace ambika::dsp {

// Selectable voicecard filter topology.
enum class FilterTopology {
    FOUR_POLE_LADDER,   // SMR4 / LM13700.  juce::dsp::LadderFilter LPF24 (aggressive, self-oscillating).
    FOUR_POLE_SSM2164,  // 4-pole SSM2164 cascade.  Custom 4-stage one-pole cascade + feedback (smoother / 'politer'). Always LP.
    TWO_POLE_SVF        // 2-pole state-variable (SSM2164).  juce::dsp::StateVariableTPTFilter (LP/BP/HP, NOTCH = low+high).
};

// Local filter-mode enum. Values match common/patch.h FilterMode
// (FILTER_MODE_LP=0, _BP=1, _HP=2, _NOTCH=3). Kept as a *distinct* identifier
// (AnalogFilterMode) so this module stays independent of dsp/patch.h during the
// parallel porting phase; integration will reconcile it with patch.h::FilterMode.
enum class AnalogFilterMode : int {
    Lowpass  = 0,
    Bandpass = 1,
    Highpass = 2,
    Notch    = 3
};

// Emulation of the Ambika analog filter section.
//
// Usage (per voice, once per 40-sample control block, then per sample):
//     filter.prepare(sampleRate, blockSize);
//     // ... once per control block:
//     filter.setTopology(t);
//     filter.setMode(patchMode);        // 0..3
//     filter.setCutoffHz(hzFromVoice);  // Voice passes the absolute Hz it computed
//     filter.setResonance(r);           // 0..1
//     filter.commit();                  // pushes cached params into the juce::dsp filter(s)
//     // ... per audio sample:
//     out = filter.processSample(in);
//
// This mirrors the hardware CV-update cadence (params updated once per block,
// filter processed every sample).
class AnalogFilter
{
public:
    // Cutoff span (Hz). The hardware DAC/OTA is roughly exponential V/Hz; these
    // bounds are the v1 mapping, tunable in Phase 3 against a frequency response.
    static constexpr float kMinHz = 20.0f;
    static constexpr float kMaxHz = 16000.0f;
    // Resonance ceiling kept below the juce::dsp self-oscillation point (~1.0).
    static constexpr float kMaxResonance = 0.95f;

    AnalogFilter() = default;
    ~AnalogFilter() = default;

    // Non-copyable (owns juce::dsp filter state).
    AnalogFilter (const AnalogFilter&) = delete;
    AnalogFilter& operator= (const AnalogFilter&) = delete;

    // Allocates/prepares all underlying juce::dsp filter objects (mono, 1 channel).
    void prepare (double sampleRate, int blockSize);

    // --- control-rate setters (cache values; apply on commit()) ----------------

    // Selects the voicecard topology. Re-arming a different topology resets the
    // newly-active filter's state on the next commit() to avoid stale-state clicks.
    void setTopology (FilterTopology newTopology);

    // 0..3 = LP/BP/HP/NOTCH. For the 4-pole topologies this is ignored (HW is
    // lowpass-only); only the SVF topology honours mode.
    void setMode (int newMode);

    // Absolute cutoff in Hz. The Voice module is expected to pass the Hz it
    // derived from the firmware's 8-bit, key-tracked cutoff value. Clamped to
    // [kMinHz, min(kMaxHz, 0.49 * sampleRate)].
    void setCutoffHz (float newCutoffHz);

    // Resonance in 0..1. Capped internally at kMaxResonance for stability.
    void setResonance (float newResonance);

    // Pushes cached cutoff/resonance/mode/topology into the active juce::dsp
    // filter(s). Call ONCE per 40-sample control block.
    void commit();

    // --- audio-rate processing (call every sample, after commit()) -------------

    float processSample (float inputValue);
    void  processBlock (float* data, int numSamples);

    // --- accessors -------------------------------------------------------------
    FilterTopology getTopology() const noexcept  { return topology_; }
    int            getMode()     const noexcept  { return static_cast<int> (mode_); }
    float          getCutoffHz() const noexcept  { return cutoffHz_; }
    float          getResonance() const noexcept { return resonance_; }
    bool           isPrepared()  const noexcept  { return prepared_; }

    // Map the firmware 8-bit cutoff (0..255) to Hz, exponential across ~20..16k.
    // Provided as a convenience; the primary path is setCutoffHz() driven by the
    // Voice (which already folds in keytracking + env + lfo before scaling).
    static float cutoffByteToHz (uint8_t cutoffByte);

private:
    // Pushes the cached cutoff/resonance + mode into the active filter(s).
    void applyParams();

    double sampleRate_ = 44100.0;
    int    blockSize_  = 0;
    bool   prepared_   = false;

    FilterTopology    topology_  = FilterTopology::FOUR_POLE_LADDER;
    AnalogFilterMode  mode_      = AnalogFilterMode::Lowpass;
    float             cutoffHz_  = 1000.0f;
    float             resonance_ = 0.0f;

    // Param / topology dirtiness tracking for the control-rate commit() contract.
    bool dirty_            = true;
    bool topologyChanged_  = true;

    // 4-pole LM13700 -> the JUCE ladder (LPF24, aggressive / self-oscillating).
    juce::dsp::LadderFilter<float> ladder_;

    // 4-pole SSM2164 -> a CUSTOM 4-stage cascaded one-pole lowpass with global
    // resonance feedback. This is a genuinely different topology from the JUCE
    // ladder (linear stages, no per-stage saturation -> smoother / 'politer'
    // resonance), matching the SSM2164 4-pole voicecard character. Always LP.
    struct Ssm2164Cascade
    {
        double sampleRate = 44100.0;
        float  coeff      = 0.0f;   // per-stage one-pole RC coefficient
        float  s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;  // stage states
        float  fbGain     = 0.0f;   // resonance feedback amount
        float  cutoff     = 1000.0f;

        void prepare (double sr) { sampleRate = sr; reset(); updateCoeff(); }
        void reset() { s0 = s1 = s2 = s3 = 0.0f; }
        void setCutoff (float hz) { cutoff = hz; updateCoeff(); }
        void setResonance01 (float r01) { fbGain = juce::jlimit (0.0f, 3.3f, r01 * 3.3f); }
        void updateCoeff()
        {
            // One-pole RC TPT coefficient: a = 1 - exp(-2*pi*fc/fs). Stable, in (0,1).
            const double wc = 6.283185307179586 * cutoff / sampleRate;
            coeff = static_cast<float> (1.0 - std::exp (-wc));
        }
        float process (float in)
        {
            const float u = in - fbGain * s3;   // global resonance feedback
            s0 += coeff * (u  - s0);
            s1 += coeff * (s0 - s1);
            s2 += coeff * (s1 - s2);
            s3 += coeff * (s2 - s3);
            // Guard against runaway at extreme resonance (the linear cascade can
            // approach self-oscillation; clamp rather than NaN).
            if (! std::isfinite (s3)) { s0 = s1 = s2 = s3 = 0.0f; }
            return s3;
        }
    } ssm_;

    // 2-pole SVF. The JUCE TPT filter exposes a single output tap, so:
    //   * LP/BP/HP use svf_ (its type switched to lowpass/bandpass/highpass).
    //   * NOTCH is approximated as lowpass(svf_) + highpass(svfNotch_),
    //     matching the SVF identity notch = low + high. (Per spec E.4 the notch
    //     is approximate; this stays entirely within juce::dsp::StateVariableTPTFilter.)
    juce::dsp::StateVariableTPTFilter<float> svf_;
    juce::dsp::StateVariableTPTFilter<float> svfNotch_; // fixed highpass, used only for notch
};

} // namespace ambika::dsp

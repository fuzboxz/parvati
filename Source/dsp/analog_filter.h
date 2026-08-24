// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
// Ambika analog-filter emulation. Original Ambika firmware (C) Emilie Gillet, GPL3.
//
// There is NO filter code in the Ambika firmware: the filter is ANALOG hardware
// (one per voicecard). The voice DSP computes cutoff / resonance (8-bit) and a
// 2-bit filter mode and sends them to a DAC + parallel port. This module
// emulates that analog filter in software using juce::dsp, fresh-written.
//
// Four voicecard topologies (see docs/DSP_PORT_SPEC.md section E):
//   * 4-pole Ladder                 -> juce::dsp::LadderFilter, LPF24 (internal tanh saturation;
//                                   controllable Drive scales the saturator -> bass-drop at high Q).
//   * 4-pole SSM2164 ("4P")       -> TWO juce::dsp::StateVariableTPTFilter (lowpass) IN SERIES,
//                                   cutoff+resonance linked — a linear 24 dB/oct baseline.
//   * 2-pole SVF (SSM2164)        -> juce::dsp::StateVariableTPTFilter (LP/BP/HP, NOTCH = low+high)
//   * 4-pole OTA ("SMR4")          -> custom OTA-cascade model (this repo), NOT juce::dsp. Four
//                                   LM13700-style stages; each integrates gm*tanh(error/2Vt).
//                                   Circuit-informed behavioural model: per-stage tanh on the OTA
//                                   error, datasheet 2Vt knee (52 mV normalized), loop-gain-
//                                   normalised resonance with its own VCA soft clip. NOT a
//                                   component-level SPICE model.

#pragma once

#include <juce_dsp/juce_dsp.h>

namespace ambika::dsp {

// Selectable voicecard filter topology.
enum class FilterTopology {
    FOUR_POLE_LADDER,   // 4-pole Ladder.  juce::dsp::LadderFilter LPF24 (tanh saturation; Drive control). Self-oscillating.
    FOUR_POLE_SSM2164,  // 4-pole ("4P").  TWO juce::dsp::StateVariableTPTFilter (lowpass) in series, cutoff+resonance linked. Linear baseline. Always LP.
    TWO_POLE_SVF,       // 2-pole state-variable (SSM2164).  juce::dsp::StateVariableTPTFilter (LP/BP/HP, NOTCH = low+high).
    FOUR_POLE_OTA       // 4-pole SMR4 OTA cascade (custom model). Four tanh OTA stages, 2Vt knee, normalised resonance VCA. Always LP. Self-oscillates at resonance 1.0.
};

// Local filter-mode enum. Values match common/patch.h FilterMode
// (FILTER_MODE_LP=0, _BP=1, _HP=2, _NOTCH=3). Kept as a *distinct* identifier
// (AnalogFilterMode) so this module stays independent of dsp/patch.h during the
// parallel porting phase; integration reconciles it with patch.h::FilterMode.
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

    // Saturation drive. Scales the tanh saturator of the Ladder card (1.2 ==
    // the juce::dsp::LadderFilter default) and the OTA knee of the SMR4 card
    // (knee = 2Vt / drive). Cached; applied on the next commit().
    void setDrive (float newDrive) { drive_ = newDrive; dirty_ = true; }

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
    // Ladder saturation drive. Default 1.2 == the juce::dsp::LadderFilter ctor
    // default, so the pre-control sound is preserved when Drive is untouched.
    // The OTA card maps drive to the inverse knee (2Vt / drive).
    float             drive_     = 1.2f;

    // Param / topology dirtiness tracking for the control-rate commit() contract.
    bool dirty_            = true;
    bool topologyChanged_  = true;

    // 4-pole Ladder -> the JUCE ladder (LPF24, tanh saturation + Drive).
    // LadderTap exposes LadderFilter's PROTECTED per-sample hooks so the
    // per-sample path can call them directly. JUCE's public process() loops
    // `updateSmoothers(); processSample (input, ch);` once per sample — the
    // tap reproduces exactly that sequence for one mono channel, so the
    // per-sample path is behavior-identical to the legacy 1-sample
    // AudioBlock + process() routing WITHOUT constructing a block + context
    // per sample (~3.8M wrapper calls/s at 96-voice polyphony).
    struct LadderTap : juce::dsp::LadderFilter<float>
    {
        using juce::dsp::LadderFilter<float>::processSample;
        using juce::dsp::LadderFilter<float>::updateSmoothers;
    };
    LadderTap ladder_;

    // 4-pole ("4P") -> TWO series StateVariableTPTFilter (both lowpass), cutoff+
    // resonance LINKED (per the modeling spec: a linear 24 dB/oct baseline). The
    // cascade is lowpass-only (the voice forces mode 0 for 4-pole cards).
    juce::dsp::StateVariableTPTFilter<float> svf4p_[2];

    // 2-pole SVF. The JUCE TPT filter exposes a single output tap, so:
    //   * LP/BP/HP use svf_ (its type switched to lowpass/bandpass/highpass).
    //   * NOTCH is approximated as lowpass(svf_) + highpass(svfNotch_),
    //     matching the SVF identity notch = low + high. (Per spec E.4 the notch
    //     is approximate; this stays entirely within juce::dsp::StateVariableTPTFilter.)
    juce::dsp::StateVariableTPTFilter<float> svf_;
    juce::dsp::StateVariableTPTFilter<float> svfNotch_; // fixed highpass, used only for notch

    // ---- 4-pole OTA cascade (SMR4) -------------------------------------------
    // One sample of the OTA model. x = input. Returns the 4th stage output.
    float processOTASample (float x) noexcept;
    // Stage states = capacitor voltages (one per OTA pole).
    float otaState_[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    // Coefficients, recomputed in commit()/applyParams() (double math), stored
    // as float for the per-sample path:
    //   gLin_  exact TPT pole conductance tan(pi*fc/fs). Small-signal slope of
    //          every stage tanh is normalised to exactly this (unit-slope form
    //          gk*tanh(v/knee) with gk = gLin*knee), so the cutoff tuning is
    //          exact and knee only sets the saturation threshold.
    //   G_     gLin/(1+gLin): linearised one-pole gain used by the feedback solve.
    //   G2..4  powers of G for the sigma sum.
    //   gk_    gLin*knee: the coefficient that multiplies the tanh.
    //   knee_  2Vt normalized (0.052) / drive. invKnee_ is its reciprocal.
    //   kfb_   resonance feedback gain: kfb = res * 4.0. The factor 4 is the
    //          EXACT self-oscillation onset of four identical bilinear
    //          one-pole stages (Routh on the analog quartic, preserved by the
    //          bilinear transform — see the .cpp proof). The onset therefore
    //          sits exactly at resonance 1.0 for every cutoff.
    //   invOnePlusR_  1/(1 + kfb*G^4) for the linearised delay-free-loop solve.
    float gLin_ = 0.0f, G_ = 0.0f, G2_ = 0.0f, G3_ = 0.0f, G4_ = 0.0f;
    float gk_ = 0.0f, knee_ = 1.0f, invKnee_ = 1.0f, kfb_ = 0.0f, invOnePlusR_ = 1.0f;
    // Resonance-VCA soft clip: unit-slope form kfb*2Vt*tanh(y4/2Vt).
    float kfbVca_ = 0.0f;
    // 2Vt of the LM13700 in normalized units (52 mV at 1.0 == 1 V full scale).
    static constexpr float kTwoVt = 0.052f;
    static constexpr float kInvTwoVt = 1.0f / 0.052f;
    // Hard cap on kfb_ (numeric guard only; kOnset <= ~4 keeps kfb small).
    static constexpr double kKfbHardMax = 1.0e12;
};

} // namespace ambika::dsp

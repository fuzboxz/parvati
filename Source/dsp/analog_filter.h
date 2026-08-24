// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
// Ambika analog-filter emulation. Original Ambika firmware (C) Emilie Gillet, GPL3.
//
// There is NO filter code in the Ambika firmware: the filter is ANALOG hardware
// (one per voicecard). The voice DSP computes cutoff / resonance (8-bit) and a
// 2-bit filter mode and sends them to a DAC + parallel port. This module
// emulates that analog filter in software using juce::dsp, fresh-written.
//
// Six voicecard topologies (see docs/DSP_PORT_SPEC.md section E):
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
//   * 2-pole Polivoks SVF          -> custom ZDF SVF model (this repo), NOT juce::dsp. The Soviet
//                                   Polivoks form: an op-amp character layer on the exact
//                                   R = 2*(1-res) skeleton (char-poly proof in the .cpp):
//                                   asymmetric hard-shoulder rails (even harmonics), a harder
//                                   diode-style limiter on the resonance return, Q-dependent
//                                   damping sag, input offset, asymmetric rate-limited outputs,
//                                   supply clamp on the states. LP + BP outputs. Documented-
//                                   behaviour-derived calibration: no reference schematic exists
//                                   in the tree (a Formanta community card, not an Ambika
//                                   voicecard).
//   * 4-pole IR3109 ("Juno-60/106-class") -> the SAME OTA-cascade structure as the SMR4 card: a
//                                   calibrated SIBLING, not a new structure. Three character
//                                   deltas: a higher stage knee (0.10: the filter stays polite
//                                   until driven), a milder resonance-feedback clip (3x the
//                                   knee), and kfb capped at 3.4 — BELOW the exact 4.0 onset —
//                                   so this card never self-oscillates (the factory-capped
//                                   Juno character). Honest note: the Juno sound also owes
//                                   much to its BBD chorus, which a filter card cannot carry
//                                   (a chorus FX is possible future work).

#pragma once

#include <juce_dsp/juce_dsp.h>

namespace ambika::dsp {

// Selectable voicecard filter topology.
enum class FilterTopology {
    FOUR_POLE_LADDER,   // 4-pole Ladder.  juce::dsp::LadderFilter LPF24 (tanh saturation; Drive control). Self-oscillating.
    FOUR_POLE_SSM2164,  // 4-pole ("4P").  TWO juce::dsp::StateVariableTPTFilter (lowpass) in series, cutoff+resonance linked. Linear baseline. Always LP.
    TWO_POLE_SVF,       // 2-pole state-variable (SSM2164).  juce::dsp::StateVariableTPTFilter (LP/BP/HP, NOTCH = low+high).
    FOUR_POLE_OTA,      // 4-pole SMR4 OTA cascade (custom model). Four tanh OTA stages, 2Vt knee, normalised resonance VCA. Always LP. Self-oscillates at resonance 1.0.
    TWO_POLE_POLIVOKS,  // 2-pole Polivoks SVF (custom model). Op-amp character layer: asymmetric hard-shoulder rails, diode-style resonance limiting, Q-dependent damping sag, input offset, rate-limited outputs. LP + BP. HP/Notch clamp to LP. Self-oscillates at resonance 1.0.
    FOUR_POLE_IR3109    // 4-pole IR3109 (Juno-60/106-class) OTA cascade. Same structure as the SMR4, calibrated sibling: higher knee (0.10), milder resonance clip, kfb capped at 3.4 below the 4.0 onset. Never self-oscillates. Always LP.
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
    // the juce::dsp::LadderFilter default), the OTA knee of the SMR4 card
    // (knee = 2Vt / drive), and the clip and rate limits of the Polivoks
    // card (more drive clips lower and slews slower). Cached; applied on the
    // next commit().
    void setDrive (float newDrive) { drive_ = newDrive; dirty_ = true; }

    // TEST-ONLY: bypass the Polivoks character layer. The filter then runs
    // the pure linear skeleton. The character tests render this reference.
    // No audio path sets it.
    void setTestLinearBypass (bool b) { pvTestLinear_ = b; }

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

    // ---- 4-pole OTA cascade family (SMR4 / IR3109) ----------------------------
    // One sample of the OTA model. x = input. Returns the 4th stage output.
    // The two cards share this code; only the coefficients differ.
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
    //   knee_  the card knee base divided by drive. SMR4: 2Vt (0.052).
    //          IR3109: 0.10 (higher headroom). invKnee_ is its reciprocal.
    //   kfb_   resonance feedback gain: kfb = res * kfbMax. For the SMR4 the
    //          factor 4 is the
    //          EXACT self-oscillation onset of four identical bilinear
    //          one-pole stages (Routh on the analog quartic, preserved by the
    //          bilinear transform — see the .cpp proof). The onset therefore
    //          sits exactly at resonance 1.0 for every cutoff. The IR3109
    //          caps the factor at 3.4, BELOW the onset: the factory-capped
    //          Juno character (the card never self-oscillates).
    //   invOnePlusR_  1/(1 + kfb*G^4) for the linearised delay-free-loop solve.
    float gLin_ = 0.0f, G_ = 0.0f, G2_ = 0.0f, G3_ = 0.0f, G4_ = 0.0f;
    float gk_ = 0.0f, knee_ = 1.0f, invKnee_ = 1.0f, kfb_ = 0.0f, invOnePlusR_ = 1.0f;
    // Resonance-VCA soft clip: unit-slope form kfb*vcaKnee*tanh(y4/vcaKnee).
    // vcaKnee_ = 2Vt on the SMR4; kIr3109VcaKnee*2Vt on the IR3109 (milder).
    float kfbVca_ = 0.0f, vcaKnee_ = 1.0f, invVcaKnee_ = 1.0f;
    // 2Vt of the LM13700 in normalized units (52 mV at 1.0 == 1 V full scale).
    // The Polivoks reuses the same normalized knee scale: its op-amp
    // saturation uses the same behavioural constant, scaled by Filter Drive.
    static constexpr float kTwoVt = 0.052f;
    static constexpr float kInvTwoVt = 1.0f / 0.052f;
    // Hard cap on kfb_ (numeric guard only; kOnset <= ~4 keeps kfb small).
    static constexpr double kKfbHardMax = 1.0e12;
    // IR3109 (Juno-60/106-class) calibration. The card shares the OTA-cascade
    // structure with the SMR4; these constants make the character:
    //   kIr3109TwoVt   0.10: a higher stage knee than the SMR4's 0.052. The
    //                  stages hold more headroom, so the filter stays polite
    //                  until driven.
    //   kIr3109KfbMax  3.4: the resonance factor cap, BELOW the exact 4.0
    //                  onset. The factory cap: this card never screams.
    //   kIr3109VcaKnee 3.0: the feedback clip knee in units of 2Vt. A milder
    //                  resonance path: the high-Q character reads thinner.
    static constexpr double kIr3109TwoVt   = 0.10;
    static constexpr double kIr3109KfbMax  = 3.4;
    static constexpr double kIr3109VcaKnee = 3.0;

    // ---- 2-pole Polivoks SVF --------------------------------------------------
    // One sample of the Polivoks model. x = input. Returns LP or BP per mode.
    float processPolivoksSample (float x) noexcept;
    // Integrator states: pvS1_ = bandpass, pvS2_ = lowpass.
    float pvS1_ = 0.0f, pvS2_ = 0.0f;
    // Rate-limited node values from the previous sample. The op-amp output
    // nodes track the solved values with a per-sample rate cap.
    float pvPrevBp_ = 0.0f, pvPrevLp_ = 0.0f;
    // True when the active mode is Bandpass (set in applyParams). Highpass and
    // Notch clamp to LP: the real card has no such outputs.
    bool pvBandpass_ = false;
    // TEST-ONLY hook: run the pure linear skeleton (no shapers, no sag, no
    // offset, no rate limit). The character tests measure against this
    // reference. No audio path sets it.
    bool pvTestLinear_ = false;

    // C1 saturating shaper, one signal polarity. Slope 1 below the knee, a
    // cubic ease to slope s across width w, a hard shoulder of length sh at
    // slope s, a cubic ease to slope 0 across railT, then a flat rail at
    // value phi3. The flat rail models the supply clamp of an op-amp output
    // stage and gives the solver a closed bracket.
    struct PvShaper
    {
        float knee = 1.0f, w = 0.25f, sh = 0.5f, railT = 0.04f, s = 0.08f;
        float phi1 = 0.0f, phi2 = 0.0f, phi3 = 1.0f;   // region boundary values
        // Computes w, sh, railT, phi1..phi3 from knee and s (double math).
        void setup (double kneeIn, double sIn) noexcept;
        // v >= 0. Returns the shaper value.
        float eval (float v) const noexcept;
        // v >= 0. Returns the shaper slope in [0, 1].
        float slope (float v) const noexcept;
    };
    // Op-amp stage shapers (asymmetric: the negative side clips lower).
    PvShaper pvShapeP_, pvShapeN_;
    // Resonance-return shaper. Harder and lower: the diode-limited feedback
    // path of the resonance control breaks up before the stages clip.
    PvShaper pvReturnP_, pvReturnN_;
    // Signed shaper helpers (positive side uses the P constants).
    float pvPhi (float v) const noexcept;
    float pvPhiSlope (float v) const noexcept;
    float pvReturn (float v) const noexcept;
    float pvReturnSlope (float v) const noexcept;

    // Coefficients, recomputed in applyParams() (double math), stored as float:
    //   pvGLin_ exact TPT integrator conductance tan(pi*fc/fs).
    //   pvR_    linear damping R = 2*(1 - res). R = 0 at res = 1.0 is the EXACT
    //           self-oscillation onset of the linearised loop at every cutoff
    //           (proof in the .cpp). Analog Q = 1/R: res 0 -> Q 0.5 (damped),
    //           res -> 1 -> Q -> infinity (onset).
    //   pvKrg_  sagged damping R*(1 - sag*res^2), sag = 0.12. High resonance
    //           removes damping beyond the linear map, so the loop breaks up
    //           into clipping instead of a clean limit cycle. At res = 1.0 the
    //           sag multiplies R = 0, so the onset does not move.
    //   pvInvDen_     1/(1 + g*kR + g*g): linearised solve with the sagged R.
    //   pvInvDenLin_  1/(1 + g*R + g*g): pure linear solve (test reference).
    //   pvDc_   input offset, 0.8 percent of the positive knee. Op-amp bias
    //           current. The asymmetric rails turn it into drift and thump.
    //   pvSlewP_ positive per-sample rate cap at unity drive. The negative
    //           cap is 75 percent of it. Both divide by the drive factor, so
    //           more drive lowers the cap. A signal inside the linear zone
    //           sees a cap pvSlewFloor_ times wider (the gate holds at 0.12).
    float pvGLin_ = 0.0f, pvR_ = 2.0f, pvKrg_ = 2.0f, pvInvDen_ = 1.0f, pvInvDenLin_ = 1.0f;
    float pvDc_ = 0.0f, pvSlewP_ = 1.0f;
    float pvKneeP_ = 1.0f, pvInvTwoKneeP_ = 0.5f, pvKneeN_ = 1.0f, pvInvTwoKneeN_ = 0.5f;
    // Supply clamp on both integrator states: a fixed absolute value. The
    // drive moves the shaper knees, not the supply. See the .cpp step 5.
    static constexpr float pvNodeClamp_ = 4.0f;
    static constexpr float pvSlewFloor_ = 0.12f;
    static constexpr float pvSlewDown_ = 0.75f;
    // Newton iteration cap. The bracket [s1 - g*|satN|, s1 + g*satP] halves
    // per fallback step, so 24 iterations reach the float noise floor.
    static constexpr int kPvMaxIters = 24;
};

} // namespace ambika::dsp

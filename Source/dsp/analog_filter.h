// Copyright (c) 2026 805Labs Kft. / Hellcat.
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
//                                   per-stage Q = 0.5*(1-res)^(-0.616): the exact 24 dB/oct cascade
//                                   baseline at knob 0; the peak tracks the family cluster.
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
//
// LOUDNESS PARITY (2026-08-25 calibration): the six cards play together, so
// their output levels must match roughly at equal settings. Four measures:
//   1. Resonance maps share ONE law per pole count (see below). The knob is
//      never a raw Q: the 2-pole cards use Q = 1/(2*(1-res)) (the Polivoks
//      law; Q 0.5 at knob 0, onset at 1.0), the 4-pole cascade uses
//      q = 0.5*(1-res)^(-0.616) per stage (EXACT 4-pole baseline at knob 0;
//      the peak lands on the 2-pole family value, +20 dB at knob 0.95).
//      The Ladder knob passes ladderResonanceKnob(). JUCE maps knob r to
//      feedback k = 0.4 + 3.6*r. The remap inverts that offset. So the
//      feedback becomes k = 4*knob above knob 0.1. The pre-parity raw-Q
//      mapping left the SVF and 4P cards droopy and silent (up to 38 dB
//      under the siblings).
//   2. A static per-card output trim (the k*CardGain constants) levels the
//      passband against the Ladder (the calibration reference card).
//   3. The feedback cards (SMR4, IR3109) add a partial resonance
//      compensation (1+kfb)^exp: high resonance thins their bass by
//      1/(1+kfb), and their saturating stages sag further. The compensation
//      keeps them near the Q-based cards (which keep DC gain 1).
//   4. The OTA core runs on a 2 V signal scale (kOtaCoreScale) and its
//      output passes a 10 Hz DC blocker: the pre-parity calibration fed the
//      cores ~20x past the 2Vt knee (a rectified DC point down to -0.33 and
//      a low-cutoff AC collapse up to 10 dB under the family).
// Remaining spread at the extremes (self-oscillation at res 1.0, sub-300 Hz
// cutoffs on the saturating cards, Filter Drive 12) is card character,
// pinned by the filter_loudness_test bounds.
enum class FilterTopology {
    FOUR_POLE_LADDER,   // 4-pole Ladder.  juce::dsp::LadderFilter LPF24 (tanh saturation; Drive control). Self-oscillating.
    FOUR_POLE_SSM2164,  // 4-pole ("4P").  TWO juce::dsp::StateVariableTPTFilter (lowpass) in series, per-stage Q = 0.5*(1-res)^(-0.616): the exact 24 dB/oct cascade baseline at knob 0, peak lands on the family cluster (+20 dB at knob 0.95). Always LP.
    TWO_POLE_SVF,       // 2-pole state-variable (SSM2164).  juce::dsp::StateVariableTPTFilter (LP/BP/HP, NOTCH = low+high). Q = 1/(2*(1-res)), the same law as the Polivoks card.
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
    // 4P cascade peak-law exponent (2026-08-26 harmonization). Per-stage
    // Q = 0.5*(1-res)^(-kSsm4PeakExp). Any power law keeps q(0) = 0.5, the
    // EXACT cascade baseline (-12.04 dB at the cutoff). This exponent solves
    // q(0.95)^2 = 10.0 (+20 dB): the 2-pole family peak at knob 0.95. The
    // earlier sqrt law (exponent 0.5) gave q^2 = 0.25/(1-res) = 5 (+14 dB),
    // 6..8 dB under the family cluster. Derivation: 0.05^(-E) = 2*sqrt(10)
    // gives E = ln(2*sqrt(10))/ln(20) = 0.6158 ~= 0.616.
    static constexpr double kSsm4PeakExp = 0.616;

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
    // (knee = (2Vt/1.2) * cbrt(1.2/drive): anchored at drive 1.2. A linear
    // law dropped the saturated output ~16 dB at drive 12, far below every
    // other card), and the clip and rate limits of the Polivoks card.
    // Cached; applied on the next commit().
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
    // Output loudness trim the filter currently applies (static card trim x
    // resonance compensation). Read by the loudness parity test.
    float          getLoudnessGain() const noexcept { return loudnessGain_; }

    // Map the firmware 8-bit cutoff (0..255) to Hz, exponential across ~20..16k.
    // Provided as a convenience; the primary path is setCutoffHz() driven by the
    // Voice (which already folds in keytracking + env + lfo before scaling).
    static float cutoffByteToHz (uint8_t cutoffByte);

    // Remaps the resonance knob for the JUCE ladder. JUCE maps knob r to
    // feedback k = 0.4 + 3.6*r internally; the ideal 4-pole law is k = 4*r.
    // Returns the JUCE knob whose feedback equals the ideal law:
    // (4*r - 0.4)/3.6, clamped to [0, 1]. Above knob 0.1 the ladder tracks
    // k = 4*knob. Below 0.1 JUCE cannot go under k = 0.4: a dead zone holds
    // that floor (peak -11.1 dB at the cutoff instead of the ideal -12.04 dB).
    static float ladderResonanceKnob (float knob);

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
    // The OTA cards map drive to the softened knee law (see applyParams).
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
    // One sample of the OTA model. x = input. Returns the 4th stage output
    // (before the DC blocker and the loudness trim, both in processSample).
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
    //   knee_  the card knee base scaled by the drive law (1.2/drive)^(1/3),
    //          anchored at drive 1.2 (a linear law collapsed the loudness at
    //          high drive; see setDrive). SMR4 base: 2Vt (0.052). IR3109
    //          base: 0.10. invKnee_ is its reciprocal.
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
    // vcaKnee_ = 2Vt*kOtaCoreScale on the SMR4; kIr3109VcaKnee*2Vt*
    // kOtaCoreScale on the IR3109 (milder). The core scale rides both knees.
    float kfbVca_ = 0.0f, vcaKnee_ = 1.0f, invVcaKnee_ = 1.0f;
    // Output loudness trim: static card calibration times the resonance
    // compensation (1+kfb)^exp. Computed in applyParams(), applied on every
    // processSample() return. The ladder keeps 1.0 (its JUCE path is pinned
    // bit-identical by hellcat_analog_filter_batch_test, and JUCE already
    // half-compensates its resonance bass drop via comp = 0.5).
    float loudnessGain_ = 1.0f;
    // DC blocker for the OTA family (SMR4 / IR3109): a one-pole highpass at
    // ~10 Hz. The saturating cascade RECTIFIES an asymmetric waveform (a saw
    // within its period): measured DC operating points reach -0.26..-0.33 at
    // hot levels, which steals headroom and sums across the 6 voices. A real
    // card's output stage is AC-referenced; this models that coupling. The
    // state and coefficient live here; the pole tracks the sample rate.
    float otaDcState_  = 0.0f;
    float otaDcCoeff_  = 0.001f;   // 1 - exp(-2*pi*10Hz/fs), set in applyParams
    // 2Vt of the LM13700 in normalized units (52 mV at 1.0 == 1 V full scale).
    // The Polivoks reuses the same normalized knee scale: its op-amp
    // saturation uses the same behavioural constant, scaled by Filter Drive.
    static constexpr float kTwoVt = 0.052f;
    // OTA CORE SIGNAL SCALE (2026-08-25 loudness calibration). The physical
    // stage knee stays the LM13700 2Vt (0.052); this factor sets the signal
    // scale AT THE CORE. The card's input network attenuates the DAC signal
    // before the summing node. So 1.0 full-scale at the core equals
    // kOtaCoreScale volts, not 1 V. The pre-2026-08-25 calibration (1.0)
    // drove every stage ~20x past its knee at hot levels. The rectified DC
    // operating point reached -0.33. The low-cutoff AC output collapsed up
    // to 10 dB under the sibling cards. Value 2.0 keeps the saturation
    // audible: a hot saw still clips the first stages. The card still tracks
    // the family loudness. The scale applies to the stage knee AND the
    // resonance VCA knee. So every ratio-based character pin stays unchanged
    // (knee ratios, harmonic deltas, self-oscillation semantics).
    static constexpr double kOtaCoreScale = 2.0;
    // Hard cap on kfb_ (numeric guard only; kOnset <= ~4 keeps kfb small).
    static constexpr double kKfbHardMax = 1.0e12;

    // ---- Loudness parity calibration (see the topology comment above) ------
    // Static per-card output trims plus the feedback-card resonance
    // compensation exponent, measured on the reference program (band-limited
    // saw 110 Hz at amp 0.9, 24 partials, drive 1.2, 48 kHz, AC RMS) against
    // the Ladder card (the calibration reference) across cutoff 500 Hz..6 kHz x resonance
    // 0..0.95. Measured result: spread <= 4.5 dB over that band (2.0 dB at
    // the neutral setting), <= 5.9 dB at resonance 0.95, <= 4.8 dB at Filter
    // Drive 12. Pin: filter_loudness_test.
    static constexpr float kLadderCardGain   = 1.00f;   // reference (JUCE path pinned bit-identical)
    static constexpr float kSsm2164CardGain  = 0.78f;   // -2.2 dB
    static constexpr float kSvfCardGain      = 0.78f;   // -2.2 dB
    static constexpr float kSmr4CardGain     = 0.93f;   // -0.6 dB (x resonance compensation)
    static constexpr float kPolivoksCardGain = 0.74f;   // -2.6 dB
    static constexpr float kIr3109CardGain   = 1.00f;   // unity (x resonance compensation)
    // Resonance compensation exponent for the feedback cards: output scales
    // by (1+kfb)^exp. The IR3109 takes the stronger exponent: its factory
    // kfb cap suppresses the ring, so without the extra lift its program
    // loudness sags at high resonance (the never-screams character stays:
    // the cap is in kfb, not in this gain).
    static constexpr double kSmr4ResCompExp   = 0.50;
    static constexpr double kIr3109ResCompExp = 0.65;
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

// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// FxTypes — Parvati-exclusive per-part FX enums and constants. Included by
// SynthEngine.h (so engine/Preset/ParameterLayout code sees it) AND by the FX
// DSP core (FxProcessor.h/FxChain.h), which lets the DSP layer reference these
// types WITHOUT pulling in all of SynthEngine.h (avoiding a circular include:
// SynthEngine.h includes FxChain.h -> FxProcessor.h).
//
// Ambika knows nothing about these: they are deliberately NOT in dsp/patch.h
// (the firmware header). The CONTRACT places these in SynthEngine.h; this header
// is the dependency-free shard that makes the include graph acyclic while every
// SynthEngine.h consumer still reaches the same symbols.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

constexpr int kNumFxSlots       = 3;    // FX1/FX2/FX3
constexpr int kNumFxMatrixSlots = 16;   // separate FX mod matrix (no 14 cap)
constexpr int kNumFxSlotParams  = 5;    // generic params 1..5 per slot

// Effect type per slot (drives fx{N}_type choice).
enum class FxType : uint8_t {
    None = 0,
    Diffuser = 1, PitchShifter = 2, Reverb = 3,
    LoopingDelay = 4, WSOLAStretch = 5, Spectral = 6,
    Wavefolder = 7, FrequencyShifter = 8, RingModulator = 9,
    Resonator = 10,
    // FV-1 hardware-emulation family (Source/dsp/fx/fv1/). APPEND-ONLY like
    // the rest: fx{N}_type is an AudioParameterChoice whose stored index IS the
    // enum value, so inserting/reordering would remap existing presets.
    ClockedDelay = 11, Ensemble = 12, PlateReverb = 13,
    VinylCompressor = 14, Phaser = 15,
    // FV-1 family, second wave (2026-08-17): the standard + quirky
    // set. APPEND-ONLY (same serialization rule as above).
    Overdrive = 16, LutDistortion = 17, Compressor = 18, Gate = 19,
    Chorus = 20, Flanger = 21, Echo = 22, Room = 23, Spring = 24,
    // Dual-BBD Chorus (2026-08-24): the documented Roland Juno-60/106 chorus
    // configuration ported into the FV-1 family. APPEND-ONLY (same rule).
    JunoChorus = 25,
    Count
};
// choice list string: { "None", "Diffuser", "Pitch Shifter", "CVerb",
//                       "Looping Delay", "Pitch Stretch", "Spectral",
//                       "Wavefolder", "Frequency Shifter", "Ring Modulator",
//                       "Resonator",
//                       "Clocked Delay", "Ensemble", "Plate",
//                       "Vinyl Compressor", "Phaser",
//                       "Overdrive", "LUT", "Compressor", "Gate",
//                       "Chorus", "Flanger", "Digital Echo", "Room", "Spring",
//                       "Dual-BBD Chorus" }
//   (Diffuser / PitchShifter / Reverb are ports of the Mutable Instruments
//    Clouds `dsp/fx` chain; LoopingDelay / WSOLAStretch / Spectral are the Clouds
//    looping / WSOLA / phase-vocoder modes; Wavefolder / FrequencyShifter /
//    RingModulator are ports of the Mutable Instruments Warps DSP; Resonator is a
//    port of the Mutable Instruments Rings modal resonator; ClockedDelay /
//    Ensemble / PlateReverb / VinylCompressor / Phaser are the FV-1 hardware-
//    emulation family in dsp/fx/fv1/. New values are
//    APPEND-ONLY: fx{N}_type is an AudioParameterChoice whose stored index
//    IS the enum value, so inserting or reordering would remap existing presets
//    to the wrong effect.)

// Effect-chain topology (drives fx_topo choice). The three slots are taken in
// the current order permutation (order_[0], order_[1], order_[2]) — call them
// A, B, C below. A disabled slot is a passthrough in every topology.
enum class FxTopology : uint8_t {
    Series = 0,          // A -> B -> C  (each slot processes the running signal)
    Parallel12to3 = 1,   // (A || B) -> C   (A and B process the dry input in
                         //  parallel, equal-gain sum, then C processes the sum)
    Parallel1to23 = 2,   // A -> (B || C)   (A processes the dry input, then B and
                         //  C each process a copy of A's output, equal-gain sum)
    Count
};
// choice list string: { "Series", "Parallel 1+2->3", "Parallel 1->2+3" }

// FX mod-matrix destinations (drives fxmod{N}_dest choice). Distinct from
// MOD_DST_* (synth destinations). One dry/wet + five generic params per slot.
// Layout is CONSECUTIVE per slot (dryWet then P1..P5) so dest = slot*(kNumFxSlotParams+1)
// + field, matching the modOffset indexing in SynthEngine::renderPartFx.
enum FxModDestination : int {
    FX_DST_NONE = -1,
    FX_DST_FX1_DRYWET = 0, FX_DST_FX1_P1, FX_DST_FX1_P2, FX_DST_FX1_P3, FX_DST_FX1_P4, FX_DST_FX1_P5,
    FX_DST_FX2_DRYWET,     FX_DST_FX2_P1, FX_DST_FX2_P2, FX_DST_FX2_P3, FX_DST_FX2_P4, FX_DST_FX2_P5,
    FX_DST_FX3_DRYWET,     FX_DST_FX3_P1, FX_DST_FX3_P2, FX_DST_FX3_P3, FX_DST_FX3_P4, FX_DST_FX3_P5,
    FX_DST_LAST
};   // 18 destinations (3 slots x (1 dry/wet + 5 params))

// orderIdx 0..5 -> permutation of {0,1,2} (the FX slot process order).
inline std::array<int, 3> fxOrderPermutation (uint8_t idx)
{
    switch (idx)
    {
        default: case 0: return { 0, 1, 2 };
        case 1: return { 0, 2, 1 };
        case 2: return { 1, 0, 2 };
        case 3: return { 1, 2, 0 };
        case 4: return { 2, 0, 1 };
        case 5: return { 2, 1, 0 };
    }
}

// ----------------------------------------------------------------------------
// Effect categories — DISPLAY-ONLY grouping for the FX-slot type dropdown
// (helps the user find which effect does what). The APVTS choice list (host surface)
// stays FLAT and enum-ordered: the stored value is the enum/choice index, so
// categories never touch serialization, host automation, or the < > step
// buttons. Only Parvati's own FxTypeCombo popup presents the grouped order.
enum class FxCategory : uint8_t {
    None = 0,        // the None slot — uncategorized, always listed first
    Delay,           // repeats/echoes of the past
    Distortion,      // waveshaping / harmonics generation
    Dynamics,        // level control / compression / leveling
    Mod,             // periodic modulation of the signal
    PitchTime,       // pitch + time-domain transformation
    Reverb           // space / diffusion / decay
};
// NOTE: the enum order above IS the alphabetical category display order
// (Delay < Distortion < Dynamics < Mod < Pitch/Time < Reverb) — keep it that
// way if values are ever appended.

inline const char* fxCategoryName (FxCategory c) noexcept
{
    switch (c)
    {
        case FxCategory::Delay:      return "Delay";
        case FxCategory::Distortion: return "Distortion";
        case FxCategory::Dynamics:   return "Dynamics";
        case FxCategory::Mod:        return "Mod";
        case FxCategory::PitchTime:  return "Pitch/Time";
        case FxCategory::Reverb:     return "Reverb";
        case FxCategory::None:       break;
    }
    return "";
}

// Category of each effect (see docs/FX_FV1_DESIGN.md + FxProcessors.h for the
// per-effect signal paths backing these):
//   Diffuser -> Reverb (an allpass diffusion smear — the front half of a
//     reverb; the place a user who wants "that smeary space sound" looks),
//   PitchShifter/WSOLA/Spectral/Resonator -> Pitch/Time (the Resonator is the
//     filter-domain dual of a tuned delay/comb network — its defining control
//     is Pitch: it re-rings the input at the tuned pitch),
//   Ensemble/Phaser/FrequencyShifter/RingModulator -> Mod,
//   Wavefolder -> Distortion, VinylCompressor -> Dynamics,
//   ClockedDelay/LoopingDelay -> Delay, Reverb/PlateReverb -> Reverb.
inline FxCategory fxCategoryOf (FxType t) noexcept
{
    switch (t)
    {
        case FxType::Diffuser:        return FxCategory::Reverb;
        case FxType::PitchShifter:    return FxCategory::PitchTime;
        case FxType::Reverb:          return FxCategory::Reverb;
        case FxType::LoopingDelay:    return FxCategory::Delay;
        case FxType::WSOLAStretch:    return FxCategory::PitchTime;
        case FxType::Spectral:        return FxCategory::PitchTime;
        case FxType::Wavefolder:      return FxCategory::Distortion;
        case FxType::FrequencyShifter:return FxCategory::Mod;
        case FxType::RingModulator:   return FxCategory::Mod;
        case FxType::Resonator:       return FxCategory::PitchTime;
        case FxType::ClockedDelay:    return FxCategory::Delay;
        case FxType::Ensemble:        return FxCategory::Mod;
        case FxType::PlateReverb:     return FxCategory::Reverb;
        case FxType::VinylCompressor: return FxCategory::Dynamics;
        case FxType::Phaser:          return FxCategory::Mod;
        case FxType::Overdrive:       return FxCategory::Distortion;
        case FxType::LutDistortion:   return FxCategory::Distortion;
        case FxType::Compressor:      return FxCategory::Dynamics;
        case FxType::Gate:            return FxCategory::Dynamics;
        case FxType::Chorus:          return FxCategory::Mod;
        case FxType::JunoChorus:       return FxCategory::Mod;
        case FxType::Flanger:         return FxCategory::Mod;
        case FxType::Echo:            return FxCategory::Delay;
        case FxType::Room:            return FxCategory::Reverb;
        case FxType::Spring:          return FxCategory::Reverb;
        case FxType::None:
        case FxType::Count:     break;
    }
    return FxCategory::None;
}

// The DROPDOWN display order: None first, then categories alphabetically
// (the FxCategory enum order), effects alphabetically by display name within
// a category. DISPLAY ONLY — the ComboBox's internal items stay in ENUM order
// (ComboBoxAttachment syncs by item INDEX, so position == choice index there;
// the popup's action lambda maps back through this table). Pinned by
// parvati_clouds_fx_test (all types exactly once; category order ascending).
inline std::array<FxType, static_cast<size_t> (FxType::Count)> fxTypeDisplayOrder() noexcept
{
    return { {
        FxType::None,
        // Delay
        FxType::ClockedDelay, FxType::Echo, FxType::LoopingDelay,
        // Distortion
        FxType::LutDistortion, FxType::Overdrive, FxType::Wavefolder,
        // Dynamics
        FxType::Compressor, FxType::Gate, FxType::VinylCompressor,
        // Mod
        FxType::Chorus, FxType::JunoChorus, FxType::Ensemble, FxType::Flanger, FxType::FrequencyShifter,
        FxType::Phaser, FxType::RingModulator,
        // Pitch/Time
        FxType::PitchShifter, FxType::Resonator, FxType::Spectral, FxType::WSOLAStretch,
        // Reverb (CVerb is the DISPLAY label of FxType::Reverb — see makeFxTypes)
        FxType::Reverb, FxType::Diffuser, FxType::PlateReverb, FxType::Room, FxType::Spring
    } };
}

// ----------------------------------------------------------------------------
// Tail-length estimation — feeds AudioProcessor::getTailLengthSeconds() so
// hosts size their offline bounces / freeze tails correctly (a 0 tail makes
// Logic/Cubase truncate reverb+delay ends on export). PURE math, no audio, no
// state: unit-testable standalone. The estimate is the per-effect time to
// decay to -60 dB (t60) after the input stops, in SECONDS, from the raw
// normalized 0..1 slot params + the current transport BPM (tempo-synced
// delays only). The knob laws themselves live in fxlaw (next block), shared
// with the DSP setParams() implementations — one law, one definition.
//
// Feedback-decay law: a loop of period T seconds and per-pass gain g (0<g<1)
// reaches -60 dB after n = ln(1e-3)/ln(g) passes, so t60 = T * ln(1e-3)/ln(g).
// For g at/above unity (frozen/held loops) the tail is formally infinite; the
// caller-side cap applies.
inline constexpr double kTailFloorSeconds = 0.2;   // floor: release/release-ish minimum for pure-synth patches
inline constexpr double kTailCapSeconds   = 12.0;  // cap: frozen loops / near-unity feedback are formally infinite

// ----------------------------------------------------------------------------
// fxlaw — the shared FX knob laws. Each helper holds ONE law used by BOTH of
// its consumers: the DSP setParams() call sites (float arithmetic, audio
// path) and tailSecondsForFx (double arithmetic, host tail estimate). Every
// helper is a template: each caller instantiates it with its own arithmetic
// type, so float operation order, overload resolution and values at the DSP
// sites stay bit-identical to the former local expressions. History: two
// hand copies of one law drifted apart twice (CVerb loop length, Echo
// ping-pong period). Add new laws here; do not copy a law beside a caller.
// ----------------------------------------------------------------------------
namespace fxlaw
{
// FV-1 Echo: Time knob 0..1 -> 10..470 ms (one decade per 47^x).
template <typename T> T echoBaseMs (T p0) noexcept { return T (10.0) * std::pow (T (47.0), p0); }
// Echo feedback: 0..100% knob -> 0..0.995 loop gain (100% reads as infinite).
template <typename T> T echoFeedbackGain (T p1) noexcept { return p1 * T (0.995); }
// Echo R-line guard: the ping-pong lines are 2 x 16K samples; each half holds
// at most 16383 samples (one guard slot). Samples at the 32768 Hz FV-1 rate.
inline constexpr int echoHalfLineSamples = 16383;

// FV-1 Clocked Delay: Sync knob -> one of eight note divisions, then a
// tempo-locked base delay in SECONDS: T = (4/div) * (60/bpm). Double at both
// call sites (the DSP site converts to whole samples afterwards).
inline double clockedDelaySeconds (double pSync, double bpm) noexcept
{
    constexpr double kDiv[8] = { 1.0, 2.0, 3.0, 4.0, 6.0, 8.0, 12.0, 16.0 };
    int i = static_cast<int> (std::lround (pSync * 7.0));
    if (i < 0) i = 0;
    if (i > 7) i = 7;
    const double b = (bpm > 0.0) ? bpm : 120.0;
    return (4.0 / kDiv[static_cast<size_t> (i)]) * (60.0 / b);
}

// FV-1 modulated-delay family: loop period in seconds from the Center/Base
// knob (Ensemble 2..25 ms, Chorus 5..25 ms, Flanger 0.15..6.0 ms).
template <typename T> T ensembleLoopSeconds (T p2) noexcept { return (T (2.0) + p2 * T (23.0)) * T (1.0e-3); }
template <typename T> T chorusLoopSeconds  (T p2) noexcept { return (T (5.0) + p2 * T (20.0)) * T (1.0e-3); }
template <typename T> T flangerLoopSeconds (T p2) noexcept { return (T (0.15) + p2 * T (5.85)) * T (1.0e-3); }
// Their loop gains: Ensemble spans -0.9..0.9 (negative feedback rings the
// same), Chorus 0..0.5, Flanger 0..0.92.
template <typename T> T ensembleFeedbackGain (T p3) noexcept { return T (-0.9) + p3 * T (1.8); }
template <typename T> T chorusFeedbackGain  (T p3) noexcept { return p3 * T (0.5); }
template <typename T> T flangerFeedbackGain (T p3) noexcept { return p3 * T (0.92); }

// Dual-BBD Chorus (Juno port): mode constants and trims. ONE law serves the
// DSP setParams, the slot-card readout and the tail estimate. The mode
// rates/depths are the documented consensus midpoints (see Fv1JunoChorus.cpp
// for the source table); the center delay is the 1024-stage line at the
// documented ~20 kHz clock.
template <typename T> T junoCenterSeconds    () noexcept { return T (25.6e-3); }
template <typename T> T junoModeRateHz      (bool modeII) noexcept { return modeII ? T (1.13) : T (0.56); }
template <typename T> T junoModeDepthSeconds (bool modeII) noexcept { return (modeII ? T (4.0) : T (2.5)) * T (1.0e-3); }
// Rate trim: 0.5x..2x, log law, center 1.0x. Depth trim: 0..2x stock.
template <typename T> T junoRateTrim  (T p1) noexcept { return std::pow (T (2.0), (p1 - T (0.5)) * T (2.0)); }
template <typename T> T junoDepthTrim (T p2) noexcept { return p2 * T (2.0); }

// FV-1 reverbs: Decay knob 0..1 -> t60 in seconds (PlateReverb 0.1..4,
// Spring 0.2..4, Room 0.1..3). PlateReverb predelay spans 0..100 ms.
template <typename T> T plateDecaySeconds    (T p1) noexcept { return T (0.1) + p1 * (T (4.0) - T (0.1)); }
template <typename T> T springDecaySeconds    (T p0) noexcept { return T (0.2) + p0 * T (3.8); }
template <typename T> T roomDecaySeconds      (T p0) noexcept { return T (0.1) + p0 * T (2.9); }
template <typename T> T platePredelaySeconds  (T p0) noexcept { return p0 * T (0.1); }

// CVerb (Clouds Griesinger) tank feedback: Time knob 0..1 -> 0.30..0.95
// recirculation. Matches juce::jmap (v, 0.30f, 0.95f) = min + v*(max-min)
// bit-for-bit at float.
template <typename T> T cverbTankFeedback (T p) noexcept { return T (0.30) + p * (T (0.95) - T (0.30)); }
} // namespace fxlaw

namespace tail_detail
{
    // t60 of a feedback loop (see the law above). g <= 0 => single pass (no
    // feedback): the loop time itself. g >= 0.995 => effectively infinite.
    inline double feedbackTail (double loopSeconds, double g) noexcept
    {
        if (g <= 0.0)   return loopSeconds;
        if (g >= 0.995) return kTailCapSeconds;
        return loopSeconds * (std::log (1.0e-3) / std::log (g));
    }
}

inline double tailSecondsForFx (FxType type, const std::array<float, kNumFxSlotParams>& param, double bpm) noexcept
{
    const double b = (bpm > 0.0 && std::isfinite (bpm)) ? bpm : 120.0;
    const float p0 = param[0], p1 = param[1], p2 = param[2], p3 = param[3];

    switch (type)
    {
        // ---- FV-1 reverbs: the Decay knob IS the t60 (g = 10^(-3/(decay*fs))
        // is defined as -60 dB per `decay` seconds) + predelay where present.
        case FxType::PlateReverb:
            return fxlaw::plateDecaySeconds ((double) p1) + fxlaw::platePredelaySeconds ((double) p0);  // decay 0.1..4 s + predelay 0..100 ms
        case FxType::Spring:
            return fxlaw::springDecaySeconds ((double) p0);                // decay 0.2..4 s
        case FxType::Room:
            return fxlaw::roomDecaySeconds ((double) p0);                  // decay 0.1..3 s

        // ---- CVerb (Clouds Griesinger/Dattorro): tank feedback =
        // jmap(time, 0.30, 0.95). The tank is CROSS-COUPLED (loop A reads
        // del2@4680 -> dap1a(1653) -> dap1b(2038) -> writes del1@3410; loop B
        // reads del1@3410 -> dap2a(1913) -> dap2b(1663) -> writes del2@4680),
        // so a full recirculation walks BOTH loops:
        // 4680+1652+2037+3410+1912+1662 = 15353 samples @ 32000 Hz (the
        // engine's fixed internal rate, HostRateBridge.h) = 0.4798 s/pass;
        // plus the 0..200 ms predelay. (The old 8483-sample figure counted
        // one loop only -> tails under-reported ~1.8x.)
        case FxType::Reverb: {
            const double fb = fxlaw::cverbTankFeedback ((double) p2);
            return tail_detail::feedbackTail (15353.0 / 32000.0, fb) + (double) p0 * 0.20;
        }

        // ---- Delays (the user-facing requirement: delays count too).
        // FV-1 Echo: time 10*47^p0 ms (10..470 ms), feedback p1*0.995. The
        // recirculation is PING-PONG: tapR -> damp -> fb -> lineL(timeL) ->
        // tapL -> lineR(timeR) -> tapR, so the full loop period is
        // timeL + timeR = T*(2+p3), NOT T (timeR = T*(1+p3) via the Spread
        // knob). timeR is guarded to the 16383-sample half of the 2x16K
        // delay RAM — mirrored here exactly (matters at Time max + Spread).
        case FxType::Echo: {
            const double T = fxlaw::echoBaseMs ((double) p0) * 1.0e-3;
            const double timeR = std::min (T * (1.0 + (double) p3),
                                          (double) fxlaw::echoHalfLineSamples / 32768.0);
            return tail_detail::feedbackTail (T + timeR, fxlaw::echoFeedbackGain ((double) p1));
        }
        // FV-1 Clocked Delay: tempo-synced T = (4/div)*(60/bpm) with div from
        // round(pSync*7) over {1,2,3,4,6,8,12,16}, clamped to the 32768-sample
        // (1.0 s @ 32768 Hz) line; feedback pFb*0.95.
        case FxType::ClockedDelay: {
            double T = fxlaw::clockedDelaySeconds ((double) p0, b);
            if (T > 1.0) T = 1.0;                       // kMaxDelaySamples @ 32768 Hz
            return tail_detail::feedbackTail (T, (double) p1 * 0.95);
        }

        // ---- Clouds granular/looping family: 4 s capture buffer
        // (128000 @ 32000 Hz). A held loop (freeze) never decays -> the cap.
        case FxType::LoopingDelay:
            return (p3 > 0.5f) ? kTailCapSeconds : 4.0;
        case FxType::WSOLAStretch:
            return (p3 > 0.5f) ? kTailCapSeconds : 4.0;
        case FxType::Spectral:
            return (param[4] > 0.5f) ? kTailCapSeconds : 4.0;

        // ---- Diffuser: a 2048-sample allpass smear @ 32000 Hz (no feedback
        // loop — the energy is all first-pass).
        case FxType::Diffuser:
            return 2048.0 / 32000.0;

        // ---- FV-1 modulated-delay family: each is a genuine FEEDBACK loop
        // (write = lin + fb*read), so a high-feedback setting rings long
        // after the input stops — exactly the class the table exists for.
        // Ensemble: per-line loop at Center (2..25 ms), fb spans -0.9..0.9
        // (negative fb rings identically — decay depends on |fb|): max
        // Center + max |fb| -> ~1.64 s t60 (was the 0.2 s floor).
        case FxType::Ensemble: {
            const double T  = fxlaw::ensembleLoopSeconds ((double) p2);
            const double fb = std::fabs (fxlaw::ensembleFeedbackGain ((double) p3));
            return tail_detail::feedbackTail (T, fb);
        }
        // Chorus: same loop topology, Center 5..25 ms, fb 0..0.5 -> max
        // 0.25 s t60 (marginal over the floor, but correct).
        case FxType::Chorus: {
            const double T = fxlaw::chorusLoopSeconds ((double) p2);
            return tail_detail::feedbackTail (T, fxlaw::chorusFeedbackGain ((double) p3));
        }
        // Flanger: one line, one feedback loop through the 8 kHz damper
        // (DC gain 1, so g is the raw fb); the loop period is the
        // Manual-mapped base delay 0.15..6.0 ms (the LFO sweep averages to
        // zero around it) -> max ~0.50 s t60 at fb 0.92.
        case FxType::Flanger: {
            const double T = fxlaw::flangerLoopSeconds ((double) p2);
            return tail_detail::feedbackTail (T, fxlaw::flangerFeedbackGain ((double) p3));
        }
        // Dual-BBD Chorus (Juno port): OPEN lines, no feedback loop (the
        // source hardware mixes dry + wet with no regeneration). The tail is
        // the line delay itself (25.6 ms center + up to 4 ms sweep); the
        // caller-side floor covers the voice release.
        case FxType::JunoChorus:
            return fxlaw::junoCenterSeconds<double>() + fxlaw::junoModeDepthSeconds<double> (true);

        // ---- Resonator (Rings modal, NATIVE host rate): every mode decays
        // ~pi/q per sample with q = 500*10^(4*damping) (resonator.cc:63;
        // mode 0 keeps full q, q_loss only trims higher modes), so
        // t60 = ln(1e-3)*q/pi = 1099*10^(4*d) samples. The table is
        // rate-free, so normalize at 48 kHz (44.1 kHz runs ~8% longer —
        // small next to the cap). Damping is param[1] (the Decay
        // knob): 0.3 -> 0.36 s, 0.6 -> 5.8 s, 1.0 -> formally minutes -> cap.
        case FxType::Resonator: {
            const double t60 = 1099.0 * std::pow (10.0, 4.0 * (double) p1) / 48000.0;
            return t60 > kTailCapSeconds ? kTailCapSeconds : t60;
        }

        // ---- Everything else is memoryless/short (modulators, distortions,
        // dynamics, pitch): the engine tail is dominated by the voice release,
        // which the floor covers.
        case FxType::PitchShifter:
        case FxType::Wavefolder:
        case FxType::FrequencyShifter:
        case FxType::RingModulator:
        case FxType::Phaser:
        case FxType::VinylCompressor:
        case FxType::Overdrive:
        case FxType::LutDistortion:
        case FxType::Compressor:
        case FxType::Gate:
        case FxType::None:
        case FxType::Count:
            break;
    }
    return 0.0;
}

// Clamp an aggregate tail estimate into [floor, cap]. The FLOOR keeps a
// pure-synth patch reporting a small nonzero release tail (hosts render a
// sensible decay even with no FX); the CAP bounds formally-infinite cases
// (frozen loops, near-unity feedback) so bounces do not grow by minutes.
inline double clampTailSeconds (double s) noexcept
{
    if (! (s > 0.0))  return kTailFloorSeconds;   // also maps NaN -> floor
    return s < kTailFloorSeconds ? kTailFloorSeconds
         : (s > kTailCapSeconds   ? kTailCapSeconds   : s);
}

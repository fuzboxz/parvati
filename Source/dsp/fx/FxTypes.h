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

#include <array>
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
    // FV-1 family, second wave (2026-08-17): the bread-and-butter + quirky
    // set. APPEND-ONLY (same serialization rule as above).
    Overdrive = 16, LutDistortion = 17, Compressor = 18, Gate = 19,
    Chorus = 20, Flanger = 21, Echo = 22, Room = 23, Spring = 24,
    Count
};
// choice list string: { "None", "Diffuser", "Pitch Shifter", "CVerb",
//                       "Looping Delay", "Pitch Stretch", "Spectral",
//                       "Wavefolder", "Frequency Shifter", "Ring Modulator",
//                       "Resonator",
//                       "Clocked Delay", "Ensemble", "Plate",
//                       "Vinyl Compressor", "Phaser",
//                       "Overdrive", "LUT", "Compressor", "Gate",
//                       "Chorus", "Flanger", "Digital Echo", "Room", "Spring" }
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
// (what-effect-does-what discoverability). The APVTS choice list (host surface)
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
//     reverb; where someone hunting "that smeary space sound" looks),
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
        FxType::Chorus, FxType::Ensemble, FxType::Flanger, FxType::FrequencyShifter,
        FxType::Phaser, FxType::RingModulator,
        // Pitch/Time
        FxType::PitchShifter, FxType::Resonator, FxType::Spectral, FxType::WSOLAStretch,
        // Reverb (CVerb is the DISPLAY label of FxType::Reverb — see makeFxTypes)
        FxType::Reverb, FxType::Diffuser, FxType::PlateReverb, FxType::Room, FxType::Spring
    } };
}

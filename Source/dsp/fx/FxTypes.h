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
constexpr int kNumFxSlotParams  = 4;    // generic params 1..4 per slot

// Effect type per slot (drives fx{N}_type choice).
enum class FxType : uint8_t {
    None = 0, GainPan = 1, Delay = 2, Reverb = 3, Chorus = 4,
    Count
};
// choice list string: { "None", "Gain+Pan", "Delay", "Reverb", "Chorus" }

enum class FxTopology : uint8_t { Series = 0, Parallel = 1 };
// choice list string: { "Series", "Parallel" }

// FX mod-matrix destinations (drives fxmod{N}_dest choice). Distinct from
// MOD_DST_* (synth destinations). One dry/wet + four generic params per slot.
enum FxModDestination : int {
    FX_DST_NONE = -1,
    FX_DST_FX1_DRYWET = 0, FX_DST_FX1_P1, FX_DST_FX1_P2, FX_DST_FX1_P3, FX_DST_FX1_P4,
    FX_DST_FX2_DRYWET,     FX_DST_FX2_P1, FX_DST_FX2_P2, FX_DST_FX2_P3, FX_DST_FX2_P4,
    FX_DST_FX3_DRYWET,     FX_DST_FX3_P1, FX_DST_FX3_P2, FX_DST_FX3_P3, FX_DST_FX3_P4,
    FX_DST_LAST
};   // 15 destinations

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

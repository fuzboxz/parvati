// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// MulExport — the .MUL hardware-export fallback solver (pure, dependency-free).
//
// A Parvati multi can give a Part MORE voices than the voicecards it owns
// (the per-part `voice_slots` pool extension), but the Ambika hardware has
// exactly 6 voicecards: a .MUL can only express "this Part owns these cards"
// (a 6-bit bitmask) and its polyphony MODE. When a setup requests more voices
// than a Part has cards, exporting needs a STRATEGY for mapping the requested
// voices onto the 6 cards across Parts. This module owns that mapping as pure
// functions (no JUCE / engine state) so it is trivially unit-testable; the
// processor applies the result while writing the .MUL, and the editor shows a
// preview derived from it before saving.
//
// All strategies produce CONTIGUOUS bitmasks in Part order (part 0 takes the
// first cards, part 1 the next, ...) — the same shape PatchArrangement and the
// Patch page use — except AsIs, which writes the engine's stored bitmasks
// unchanged (the pre-extension behaviour).

#ifndef PARVATI_MUL_EXPORT_H_
#define PARVATI_MUL_EXPORT_H_

#include <array>
#include <cstdint>
#include <vector>

namespace parvati::mul_export
{
inline constexpr int kParts = 6;   // firmware Parts / voicecards

enum class Strategy
{
    AsIs = 0,        // write the engine's bitmasks unchanged; slots ignored (legacy)
    Proportional,    // largest-remainder split of the 6 cards by requested voices
    Priority,        // first-wins in Part order (firmware Multi::AssignVoices shape)
    EvenSplit,       // equal cards per active Part, capped by each Part's request
    MonoFold,        // Proportional + constrained Parts fold to MONO (unison character)
    ChainSplit       // several .MUL "units" chained (CHAIN heads forward overflow)
};

// What the engine wants to express, per Part.
struct Setup
{
    std::array<int, kParts> requested {};   // effective voices (slots, or card count)
    std::array<int, kParts> cards {};       // currently owned voicecards
    std::array<bool, kParts> active {};     // owns >= 1 card
    std::array<uint8_t, kParts> polyMode {};  // current PartData[15] (0..4)
};

// True when any active Part requests more voices than its cards — i.e. the
// .MUL cannot represent the setup faithfully and a strategy should be chosen.
bool needsFallback (const Setup& s);

// Derive the contiguous per-Part voicecard bitmasks from per-Part VOICE
// COUNTS: largest-remainder proportional share of the 6 cards weighted by
// each nonzero count, minimum one card per Part with a nonzero count, cards
// packed contiguously in Part order (part 0 first). This is the allocation
// rule the ENGINE uses to derive its card masks from voiceSlots (the slots
// model: 1 voice = digital voice + card) and the same shape
// Strategy::Proportional produces for export — one source of truth, kept
// here (pure + dependency-free) so the engine cannot drift from the solver.
std::array<uint8_t, kParts> deriveMasks (const std::array<int, kParts>& requested);

// One unit's solved allocation (a single .MUL file).
struct Solution
{
    std::array<uint8_t, kParts> masks {};            // contiguous voicecard bitmasks
    std::array<uint8_t, kParts> polyMode {};         // the PartData[15] to write
    std::array<bool, kParts> polyOverridden {};      // which modes the strategy rewrote
};

// Solve a single-file strategy (AsIs / Proportional / Priority / EvenSplit /
// MonoFold). AsIs echoes the Setup's own bitmasks (recomputed contiguously
// from its card counts — identical for contiguous setups) and never overrides
// a mode.
Solution solve (const Setup& s, Strategy strategy);

// Solve the chain strategy: packs each active Part's request into units of at
// most 6 cards. Every segment of a Part that continues on a later unit is
// written with polyphony CHAIN (the firmware forwards that Part's overflow
// notes over MIDI to the next unit, which receives them on the same channel +
// key zone); the final segment keeps the Part's original mode. Later Parts
// pack after a continuation on the same unit (zone collisions between such
// Parts are the user's responsibility — a power-user strategy). The first unit
// is the file the user chose; the rest are written as sibling files.
std::vector<Solution> solveChain (const Setup& s);

// Optional human context for the preview helpers (empty = "Part N" fallback).
struct PreviewContext
{
    std::vector<std::string> names;   // per-Part display names ("Kick", ...)
};

// Human-readable preview of one solution against the setup. One line per
// active Part, e.g. "Lead: 10 -> 3 voices (Poly)" / "Pad: 8 -> 2 voices
// (Mono, switched)". Dependency-free C++ (std::string) so tests stay light;
// the editor wraps it in juce::String.
std::vector<std::string> previewLines (const Setup& s, const Solution& sol,
                                       int unitIndex = 0,
                                       const PreviewContext* ctx = nullptr);

// One-line outcome summary for the whole strategy choice, e.g.
// "Fits on one Ambika. 6 of 24 voices kept." or
// "Needs 3 chained Ambikas. All 24 voices kept."
std::string summarize (const Setup& s, Strategy strategy);

}  // namespace parvati::mul_export

#endif  // PARVATI_MUL_EXPORT_H_

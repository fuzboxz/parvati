// Copyright (c) 2026 805Labs Kft. / Hellcat.
//
// TuningTables — the controller-firmware scale ("raga") presets.
//
// Vendored VERBATIM from the Mutable Instruments Ambika controller firmware
// (ambika_reference/controller/resources.cc:773-891, lookup_table_table[]
// dispatch :918-950). These are CONTROLLER-side resources (applied by firmware
// Part::TuneNote before the note reaches the voicecard), so they live here and
// NOT under the voicecard-scoped Source/dsp/resources/ (which stays untouched).
// Do not edit by hand; re-vendor from upstream if the firmware ever changes
// (same policy class as Source/dsp/resources/resources_data.cpp).
//
// Upstream: (c) 2012-2015 Emilie Gillet, mutable-instruments.net — GPL-3.0
// (see NOTICES.md). Mechanical transcription; no creative content added.
//
// Semantics (firmware faithful):
//   - Each table holds 12 per-note-class offsets in 1/128-semitone units,
//     indexed by (raw incoming note % 12) at trigger time (Part::TuneNote,
//     part.cc:634-647: raga shifts are applied to the tuned note).
//   - 32767 is the firmware "silence this note class" sentinel: firmware
//     Part::AcceptNote (part.cc:649-660) REFUSES notes of a muted class; it
//     is never voiced as a pitch (a deliberate Hellcat deviation from the
//     firmware's TuneNote arithmetic, which would add 32767 and clamp to
//     garbage — see SynthEngine::isNoteAcceptedByPartTuning).
//   - Preset ids 1..32 are the firmware lookup_table_table indices; id 16
//     (bageshree) aliases the kafi array and id 32 (rasia) aliases the yaman
//     array, exactly like the firmware dispatch. The hardware UI offers
//     0..31 (parameter.cc:680-689) but files can carry raga=32; Hellcat keeps
//     the file-faithful superset (harmless: the alias resolves).

#pragma once

#include <cstdint>

namespace hellcat
{
// Preset ids run 1..kNumTuningPresets; 0 means "off" (12-EDO).
inline constexpr int kNumTuningPresets = 32;

// Firmware sentinel: this note class is muted (AcceptNote rejects it).
inline constexpr int16_t kTuningSilence = 32767;

// Number of note classes / table entries (octave-repeating tables).
inline constexpr int kNumNoteClasses = 12;

// The id-th preset's 12 offsets (1/128-semitone units, indexed by note % 12).
// id 0 (off) returns nullptr. Out-of-range ids return nullptr.
const int16_t* tuningPresetTable (int id);

// Display name for preset id 1..32 ("Just", "Pythagorean", "Bhairav", ...);
// id 0 / out-of-range returns nullptr.
const char* tuningPresetName (int id);

// The neutral 12-EDO table (twelve zeros).
const int16_t* tuningEdoTable();
}  // namespace hellcat

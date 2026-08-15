// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// ScalaImport — converts a Scala .scl (+ optional .kbm) tuning into Parvati's
// per-part custom tuning table (12 int16 offsets in 1/128-semitone units,
// indexed by raw note class). Pure conversion: no engine, no state, no UI —
// deterministic and unit-testable in isolation.
//
// HARDWARE-LIMITATION CONTRACT (why this exists as a converter, not a full
// Scala implementation — see audit/2026-08-15-microtonal-design.md):
//   - The Ambika oscillator pitch path is 14-bit with an effective resolution
//     of 1/128 semitone (the increment LUT drops the LSB), so offsets are
//     quantized to ~0.78 cents. Offsets are clamped to ±127 (±~1 semitone),
//     the partTuning_ byte range; clamped classes raise a warning.
//   - Tables are octave-repeating by construction (indexed by note % 12), so
//     the formal octave MUST be 2/1 (period 1200 cents within the 1/128-st
//     quantum) — non-octave scales (e.g. Bohlen-Pierce) are REJECTED, not
//     approximated. Arpeggiator octave shifts therefore transpose by exactly
//     one scale period.
//   - A 12-key mapping is required: S != 12 is rejected (with or without a
//     kbm; an N-degree scale with N != 12 and no kbm maps S = N).
//   - Unmapped note classes (kbm 'x') become the firmware mute sentinel
//     32767 (the note class is refused, firmware AcceptNote semantics).
//
// GRAMMAR (normative Scala format, huygens-fokker.org/scala; line-oriented):
//   .scl: line 1 = description (anything, by position); the tone count N is
//   the first non-comment/non-blank line after it ('!' comment lines between
//   description and count are standard in the corpus); N is digits only; then N pitch lines — a token containing '.' is cents (may be
//   negative), otherwise 'a/b' is a ratio (ints, positive, 0 => error) and a
//   bare integer is the ratio n/1. '!' starts a comment anywhere (rest of the
//   line is dropped); text after the valid first token is ignored; blank
//   lines inside the pitch block are skipped; fewer than N tones => error;
//   lines after the Nth tone are ignored. Numbers are parsed in the C locale
//   only (a ',' or other stray character => error).
//   .kbm: no description line; 7 header values in order (S, first, last, M,
//   R, F, o) then exactly S key lines, each an integer scale degree (0-based;
//   degree 0 = 1/1) or 'x' (unmapped). '!'-comments and blank lines are
//   skipped like in .scl. Fewer than S key lines => REJECT (stricter than the
//   official template's "trailing unmapped may be omitted" — deterministic,
//   matches Surge). first/last are parsed but carry no meaning for a full
//   12-key mapping and are ignored. S == 0 ("linear map") => REJECT.
//
// DEFAULTS when kbmText is empty (Surge/Scala defaults — provably identical
// output to M=69/R=69/F=440 for an octave-repeating table): S = N, M = 60,
// R = 60, F = 261.6255653 Hz, o = N, keys = identity (0..S-1).
//
// MATH (single final rounding, full double precision throughout):
//   For the 12 consecutive keys n = M + x (x = 0..11):
//     octave  = floor((n - M) / 12)                       (always 0 here)
//     d(x)    = keys[x]
//     rel(x)  = octave * P + (d == 0 ? 0 : cents(tone[d]))   P = cents(tone[o])
//     dev(x)  = rel(x) - 100 * x                           (vs 12-EDO at that key)
//     r'      = the x in 0..11 whose (M + x) mod 12 == R mod 12
//     off(x)  = dev(x) - dev(r') + 1200 * log2(F / (440 * 2^((R - 69) / 12)))
//     units   = llround(off(x) * 1.28)                     (1/128-semitone units)
//   offsets[(M + x) % 12] = units. This makes key R land exactly on its
//   reference frequency F (which may itself deviate from 12-EDO — e.g. an
//   A = 432 Hz reference shows up as a constant offset across all classes).

#pragma once

#include <cstdint>

#include <juce_core/juce_core.h>

namespace parvati
{
struct ScalaImportResult
{
    bool ok = false;
    // 1/128-semitone offset per note class C..B (index = rawNote % 12),
    // clamped to ±127; muted classes carry kTuningSilence (32767).
    int16_t offsets[12] {};
    juce::StringArray warnings;
    juce::String error;   // empty iff ok
};

// Parse + convert. kbmText empty = Scala defaults (see header). A non-empty
// kbmText is always required to define a full 12-key mapping (S = 12).
ScalaImportResult importScala (const juce::String& sclText, const juce::String& kbmText = {});
}  // namespace parvati

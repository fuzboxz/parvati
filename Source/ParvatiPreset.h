// Copyright (c) 2024 805LABS / Parvati.
//
// ParvatiPreset — a Parvati-native, human-editable YAML preset format that
// COEXISTS with the Ambika .PRO/.MUL byte format. The Ambika format has no slot
// for Parvati-only sound settings: `vca_curve` (linear vs exponential VCA) and
// `filter_card` (filter topology) are flagged isOption and are silently dropped
// by saveProgramFile/saveMultiFile; arp settings are also dropped from a .PRO.
// A Parvati patch sculpted with the SVF + exponential VCA therefore reverts to
// defaults on a .PRO/.MUL reload. This format carries EVERYTHING.
//
// Format: a minimal OWNED YAML subset (no external dependency). We emit and parse
// only `key: value` scalars, `# comment` to end-of-line, one level of nested map
// under `params:` / `options:`, and a `parts:` list of maps. Values are int,
// float, or double-quoted strings. Indentation = 2 spaces.
//
// File extension `.parvati` for both patches and multis, discriminated by the
// top-level `format:` field (`parvati-patch` / `parvati-multi`).

#pragma once

#include <juce_core/juce_core.h>

class ParvatiAudioProcessor;

namespace parvati::preset
{
// ---- The format this code emits / understands ----
inline constexpr int      kFormatVersion  = 1;
inline constexpr const char* kParvatiVersion = "0.1.0";   // PRE-RELEASE (not 1.0)
inline constexpr const char* kFormatPatch   = "parvati-patch";
inline constexpr const char* kFormatMulti   = "parvati-multi";
inline constexpr const char* kFileExtension = ".parvati";

// ---- Minimal YAML-subset parse/emit (we control both ends) -----------------
// parseParvatiYaml -> a juce::var tree (DynamicObject for maps, Array<var> for
// lists, scalar vars otherwise). Returns a null var on a malformed document.
juce::var parseParvatiYaml (const juce::String& text);
// emitParvatiYaml -> a YAML-subset string from a juce::var tree.
juce::String emitParvatiYaml (const juce::var& tree);

// ---- Patch (single, current part) ------------------------------------------
// Serializes EVERY patch/part descriptor's current APVTS raw value — INCLUDING
// isArp / isOption (vca_curve, filter_card) / isSequencer — plus a header
// (format/version/parvati_version/name/author). `part_select` is excluded: it
// is the "which part am I editing" selector, not a sound setting. Choice params
// emit a ` # <choice label>` comment for readability.
juce::String serializeParvatiPatch (ParvatiAudioProcessor& proc);

// Parses @p yaml and applies every recognized paramID in `params:` to the APVTS
// (clamped to the descriptor range; unknown keys ignored for forward-compat),
// then syncAllParamsToEngine(). Returns true if the document parsed.
bool applyParvatiPatch (ParvatiAudioProcessor& proc, const juce::String& yaml);

// ---- Multi (all 6 parts) ---------------------------------------------------
// Serializes every part's patch/part/arp/seq values (gathered uniformly from
// engine storage — the current part's live edits are mirrored into patchBytes/
// partBytes/objects by the byte bridge) + per-part routing (channel / key zone /
// voice allocation) + global options (vca_curve, filter_card) under `options:`.
juce::String serializeParvatiMulti (ParvatiAudioProcessor& proc);

// Applies a parsed multi: writes every part's patch/part/arp/seq + routing to
// the engine, applies the global options, then shows Part 0 in the editor
// (mirroring loadMultiFile's end state). Returns true if the document parsed.
bool applyParvatiMulti (ParvatiAudioProcessor& proc, const juce::String& yaml);

// ---- Format sniffing (for the Load dialog / drag-drop routing) --------------
// Reads the top-level `format:` field of @p yaml. Returns kFormatPatch /
// kFormatMulti, or an empty string if not a Parvati preset document.
juce::String detectParvatiFormat (const juce::String& yaml);
}  // namespace parvati::preset

// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.
//
// HellcatPreset — a Hellcat-native, human-editable YAML preset format that
// COEXISTS with the Ambika .PRO/.MUL byte format. The Ambika format has no slot
// for Hellcat-only sound settings: `vca_curve` (linear vs exponential VCA) and
// `filter_card` (filter topology) are flagged isOption and are silently dropped
// by saveProgramFile/saveMultiFile; arp settings are also dropped from a .PRO.
// A Hellcat patch sculpted with the SVF + exponential VCA therefore reverts to
// defaults on a .PRO/.MUL reload. This format carries EVERYTHING.
//
// Format: a minimal OWNED YAML subset (no external dependency). We emit and parse
// only `key: value` scalars, `# comment` to end-of-line, one level of nested map
// under `params:` / `options:`, and a `parts:` list of maps. Values are int,
// float, or double-quoted strings. Indentation = 2 spaces.
//
// File extension `.yml` for both patches and multis, discriminated by the
// top-level `format:` field (`hellcat-patch` / `hellcat-multi`).

#pragma once

#include <juce_core/juce_core.h>

class HellcatAudioProcessor;

namespace hellcat::preset
{
// ---- The format this code emits / understands ----
inline constexpr int      kFormatVersion  = 1;
inline constexpr const char* kHellcatVersion = "0.1.0";   // PRE-RELEASE (not 1.0)
inline constexpr const char* kFormatPatch   = "hellcat-patch";
inline constexpr const char* kFormatMulti   = "hellcat-multi";
inline constexpr const char* kFileExtension = ".yml";

// ---- Minimal YAML-subset parse/emit (we control both ends) -----------------
// parseHellcatYaml -> a juce::var tree (DynamicObject for maps, Array<var> for
// lists, scalar vars otherwise). Returns a null var on a malformed document.
juce::var parseHellcatYaml (const juce::String& text);
// emitHellcatYaml -> a YAML-subset string from a juce::var tree.
juce::String emitHellcatYaml (const juce::var& tree);

// ---- Patch (single, current part) ------------------------------------------
// Serializes EVERY patch/part descriptor's current APVTS raw value — INCLUDING
// isArp / isOption (vca_curve, filter_card) / isSequencer — plus a header
// (format/version/hellcat_version/name/author). `part_select` is excluded: it
// is the "which part am I editing" selector, not a sound setting. Choice params
// emit a ` # <choice label>` comment for readability.
juce::String serializeHellcatPatch (HellcatAudioProcessor& proc);

// Parses @p yaml and applies every recognized paramID in `params:` to the APVTS
// (clamped to the descriptor range; unknown keys ignored for forward-compat),
// then syncAllParamsToEngine(). Returns true if the document parsed.
bool applyHellcatPatch (HellcatAudioProcessor& proc, const juce::String& yaml);

// ---- Multi (all 6 parts) ---------------------------------------------------
// Serializes every part's patch/part/arp/seq values (gathered uniformly from
// engine storage — the current part's live edits are mirrored into patchBytes/
// partBytes/objects by the byte bridge) + per-part routing (channel / key zone /
// voice allocation) + global options (vca_curve, filter_card) under `options:`.
juce::String serializeHellcatMulti (HellcatAudioProcessor& proc);

// Applies a parsed multi: writes every part's patch/part/arp/seq + routing to
// the engine, applies the global options, then shows Part 0 in the editor
// (mirroring loadMultiFile's end state). Returns true if the document parsed.
bool applyHellcatMulti (HellcatAudioProcessor& proc, const juce::String& yaml);

// ---- Format sniffing (for the Load dialog / drag-drop routing) --------------
// Reads the top-level `format:` field of @p yaml. Returns kFormatPatch /
// kFormatMulti, or an empty string if not a Hellcat preset document.
juce::String detectHellcatFormat (const juce::String& yaml);
}  // namespace hellcat::preset

// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// ParvatiTheme — a self-contained colour-palette struct plus the 5 built-in
// themes (Carbon, Midnight, Obsidian, Paper, Crimson). Carbon is byte-identical
// to the legacy `col::` palette in PluginEditor.cpp so the default look is
// preserved exactly. Phase 1a of docs/UI_MODERNIZATION_PLAN.md.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

//==============================================================================
// One complete colour palette for the Parvati UI. Every LookAndFeel / paint
// call reads its colours from a ParvatiTheme, so switching themes is a single
// pointer change. The field names map onto the old `col::` constants plus the
// extra colours the modernised UI needs (panel headers, dividers, knob arcs…).
struct ParvatiTheme
{
    juce::String name;             // "Carbon", "Midnight", ...
    juce::Colour windowBackground; // page fill (was col::bg)
    juce::Colour panelBackground;  // was col::panel
    juce::Colour panelBackground2; // was col::panel2
    juce::Colour panelHeader;      // bordered-group heading strip
    juce::Colour outline;          // control/panel border (was col::outline)
    juce::Colour divider;          // thin separator inside panels
    juce::Colour accent;           // primary highlight (was col::accent gold)
    juce::Colour accent2;          // secondary / bipolar-mod (was col::accent2 steel)
    juce::Colour text;             // was col::text
    juce::Colour textDim;          // was col::textDim
    juce::Colour textValue;        // numeric value readout (brightest text)
    juce::Colour knobArc;          // rotary fill arc
    juce::Colour knobTrack;        // rotary background track
    juce::Colour knobMod;          // bipolar / modulation overlay
    // --- UI-refinement tokens (container depth + tab chrome) ---
    // Subtle interior fill that "lifts" functional containers off the pure
    // background; the LookAndFeel draws it inside every GroupComponent card.
    juce::Colour containerFill;    // subtle interior fill of functional panels (lighter than bg)
    // Base colour for a faint diffuse depth ring drawn inside each panel. The
    // LookAndFeel applies alpha (stored opaque so the struct stays a palette).
    juce::Colour containerShadow;  // faint diffuse depth ring inside panels (darker than bg; alpha applied in L&F)
    // Inset depth line for clickable containers (ComboBox / TextButton) so they
    // read as recessed rather than flat; alpha applied in the LookAndFeel.
    juce::Colour innerShadow;      // inset depth line for combos/buttons (alpha applied in L&F)
    // Tab-button chrome. Selected tab sits on a lighter fill with a prominent
    // full-width underline; unselected on a darker charcoal fill.
    juce::Colour tabUnselectedBg;  // tab button background — inactive
    juce::Colour tabSelectedBg;    // tab button background — active
    juce::Colour tabUnderline;     // prominent active-tab underline (== accent)
    // --- category colour tokens (control arcs/graphs coloured by function group) ---
    // Later workers READ these to tint per-category control arcs / graphs. They
    // are the ONLY new colours; do not hardcode category hues elsewhere.
    juce::Colour catAudio;         // Audio: Oscillators, Sub-Osc, Noise, Filter, Mixer (amber)
    juce::Colour catEnv;           // Envelopes (cyan)
    juce::Colour catLfo;           // LFOs (magenta)
    juce::Colour catSeq;           // Sequencer (neon green)
    juce::Colour catArp;           // Arpeggiator (electric purple)
    // --- modulation-source catalogue tokens (CentralModBar clusters) ---
    // Read by ModSourceCatalog::clusterAccent to tint each cluster's micro-pills
    // and drag image. The four hues are DISTINCT from the five control-arc tokens
    // above so a generator pill never reads the same colour as a control section.
    juce::Colour catPerf;          // Performance sources: Velocity/Aftertouch/Bend/Wheels/Expression/Note (yellow)
    juce::Colour catUtil;          // Utility sources: Gate/Noise/Random (orange)
    juce::Colour catMod;           // Modifier / operator outputs (magenta, distinct from catArp)
    juce::Colour catConst;         // Constant sources 256..4 (slate/teal)
    // --- keyboard natural-key token ---
    // Resting fill of the on-screen piano's natural (white) keys — a warm near-
    // white on the dark themes, a light off-white on Paper. KeyboardView derives
    // every key depth / highlight / shadow colour from this. The ONLY new colour;
    // do not hardcode a key-white outside the theme factories.
    juce::Colour keyWhite;         // natural (white) key resting fill (piano white)
    bool isDark = true;
};

//==============================================================================
// Stable builtin list order: Carbon(0), Midnight(1), Obsidian(2), Paper(3),
// Crimson(4). Each factory returns a reference to a function-local static, so
// the theme objects live for the whole program and are safe to hold pointers to.

// All built-in themes, in the order above. Stable for the program's lifetime.
const std::vector<ParvatiTheme>& getBuiltinThemes();

// Number of built-in themes (== 5).
int kNumBuiltinThemes();

const ParvatiTheme& carbonTheme();     // default = current look (dark / gold)
const ParvatiTheme& midnightTheme();   // dark blue / teal
const ParvatiTheme& obsidianTheme();   // near-black / violet
const ParvatiTheme& paperTheme();      // light
const ParvatiTheme& crimsonTheme();    // dark red

// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// ParvatiTheme — a self-contained colour-palette struct plus the 6 built-in
// themes (Carbon, Midnight, Obsidian, Paper, Crimson, Legacy). The palette is a clean
// 3-layer semantic scheme so every LookAndFeel / paint call reads its colours
// from a single ParvatiTheme (switching themes is one pointer change):
//
//   Layer 1 — BASE      : the surfaces the UI is painted on.
//   Layer 2 — INFORMATION: text / icons that read against those surfaces.
//   Layer 3 — ACTION     : the brand + routing accents.
//
// A handful of auxiliary tokens (borders, separators, keyboard white, tab
// chrome) and the modulation-routing palette round the struct out. Selecting
// Carbon reproduces the default dark look (cyan brand accent, was gold).

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

//==============================================================================
// One complete colour palette for the Parvati UI, organised in three semantic
// layers plus the auxiliary + modulation-routing tokens. The 6 factories in
// ParvatiTheme.cpp initialise this struct POSITIONALLY (brace init in field
// order), so a missed / extra / mis-ordered value silently misaligns every
// later field with NO compile error — keep every factory in lockstep with the
// field order here, and keep the editor_test alignment guard green.
struct ParvatiTheme
{
    juce::String name;

    // ---- Layer 1: BASE (surfaces) -----------------------------------------
    juce::Colour backgroundBase;      // chassis / window fill (was windowBackground)
    juce::Colour backgroundPanel;     // section cards (was panelBackground; panelBackground2 folded in)
    juce::Colour backgroundInput;     // dropdowns / textboxes / recessed input fills (NEW, distinct from panel)
    juce::Colour backgroundInputHover;// hover state of an input (NEW, lighter than backgroundInput)

    // ---- Layer 2: INFORMATION (text / icons) ------------------------------
    juce::Colour textPrimary;         // active values, selected-tab text, active dropdown (was textValue/text)
    juce::Colour textSecondary;       // parameter labels, section headers, dim text (was labelText/panelHeader/textDim)
    juce::Colour textDisabled;        // bypassed / inactive / empty slots (NEW)
    juce::Colour trackEmpty;          // knob empty arc / slider zero path (was knobTrack) — visible but recedes

    // ---- Layer 3: ACTION (accents / routing) ------------------------------
    juce::Colour accentPrimary;       // brand: filled knob arc, active-slider fill, selected-tab BG (was accent/knobArc)
    juce::Colour accentSecondary;     // complementary: bypass / visualizer / bipolar mod (was accent2)

    // ---- Auxiliary surface tokens -----------------------------------------
    juce::Colour outline;             // control / panel border
    juce::Colour divider;             // thin separator inside panels
    juce::Colour containerFill;       // subtle interior fill of functional panels
    juce::Colour tabUnselectedBg;     // inactive tab-button background
    juce::Colour tabSelectedBg;       // active tab-button background
    juce::Colour keyWhite;            // natural (white) key resting fill (piano white)

    // ---- Modulation routing palette ---------------------------------------
    // The per-category ROUTING colours. The synth needs every category colour,
    // not just a handful — keep all nine stable (relabelling the group only).
    // The STRICT family palette: teal=Envelope, magenta=LFO, amber=keyboard/perf,
    // mint=sequencer (Seq+Arp), orange=utility, purple=modifier, indigo=constant.
    // Control arcs/graphs + mod-bar pills + matrix rows tinted by function group:
    juce::Colour catAudio;            // Audio: Oscillators, Sub-Osc, Noise, Filter, Mixer (amber — NOT a mod-source family; section headers)
    juce::Colour catEnv;              // Envelopes (TEAL)
    juce::Colour catLfo;              // LFOs (MAGENTA)
    juce::Colour catSeq;              // Sequencer (MINT GREEN)
    juce::Colour catArp;              // Arpeggiator (MINT GREEN — grouped with Seq, the sequencer family)
    // Modulation-source catalogue clusters (CentralModBar):
    juce::Colour catPerf;             // Performance sources: Velocity/Aftertouch/Bend/Wheels/Expression/Note (AMBER)
    juce::Colour catUtil;             // Utility sources: Gate/Noise/Random (ORANGE)
    juce::Colour catMod;              // Modifier / operator outputs M1-4 (PURPLE)
    juce::Colour catConst;            // Constant sources C4..C255 (SLATE-BLUE / INDIGO)

    bool isDark = true;
};

//==============================================================================
namespace parvati
{
// NEUTRAL fallback accent for display components that paint before a theme is
// reachable (nullptr theme): a cool steel blue matching Carbon's
// accentSecondary. Deliberately NOT a warm hue — the old pre-theme gold
// (0xffe8b84b-class literals) is gone so no indicator can render amber by
// DEFAULT; amber now appears only where a THEME deliberately defines it.
inline const juce::Colour parvatiFallbackAccent { 0xff5b8db8 };
}   // namespace parvati

//==============================================================================
// Stable builtin list order: Carbon(0), Midnight(1), Obsidian(2), Paper(3),
// Crimson(4), Legacy(5). Each factory returns a reference to a function-local static, so
// the theme objects live for the whole program and are safe to hold pointers to.

// All built-in themes, in the order above. Stable for the program's lifetime.
const std::vector<ParvatiTheme>& getBuiltinThemes();

// Number of built-in themes (== 6).
int kNumBuiltinThemes();

const ParvatiTheme& carbonTheme();     // default = current look (dark / cyan)
const ParvatiTheme& midnightTheme();   // dark blue / teal
const ParvatiTheme& obsidianTheme();   // near-black / violet
const ParvatiTheme& paperTheme();      // light
const ParvatiTheme& crimsonTheme();    // dark red
const ParvatiTheme& legacyTheme();     // light gray / magenta (reference adoption)

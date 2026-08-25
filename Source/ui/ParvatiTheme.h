// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// ParvatiTheme — a self-contained colour-palette struct plus the 5 built-in
// themes (Carbon, Midnight, Immutable, Swedish Red, Y2K). The palette is a clean
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
    juce::Colour textPrimary;         // active values, selected-tab text, active dropdown, MODULE/SECTION HEADERS (group titles — raised to this tier 2026-08-20; was textSecondary) (was textValue/text)
    juce::Colour textSecondary;       // parameter labels, dim text (NOT section headers — they use textPrimary) (was labelText/panelHeader/textDim)
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
    juce::Colour keyWhite;            // natural-key resting fill — theme-matched ELEVATED surface (NOT piano ivory: dark themes use a light slate step above the panels, light themes a neutral near-white)
    juce::Colour keyBlack;            // sharp-key resting fill — near-background recessed tone on dark themes, dark key-charcoal on light themes (NEW with the themed-keyboard wave)

    // ---- Modulation routing palette ---------------------------------------
    // The per-category ROUTING colours. The synth needs every category colour,
    // not just a handful — keep all nine stable (relabelling the group only).
    // The STRICT family palette: teal=Envelope, magenta=LFO, amber=keyboard/perf,
    // mint=sequencer (Seq+Arp), orange=utility, purple=modifier, indigo=constant.
    // Control arcs/graphs + mod-bar pills + matrix rows tinted by function group:
    juce::Colour catAudio;            // Audio: Oscillators, Sub-Osc, Noise, Filter, Mixer (NOT a mod-source family; section headers). Adopts the theme's BRAND ACCENT in every theme — never the family amber, so knob rings/previews follow the accent (Carbon/Immutable/Y2K: primary; Midnight: secondary — its primary teal collides with the Env teal family; Swedish Red: the theme's deliberately-defined display hue — see the factory). The mod-source family hues below are unaffected and stay uniform across themes.
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

// No-LookAndFeel fallback colours for components that paint in a plain host
// or test harness (no ParvatiLookAndFeel installed). These are STATIC safety
// defaults, NOT theme tokens: they keep a component readable until the app
// LookAndFeel attaches. Each value matches the literal the paint code
// historically carried, so pixels stay identical. One named constant per
// role; do not add new spellings of the same role.
inline const juce::Colour kFallbackTextPrimary   { 0xffe8e8ee };  // textPrimary role
inline const juce::Colour kFallbackTextSoft      { 0xffe0e0e0 };  // wheels chip text (its own historical value)
inline const juce::Colour kFallbackTextSecondary { 0xff9a9aa8 };  // textSecondary role
inline const juce::Colour kFallbackTextDisabled  { 0xff6b7280 };  // textDisabled role
inline const juce::Colour kFallbackPanel         { 0xff24242e };  // backgroundPanel role
inline const juce::Colour kFallbackOutline       { 0xff3c3c4a };  // outline role
inline const juce::Colour kFallbackContainerFill { 0xff202028 };  // containerFill role
inline const juce::Colour kFallbackBase          { 0xff141419 };  // backgroundBase role (wheels bay)
inline const juce::Colour kFallbackOutlineSoft   { 0xff3a3a44 };  // wheels track outline
}   // namespace parvati

//==============================================================================
// Stable builtin list order: Carbon(0), Midnight(1), Immutable(2),
// Swedish Red(3), Y2K(4). Each factory returns a reference to a
// function-local static, so
// the theme objects live for the whole program and are safe to hold pointers to.

// All built-in themes, in the order above. Stable for the program's lifetime.
const std::vector<ParvatiTheme>& getBuiltinThemes();

// Number of built-in themes (== 5).
int kNumBuiltinThemes();

const ParvatiTheme& carbonTheme();     // default = current look (dark / cyan)
const ParvatiTheme& midnightTheme();   // dark blue / teal
const ParvatiTheme& immutableTheme();  // light gray / magenta (reference adoption)
const ParvatiTheme& swedishRedTheme(); // red chassis / grey-black cards / green LCD displays
const ParvatiTheme& y2kTheme();         // glossy desktop azure / liquid chrome / candy routing colours

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

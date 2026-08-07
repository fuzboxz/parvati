// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// ParvatiTheme — the 5 built-in themes. The ARGB values below are the exact
// RGB table from the Phase 1a implementation plan. Carbon is the verbatim
// legacy `col::` palette (PluginEditor.cpp), so selecting Carbon reproduces
// the original dark/gold look byte-for-byte.

#include "ParvatiTheme.h"

namespace
{
    // Helper: the colours are declared in struct field order so every factory
    // reads identically and a diff against the table is trivial. POSITIONAL
    // brace init is used, so a missed / extra / mis-ordered value silently
    // misaligns ALL later fields with NO compile error. Keep this list and every
    // factory in lockstep, and keep the editor_test guard green:
    //   name (juce::String),
    //   windowBackground, panelBackground, panelBackground2, panelHeader,
    //   outline, divider,
    //   accent, accent2,
    //   text, textDim, textValue,
    //   knobArc, knobTrack, knobMod,
    //   containerFill, containerShadow, innerShadow,
    //   tabUnselectedBg, tabSelectedBg, tabUnderline,
    //   catAudio, catEnv, catLfo, catSeq, catArp,
    //   catPerf, catUtil, catMod, catConst,
    //   keyWhite, labelText,
    //   isDark (bool)  <-- trailing bool is a positional-init sentinel
} // namespace

//==============================================================================
const ParvatiTheme& carbonTheme()
{
    // Default look, modernised flat palette: a muted near-black window with
    // slightly LIGHTER panel cards for tonal separation (no skeuomorphic
    // shadows/outlines). Gold accent retained as the brand colour.
    static const ParvatiTheme t {
        "Carbon",
        juce::Colour (0xff15171C), juce::Colour (0xff1E2228), juce::Colour (0xff252A31),
        juce::Colour (0xff9AA0A8), juce::Colour (0xff2C3138), juce::Colour (0xff2A2E35),
        juce::Colour (0xffE5A93C), juce::Colour (0xff5b8db8),   // gold / steel
        juce::Colour (0xffe8e8ee), juce::Colour (0xff9a9aa8), juce::Colour (0xfff6f6fa),
        juce::Colour (0xffE5A93C), juce::Colour (0xff2A2E35), juce::Colour (0xff5b8db8),
        // container fill/shadow (shadow kept for palette completeness, NOT drawn),
        // inner shadow, tab unselected/selected bg, tab underline
        juce::Colour (0xff1E2228), juce::Colour (0xff15171C), juce::Colour (0xff15171C),
        juce::Colour (0xff1A1E24), juce::Colour (0xff252A31), juce::Colour (0xffE5A93C),
        // category colours: catAudio, catEnv, catLfo, catSeq, catArp
        juce::Colour (0xffFFB400), juce::Colour (0xff00E5FF), juce::Colour (0xffFF0055),
        juce::Colour (0xff00FF66), juce::Colour (0xffAA00FF),
        // mod-catalogue cluster colours: catPerf, catUtil, catMod, catConst
        juce::Colour (0xffFFE600), juce::Colour (0xffFF7800), juce::Colour (0xffFF52D9),
        juce::Colour (0xff3DD2B8),
        // keyWhite: natural (white) key resting fill (warm ivory)
        juce::Colour (0xffeeeae0),
        // labelText: low-contrast parameter-label gray
        juce::Colour (0xff6B7280),
        true
    };
    return t;
}

const ParvatiTheme& midnightTheme()
{
    // Dark blue / teal — modernised flat palette (lighter panel cards, no
    // skeuomorphic shadows/outlines).
    static const ParvatiTheme t {
        "Midnight",
        juce::Colour (0xff131922), juce::Colour (0xff1C2433), juce::Colour (0xff242E40),
        juce::Colour (0xff9AA6B4), juce::Colour (0xff2E3950), juce::Colour (0xff26303F),
        juce::Colour (0xff2bb6c4), juce::Colour (0xff5b9bd5),   // teal / blue
        juce::Colour (0xffdde7f0), juce::Colour (0xff8a9bb0), juce::Colour (0xffeaf4fb),
        juce::Colour (0xff2bb6c4), juce::Colour (0xff2A3242), juce::Colour (0xff5b9bd5),
        // container fill/shadow (shadow NOT drawn), inner shadow, tab bg, tab underline
        juce::Colour (0xff1C2433), juce::Colour (0xff131922), juce::Colour (0xff131922),
        juce::Colour (0xff182030), juce::Colour (0xff242E40), juce::Colour (0xff2bb6c4),
        // category colours: catAudio, catEnv, catLfo, catSeq, catArp
        juce::Colour (0xffFFB400), juce::Colour (0xff00E5FF), juce::Colour (0xffFF0055),
        juce::Colour (0xff00FF66), juce::Colour (0xffAA00FF),
        // mod-catalogue cluster colours: catPerf, catUtil, catMod, catConst
        juce::Colour (0xffFFE600), juce::Colour (0xffFF7800), juce::Colour (0xffFF52D9),
        juce::Colour (0xff3DD2B8),
        // keyWhite: natural (white) key resting fill (cool ivory, suits the blue palette)
        juce::Colour (0xffeceef1),
        // labelText: low-contrast parameter-label gray (blue-tinted)
        juce::Colour (0xff6E7A8C),
        true
    };
    return t;
}

const ParvatiTheme& obsidianTheme()
{
    // Near-black / violet — modernised flat palette (lighter panel cards, no
    // skeuomorphic shadows/outlines).
    static const ParvatiTheme t {
        "Obsidian",
        juce::Colour (0xff131318), juce::Colour (0xff1C1C26), juce::Colour (0xff252532),
        juce::Colour (0xff9A92B0), juce::Colour (0xff2C2C3C), juce::Colour (0xff262634),
        juce::Colour (0xff8b5cf6), juce::Colour (0xffd946ef),   // violet / fuchsia
        juce::Colour (0xffe4e0f0), juce::Colour (0xff9a92b0), juce::Colour (0xfff0ecfa),
        juce::Colour (0xff8b5cf6), juce::Colour (0xff2A2A38), juce::Colour (0xffd946ef),
        // container fill/shadow (shadow NOT drawn), inner shadow, tab bg, tab underline
        juce::Colour (0xff1C1C26), juce::Colour (0xff131318), juce::Colour (0xff131318),
        juce::Colour (0xff181820), juce::Colour (0xff252532), juce::Colour (0xff8b5cf6),
        // category colours: catAudio, catEnv, catLfo, catSeq, catArp
        juce::Colour (0xffFFB400), juce::Colour (0xff00E5FF), juce::Colour (0xffFF0055),
        juce::Colour (0xff00FF66), juce::Colour (0xffAA00FF),
        // mod-catalogue cluster colours: catPerf, catUtil, catMod, catConst
        juce::Colour (0xffFFE600), juce::Colour (0xffFF7800), juce::Colour (0xffFF52D9),
        juce::Colour (0xff3DD2B8),
        // keyWhite: natural (white) key resting fill (neutral ivory w/ faint violet)
        juce::Colour (0xffefeaf2),
        // labelText: low-contrast parameter-label gray (violet-tinted)
        juce::Colour (0xff6C6878),
        true
    };
    return t;
}

const ParvatiTheme& paperTheme()
{
    // Light theme. isDark = false so L&F can adapt contrast / focus rings.
    // Modernised flat palette: panel cards slightly DARKER than the page (tonal
    // contrast inverts on light themes); no skeuomorphic shadows/outlines.
    static const ParvatiTheme t {
        "Paper",
        juce::Colour (0xfff7f6f2), juce::Colour (0xffeceae4), juce::Colour (0xffe2dfd7),
        juce::Colour (0xff565B63), juce::Colour (0xffc2beb3), juce::Colour (0xffdedbd1),
        juce::Colour (0xffb45309), juce::Colour (0xff2563eb),   // deep amber / blue
        juce::Colour (0xff2b2a26), juce::Colour (0xff6b6862), juce::Colour (0xff11100e),
        juce::Colour (0xffb45309), juce::Colour (0xffD0CCC2), juce::Colour (0xff2563eb),
        // container fill/shadow (grey, not near-black; shadow NOT drawn), inner
        // shadow, tab unselected/selected bg, tab underline
        juce::Colour (0xffeceae4), juce::Colour (0xfff7f6f2), juce::Colour (0xfff7f6f2),
        juce::Colour (0xffe0ddd3), juce::Colour (0xffece9e0), juce::Colour (0xffb45309),
        // category colours: catAudio, catEnv, catLfo, catSeq, catArp
        // (darker / saturated variants for clear contrast on the light bg)
        juce::Colour (0xffB45309), juce::Colour (0xff0E7490), juce::Colour (0xffBE123C),
        juce::Colour (0xff15803D), juce::Colour (0xff6B21A8),
        // mod-catalogue cluster colours: catPerf, catUtil, catMod, catConst
        // (dark / saturated variants for clear contrast on the light bg)
        juce::Colour (0xffA16207), juce::Colour (0xffC2410C), juce::Colour (0xffA21CAF),
        juce::Colour (0xff475569),
        // keyWhite: light off-white that still reads against the light keyboard strip
        juce::Colour (0xfffbf9f4),
        // labelText: parameter-label gray (medium, readable on light bg)
        juce::Colour (0xff6b6862),
        false
    };
    return t;
}

const ParvatiTheme& crimsonTheme()
{
    // Dark red — modernised flat palette (lighter panel cards, no skeuomorphic
    // shadows/outlines).
    static const ParvatiTheme t {
        "Crimson",
        juce::Colour (0xff1A0E0E), juce::Colour (0xff2A1818), juce::Colour (0xff341E1E),
        juce::Colour (0xffB09A9A), juce::Colour (0xff3E2424), juce::Colour (0xff2E1818),
        juce::Colour (0xffe5484d), juce::Colour (0xff3b82f6),   // crimson / blue
        juce::Colour (0xfff2e6e6), juce::Colour (0xffb08a8a), juce::Colour (0xfff9eded),
        juce::Colour (0xffe5484d), juce::Colour (0xff3A2020), juce::Colour (0xff3b82f6),
        // container fill/shadow (shadow NOT drawn), inner shadow, tab bg, tab underline
        juce::Colour (0xff2A1818), juce::Colour (0xff1A0E0E), juce::Colour (0xff1A0E0E),
        juce::Colour (0xff201212), juce::Colour (0xff341E1E), juce::Colour (0xffe5484d),
        // category colours: catAudio, catEnv, catLfo, catSeq, catArp
        // (harmonized with the crimson palette: vivid but slightly desaturated / shifted)
        juce::Colour (0xffE8923C), juce::Colour (0xff3DD6D0), juce::Colour (0xffE0508A),
        juce::Colour (0xff4FD17A), juce::Colour (0xff9D5BD9),
        // mod-catalogue cluster colours: catPerf, catUtil, catMod, catConst
        // (harmonized with the crimson palette; catConst is slate since catEnv is teal)
        juce::Colour (0xffE8C04A), juce::Colour (0xffCC3A1F), juce::Colour (0xffB85AB5),
        juce::Colour (0xff6B8FA8),
        // keyWhite: natural (white) key resting fill (warm ivory w/ faint warm undertone)
        juce::Colour (0xfff1ebe7),
        // labelText: low-contrast parameter-label gray (warm-tinted)
        juce::Colour (0xff8A6E6E),
        true
    };
    return t;
}

//==============================================================================
const std::vector<ParvatiTheme>& getBuiltinThemes()
{
    static const std::vector<ParvatiTheme> v {
        carbonTheme(),
        midnightTheme(),
        obsidianTheme(),
        paperTheme(),
        crimsonTheme()
    };
    return v;
}

int kNumBuiltinThemes()
{
    return static_cast<int> (getBuiltinThemes().size());
}

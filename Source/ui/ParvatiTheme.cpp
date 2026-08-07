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
    //   keyWhite,
    //   isDark (bool)
} // namespace

//==============================================================================
const ParvatiTheme& carbonTheme()
{
    // Default = current look. Byte-identical to PluginEditor.cpp `col::`.
    static const ParvatiTheme t {
        "Carbon",
        juce::Colour (0xff15161A), juce::Colour (0xff24242e), juce::Colour (0xff2e2e3a),
        juce::Colour (0xff343440), juce::Colour (0xff333742), juce::Colour (0xff2a2a34),
        juce::Colour (0xffE5A93C), juce::Colour (0xff5b8db8),   // gold / steel
        juce::Colour (0xffe8e8ee), juce::Colour (0xff9a9aa8), juce::Colour (0xfff6f6fa),
        juce::Colour (0xffE5A93C), juce::Colour (0xff333742), juce::Colour (0xff5b8db8),
        // container fill/shadow, inner shadow, tab unselected/selected bg, tab underline
        juce::Colour (0xff1e1e26), juce::Colour (0xff050507), juce::Colour (0xff060609),
        juce::Colour (0xff16161c), juce::Colour (0xff262630), juce::Colour (0xffE5A93C),
        // category colours: catAudio, catEnv, catLfo, catSeq, catArp
        juce::Colour (0xffFFB400), juce::Colour (0xff00E5FF), juce::Colour (0xffFF0055),
        juce::Colour (0xff00FF66), juce::Colour (0xffAA00FF),
        // mod-catalogue cluster colours: catPerf, catUtil, catMod, catConst
        juce::Colour (0xffFFE600), juce::Colour (0xffFF7800), juce::Colour (0xffFF52D9),
        juce::Colour (0xff3DD2B8),
        // keyWhite: natural (white) key resting fill (warm ivory)
        juce::Colour (0xffeeeae0),
        true
    };
    return t;
}

const ParvatiTheme& midnightTheme()
{
    // Dark blue / teal.
    static const ParvatiTheme t {
        "Midnight",
        juce::Colour (0xff0d1320), juce::Colour (0xff16202f), juce::Colour (0xff1d2a3d),
        juce::Colour (0xff233349), juce::Colour (0xff2b3a52), juce::Colour (0xff1f2c40),
        juce::Colour (0xff2bb6c4), juce::Colour (0xff5b9bd5),   // teal / blue
        juce::Colour (0xffdde7f0), juce::Colour (0xff8a9bb0), juce::Colour (0xffeaf4fb),
        juce::Colour (0xff2bb6c4), juce::Colour (0xff2b3a52), juce::Colour (0xff5b9bd5),
        // container fill/shadow, inner shadow, tab unselected/selected bg, tab underline
        juce::Colour (0xff121b2c), juce::Colour (0xff04070c), juce::Colour (0xff05080e),
        juce::Colour (0xff101827), juce::Colour (0xff1a2638), juce::Colour (0xff2bb6c4),
        // category colours: catAudio, catEnv, catLfo, catSeq, catArp
        juce::Colour (0xffFFB400), juce::Colour (0xff00E5FF), juce::Colour (0xffFF0055),
        juce::Colour (0xff00FF66), juce::Colour (0xffAA00FF),
        // mod-catalogue cluster colours: catPerf, catUtil, catMod, catConst
        juce::Colour (0xffFFE600), juce::Colour (0xffFF7800), juce::Colour (0xffFF52D9),
        juce::Colour (0xff3DD2B8),
        // keyWhite: natural (white) key resting fill (cool ivory, suits the blue palette)
        juce::Colour (0xffeceef1),
        true
    };
    return t;
}

const ParvatiTheme& obsidianTheme()
{
    // Near-black / violet.
    static const ParvatiTheme t {
        "Obsidian",
        juce::Colour (0xff0a0a0f), juce::Colour (0xff12121c), juce::Colour (0xff1a1a28),
        juce::Colour (0xff21212f), juce::Colour (0xff2a2a3c), juce::Colour (0xff191926),
        juce::Colour (0xff8b5cf6), juce::Colour (0xffd946ef),   // violet / fuchsia
        juce::Colour (0xffe4e0f0), juce::Colour (0xff9a92b0), juce::Colour (0xfff0ecfa),
        juce::Colour (0xff8b5cf6), juce::Colour (0xff2a2a3c), juce::Colour (0xffd946ef),
        // container fill/shadow, inner shadow, tab unselected/selected bg, tab underline
        juce::Colour (0xff131320), juce::Colour (0xff030305), juce::Colour (0xff040406),
        juce::Colour (0xff10101a), juce::Colour (0xff1c1c2c), juce::Colour (0xff8b5cf6),
        // category colours: catAudio, catEnv, catLfo, catSeq, catArp
        juce::Colour (0xffFFB400), juce::Colour (0xff00E5FF), juce::Colour (0xffFF0055),
        juce::Colour (0xff00FF66), juce::Colour (0xffAA00FF),
        // mod-catalogue cluster colours: catPerf, catUtil, catMod, catConst
        juce::Colour (0xffFFE600), juce::Colour (0xffFF7800), juce::Colour (0xffFF52D9),
        juce::Colour (0xff3DD2B8),
        // keyWhite: natural (white) key resting fill (neutral ivory w/ faint violet)
        juce::Colour (0xffefeaf2),
        true
    };
    return t;
}

const ParvatiTheme& paperTheme()
{
    // Light theme. isDark = false so L&F can adapt contrast / focus rings.
    static const ParvatiTheme t {
        "Paper",
        juce::Colour (0xfff7f6f2), juce::Colour (0xffeceae4), juce::Colour (0xffe2dfd7),
        juce::Colour (0xffd8d5cc), juce::Colour (0xffc2beb3), juce::Colour (0xffdedbd1),
        juce::Colour (0xffb45309), juce::Colour (0xff2563eb),   // deep amber / blue
        juce::Colour (0xff2b2a26), juce::Colour (0xff6b6862), juce::Colour (0xff11100e),
        juce::Colour (0xffb45309), juce::Colour (0xffc2beb3), juce::Colour (0xff2563eb),
        // container fill/shadow (grey, not near-black), inner shadow, tab unselected/selected bg, tab underline
        juce::Colour (0xfff2f0ea), juce::Colour (0xffc4c0b4), juce::Colour (0xffb0aca0),
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
        false
    };
    return t;
}

const ParvatiTheme& crimsonTheme()
{
    // Dark red.
    static const ParvatiTheme t {
        "Crimson",
        juce::Colour (0xff1a0d0d), juce::Colour (0xff241414), juce::Colour (0xff2e1818),
        juce::Colour (0xff381c1c), juce::Colour (0xff4a2424), juce::Colour (0xff2a1616),
        juce::Colour (0xffe5484d), juce::Colour (0xff3b82f6),   // crimson / blue
        juce::Colour (0xfff2e6e6), juce::Colour (0xffb08a8a), juce::Colour (0xfff9eded),
        juce::Colour (0xffe5484d), juce::Colour (0xff4a2424), juce::Colour (0xff3b82f6),
        // container fill/shadow, inner shadow, tab unselected/selected bg, tab underline
        juce::Colour (0xff261616), juce::Colour (0xff080303), juce::Colour (0xff0a0404),
        juce::Colour (0xff1f1212), juce::Colour (0xff301a1a), juce::Colour (0xffe5484d),
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

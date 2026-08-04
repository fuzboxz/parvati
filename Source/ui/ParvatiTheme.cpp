// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// ParvatiTheme — the 5 built-in themes. The ARGB values below are the exact
// RGB table from the Phase 1a implementation plan. Carbon is the verbatim
// legacy `col::` palette (PluginEditor.cpp), so selecting Carbon reproduces
// the original dark/gold look byte-for-byte.

#include "ParvatiTheme.h"

namespace
{
    // Helper: the 14 colours are declared in struct field order so every
    // factory reads identically and a diff against the table is trivial.
    //   windowBackground, panelBackground, panelBackground2, panelHeader,
    //   outline, divider,
    //   accent, accent2,
    //   text, textDim, textValue,
    //   knobArc, knobTrack, knobMod,
    //   isDark
} // namespace

//==============================================================================
const ParvatiTheme& carbonTheme()
{
    // Default = current look. Byte-identical to PluginEditor.cpp `col::`.
    static const ParvatiTheme t {
        "Carbon",
        juce::Colour (0xff141419), juce::Colour (0xff24242e), juce::Colour (0xff2e2e3a),
        juce::Colour (0xff343440), juce::Colour (0xff3c3c4a), juce::Colour (0xff2a2a34),
        juce::Colour (0xffe8b84b), juce::Colour (0xff5b8db8),   // gold / steel
        juce::Colour (0xffe8e8ee), juce::Colour (0xff9a9aa8), juce::Colour (0xfff6f6fa),
        juce::Colour (0xffe8b84b), juce::Colour (0xff3c3c4a), juce::Colour (0xff5b8db8),
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

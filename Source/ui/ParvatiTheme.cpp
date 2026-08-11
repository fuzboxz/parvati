// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// ParvatiTheme — the 5 built-in themes. The factories initialise ParvatiTheme
// POSITIONALLY in field order (see ParvatiTheme.h), so a missed / extra / mis-
// ordered value silently misaligns every later field with NO compile error.
// Keep this list and every factory in lockstep with the struct, and keep the
// editor_test alignment guard green:
//   name (juce::String),
//   backgroundBase, backgroundPanel, backgroundInput, backgroundInputHover,
//   textPrimary, textSecondary, textDisabled, trackEmpty,
//   accentPrimary, accentSecondary,
//   outline, divider, containerFill, tabUnselectedBg, tabSelectedBg, keyWhite,
//   catAudio, catEnv, catLfo, catSeq, catArp,
//   catPerf, catUtil, catMod, catConst,
//   isDark (bool)  <-- trailing bool is a positional-init sentinel

#include "ParvatiTheme.h"

//==============================================================================
const ParvatiTheme& carbonTheme()
{
    // Default look, modernised flat palette: a muted near-black window with
    // slightly LIGHTER panel cards for tonal separation (no skeuomorphic
    // shadows/outlines). Cyan accent retained as the brand colour (was gold).
    static const ParvatiTheme t {
        "Carbon",
        // Layer 1 — BASE: dark base, lighter panels, a distinct recessed input
        // fill (the lighter panel2 tone) and its hover (one notch lighter still).
        juce::Colour (0xff15171C), juce::Colour (0xff1E2228), juce::Colour (0xff252A31),
        juce::Colour (0xff2B303A),
        // Layer 2 — INFORMATION: brightest primary, mid gray secondary, dimmer
        // disabled, recessed empty track.
        juce::Colour (0xfff6f6fa), juce::Colour (0xff9a9aa8), juce::Colour (0xff6B7280),
        juce::Colour (0xff3B3F46),
        // Layer 3 — ACTION: cyan brand / steel complementary.
        juce::Colour (0xff38BDF8), juce::Colour (0xff5b8db8),
        // auxiliary: outline, divider, containerFill, tabUnselectedBg, tabSelectedBg, keyWhite
        juce::Colour (0xff2C3138), juce::Colour (0xff2A2E35), juce::Colour (0xff1E2228),
        juce::Colour (0xff1A1E24), juce::Colour (0xff252A31), juce::Colour (0xffeeeae0),
        // modulation routing palette — Carbon overrides catAudio to the cyan brand
        // hue (was the shared family amber) so osc/filter previews match the accent.
        // Env=TEAL, LFO=MAGENTA, Seq/Arp=MINT (sequencer family).
        juce::Colour (0xff38BDF8), juce::Colour (0xff2DD4BF), juce::Colour (0xffE879F9),
        juce::Colour (0xff34D399), juce::Colour (0xff34D399),
        // catPerf, catUtil, catMod, catConst: Perf=PERIWINKLE, Util=NAVY (shifted cold —
        // was amber/orange; no warm hue remains in Carbon), Mod=PURPLE, Const=INDIGO
        juce::Colour (0xff9AB0D0), juce::Colour (0xff3D5A80), juce::Colour (0xffA78BFA),
        juce::Colour (0xff818CF8),
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
        // Layer 1 — BASE
        juce::Colour (0xff131922), juce::Colour (0xff1C2433), juce::Colour (0xff242E40),
        juce::Colour (0xff2B3649),
        // Layer 2 — INFORMATION
        juce::Colour (0xffeaf4fb), juce::Colour (0xff8a9bb0), juce::Colour (0xff6E7A8C),
        juce::Colour (0xff3B4354),
        // Layer 3 — ACTION: teal / blue
        juce::Colour (0xff2bb6c4), juce::Colour (0xff5b9bd5),
        // auxiliary
        juce::Colour (0xff2E3950), juce::Colour (0xff26303F), juce::Colour (0xff1C2433),
        juce::Colour (0xff182030), juce::Colour (0xff242E40), juce::Colour (0xffeceef1),
        // modulation routing palette — STRICT family hues (same family hues as Carbon):
        // Env=TEAL, LFO=MAGENTA, Seq/Arp=MINT, Perf=AMBER, Util=ORANGE, Mod=PURPLE, Const=INDIGO
        juce::Colour (0xffFFB400), juce::Colour (0xff2DD4BF), juce::Colour (0xffE879F9),
        juce::Colour (0xff34D399), juce::Colour (0xff34D399),
        juce::Colour (0xffFBBF24), juce::Colour (0xffFB923C), juce::Colour (0xffA78BFA),
        juce::Colour (0xff818CF8),
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
        // Layer 1 — BASE
        juce::Colour (0xff131318), juce::Colour (0xff1C1C26), juce::Colour (0xff252532),
        juce::Colour (0xff2D2D3B),
        // Layer 2 — INFORMATION
        juce::Colour (0xfff0ecfa), juce::Colour (0xff9a92b0), juce::Colour (0xff6C6878),
        juce::Colour (0xff3B3B49),
        // Layer 3 — ACTION: violet / fuchsia
        juce::Colour (0xff8b5cf6), juce::Colour (0xffd946ef),
        // auxiliary
        juce::Colour (0xff2C2C3C), juce::Colour (0xff262634), juce::Colour (0xff1C1C26),
        juce::Colour (0xff181820), juce::Colour (0xff252532), juce::Colour (0xffefeaf2),
        // modulation routing palette — STRICT family hues (same family hues as Carbon):
        // Env=TEAL, LFO=MAGENTA, Seq/Arp=MINT, Perf=AMBER, Util=ORANGE, Mod=PURPLE, Const=INDIGO
        juce::Colour (0xffFFB400), juce::Colour (0xff2DD4BF), juce::Colour (0xffE879F9),
        juce::Colour (0xff34D399), juce::Colour (0xff34D399),
        juce::Colour (0xffFBBF24), juce::Colour (0xffFB923C), juce::Colour (0xffA78BFA),
        juce::Colour (0xff818CF8),
        true
    };
    return t;
}

const ParvatiTheme& paperTheme()
{
    // Light theme. isDark = false so L&F can adapt contrast / focus rings.
    // Modernised flat palette: panel cards slightly DARKER than the page (tonal
    // contrast inverts on light themes); no skeuomorphic shadows/outlines. On a
    // light surface an input hover DARKENS (the opposite of the dark themes).
    static const ParvatiTheme t {
        "Paper",
        // Layer 1 — BASE
        juce::Colour (0xfff7f6f2), juce::Colour (0xffeceae4), juce::Colour (0xffe2dfd7),
        juce::Colour (0xffd8d4ca),
        // Layer 2 — INFORMATION: darkest primary text, mid gray secondary,
        // lighter disabled (less contrast on the light bg).
        juce::Colour (0xff11100e), juce::Colour (0xff6b6862), juce::Colour (0xff8a8780),
        juce::Colour (0xffD5D1C7),
        // Layer 3 — ACTION: deep amber / blue
        juce::Colour (0xffb45309), juce::Colour (0xff2563eb),
        // auxiliary
        juce::Colour (0xffc2beb3), juce::Colour (0xffdedbd1), juce::Colour (0xffeceae4),
        juce::Colour (0xffe0ddd3), juce::Colour (0xffece9e0), juce::Colour (0xfffbf9f4),
        // modulation routing palette — STRICT family hues, darker (600-tier) for clear
        // contrast on the light bg: Env=teal, LFO=magenta, Seq/Arp=mint, Perf=amber,
        // Util=orange, Mod=purple, Const=indigo
        juce::Colour (0xffB45309), juce::Colour (0xff0D9488), juce::Colour (0xffC026D3),
        juce::Colour (0xff059669), juce::Colour (0xff059669),
        juce::Colour (0xffD97706), juce::Colour (0xffEA580C), juce::Colour (0xff7C3AED),
        juce::Colour (0xff4F46E5),
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
        // Layer 1 — BASE
        juce::Colour (0xff1A0E0E), juce::Colour (0xff2A1818), juce::Colour (0xff341E1E),
        juce::Colour (0xff3C2424),
        // Layer 2 — INFORMATION
        juce::Colour (0xfff9eded), juce::Colour (0xffb08a8a), juce::Colour (0xff8A6E6E),
        juce::Colour (0xff4C3232),
        // Layer 3 — ACTION: crimson / blue
        juce::Colour (0xffe5484d), juce::Colour (0xff3b82f6),
        // auxiliary
        juce::Colour (0xff3E2424), juce::Colour (0xff2E1818), juce::Colour (0xff2A1818),
        juce::Colour (0xff201212), juce::Colour (0xff341E1E), juce::Colour (0xfff1ebe7),
        // modulation routing palette — STRICT family hues (same vivid family hues as
        // the other dark themes; catAudio left harmonized): Env=TEAL, LFO=MAGENTA,
        // Seq/Arp=MINT, Perf=AMBER, Util=ORANGE, Mod=PURPLE, Const=INDIGO
        juce::Colour (0xffE8923C), juce::Colour (0xff2DD4BF), juce::Colour (0xffE879F9),
        juce::Colour (0xff34D399), juce::Colour (0xff34D399),
        juce::Colour (0xffFBBF24), juce::Colour (0xffFB923C), juce::Colour (0xffA78BFA),
        juce::Colour (0xff818CF8),
        true
    };
    return t;
}

const ParvatiTheme& legacyTheme()
{
    // Legacy — a flat, matte LIGHT interface adopted from a reference module's
    // structural colour system (NOT a copy of Parvati's usual category hues).
    // isDark = false. Reference identities and their translation:
    //  - Main background #E8E8E8 -> backgroundBase (matte light gray, no gradients).
    //  - #333333 is the reference workhorse for labels / scale markers / grid
    //    lines / dividers -> textSecondary + outline; the in-panel divider uses a
    //    slightly lighter #555555 so it recedes below the structural border.
    //  - Control Group 1 (Neutral): off-white #F4F4F0 indicators on flat-black
    //    #1A1A1A bases -> keyWhite = cream; #1A1A1A = primary / active-value text.
    //  - Control Group 2 (Magenta #C8216A) is the LEAD accent -> accentPrimary
    //    (knob arcs, selected tabs, active fills) AND catAudio (the primary audio
    //    section's osc/filter previews then match the brand — cohesive). The
    //    reference's "white text in a magenta box" maps to accentPrimary fills.
    //  - Control Group 3 (Cyan #009696) is the complementary accent ->
    //    accentSecondary (bypass / visualizer / bipolar) AND catEnv (Parvati's
    //    teal Envelope hue adopted to the reference cyan).
    //  - Auxiliary Routing/Terminal blocks #6E7B8C with white text -> catUtil /
    //    catMod (utility + modifier routing); a darker variant #5A6776 -> catConst
    //    to keep the constant slot distinct.
    //  - State indicators pale-green #A8C69F + orange-gold #E5B55C -> catSeq/catArp
    //    (sequencer; Parvati's mint adopted to pale green) and catLfo/catPerf.
    //    The reference defines only ~5 non-neutral hues for 9 routing slots, so the
    //    LFO and Performance slots share the orange-gold (outside the first-five
    //    distinctness guard); catAudio/magenta stays DISTINCT from catLfo/gold so
    //    the positional-init guard holds.
    // Light-theme tonal steps follow paperTheme(): panel darker than the page and
    // an input hover DARKENS (opposite of the dark themes).
    static const ParvatiTheme t {
        "Legacy",
        // Layer 1 — BASE
        juce::Colour (0xffE8E8E8), juce::Colour (0xffDCDCDC), juce::Colour (0xffD2D2D2),
        juce::Colour (0xffC7C7C7),
        // Layer 2 — INFORMATION
        juce::Colour (0xff1A1A1A), juce::Colour (0xff333333), juce::Colour (0xff8A8A8A),
        juce::Colour (0xffBFBFBF),
        // Layer 3 — ACTION: magenta lead / cyan complementary
        juce::Colour (0xffC8216A), juce::Colour (0xff009696),
        // auxiliary: outline, divider, containerFill, tabUnselectedBg, tabSelectedBg, keyWhite
        juce::Colour (0xff333333), juce::Colour (0xff555555), juce::Colour (0xffDEDEDE),
        juce::Colour (0xffD8D8D8), juce::Colour (0xffC8C8C8), juce::Colour (0xffF4F4F0),
        // modulation routing palette — adopted from the reference identities:
        // Audio=MAGENTA(brand), Env=CYAN, Lfo=ORANGE-GOLD, Seq/Arp=PALE-GREEN
        juce::Colour (0xffC8216A), juce::Colour (0xff009696), juce::Colour (0xffE5B55C),
        juce::Colour (0xffA8C69F), juce::Colour (0xffA8C69F),
        // catPerf, catUtil, catMod, catConst
        juce::Colour (0xffE5B55C), juce::Colour (0xff6E7B8C), juce::Colour (0xff6E7B8C),
        juce::Colour (0xff5A6776),
        false
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
        crimsonTheme(),
        legacyTheme()
    };
    return v;
}

int kNumBuiltinThemes()
{
    return static_cast<int> (getBuiltinThemes().size());
}

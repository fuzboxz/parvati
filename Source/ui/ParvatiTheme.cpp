// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// ParvatiTheme — the 8 built-in themes. The factories initialise ParvatiTheme
// POSITIONALLY in field order (see ParvatiTheme.h), so a missed / extra / mis-
// ordered value silently misaligns every later field with NO compile error.
// Keep this list and every factory in lockstep with the struct, and keep the
// editor_test alignment guard green:
//   name (juce::String),
//   backgroundBase, backgroundPanel, backgroundInput, backgroundInputHover,
//   textPrimary, textSecondary, textDisabled, trackEmpty,
//   accentPrimary, accentSecondary,
//   outline, divider, containerFill, tabUnselectedBg, tabSelectedBg, keyWhite, keyBlack,
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
        // auxiliary: outline, divider, containerFill, tabUnselectedBg, tabSelectedBg,
        // keyWhite (natural-key elevated slate), keyBlack (near-background recessed)
        juce::Colour (0xff2C3138), juce::Colour (0xff2A2E35), juce::Colour (0xff1E2228),
        juce::Colour (0xff1A1E24), juce::Colour (0xff252A31), juce::Colour (0xff4A5361),
        juce::Colour (0xff15171C),
        // modulation routing palette — catAudio adopts the CYAN brand accent
        // (was the shared family amber) so osc/filter previews match the accent.
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
        // auxiliary: outline, divider, containerFill, tabUnselectedBg, tabSelectedBg,
        // keyWhite (blue-slate naturals), keyBlack (near-black blue recessed)
        juce::Colour (0xff2E3950), juce::Colour (0xff26303F), juce::Colour (0xff1C2433),
        juce::Colour (0xff182030), juce::Colour (0xff242E40), juce::Colour (0xff435470),
        juce::Colour (0xff0F141C),
        // modulation routing palette — catAudio adopts the BLUE complementary
        // accent (the primary teal would collide with the Env family teal on
        // knob rings/previews). Env=TEAL, LFO=MAGENTA, Seq/Arp=MINT, Perf=AMBER,
        // Util=ORANGE, Mod=PURPLE, Const=INDIGO (family palette unchanged)
        juce::Colour (0xff5b9bd5), juce::Colour (0xff2DD4BF), juce::Colour (0xffE879F9),
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
        // auxiliary: outline, divider, containerFill, tabUnselectedBg, tabSelectedBg,
        // keyWhite (violet-slate naturals), keyBlack (near-black recessed)
        juce::Colour (0xff2C2C3C), juce::Colour (0xff262634), juce::Colour (0xff1C1C26),
        juce::Colour (0xff181820), juce::Colour (0xff252532), juce::Colour (0xff4D4864),
        juce::Colour (0xff0F0F16),
        // modulation routing palette — catAudio adopts the VIOLET brand accent
        // (was the shared family amber) so osc/filter previews match the accent.
        // Env=TEAL, LFO=MAGENTA, Seq/Arp=MINT, Perf=AMBER, Util=ORANGE, Mod=PURPLE,
        // Const=INDIGO (family palette unchanged; the Mod pills keep PURPLE).
        juce::Colour (0xff8b5cf6), juce::Colour (0xff2DD4BF), juce::Colour (0xffE879F9),
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
        // auxiliary: outline, divider, containerFill, tabUnselectedBg, tabSelectedBg,
        // keyWhite (neutral near-white — NOT warm bone), keyBlack (warm charcoal)
        juce::Colour (0xffc2beb3), juce::Colour (0xffdedbd1), juce::Colour (0xffeceae4),
        juce::Colour (0xffe0ddd3), juce::Colour (0xffece9e0), juce::Colour (0xffFDFDFB),
        juce::Colour (0xff33302B),
        // modulation routing palette — catAudio adopts the BLUE accent (600-tier
        // for light-bg contrast; the primary is itself amber). Env=TEAL, LFO=MAGENTA,
        // Seq/Arp=MINT, Perf=AMBER, Util=ORANGE, Mod=PURPLE, Const=INDIGO
        // (family palette unchanged, darker 600-tier hues retained)
        juce::Colour (0xff2563eb), juce::Colour (0xff0D9488), juce::Colour (0xffC026D3),
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
        // auxiliary: outline, divider, containerFill, tabUnselectedBg, tabSelectedBg,
        // keyWhite (rosy-taupe naturals), keyBlack (near-black red recessed)
        juce::Colour (0xff3E2424), juce::Colour (0xff2E1818), juce::Colour (0xff2A1818),
        juce::Colour (0xff201212), juce::Colour (0xff341E1E), juce::Colour (0xff5F4448),
        juce::Colour (0xff120808),
        // modulation routing palette — catAudio adopts the CRIMSON brand accent
        // (was a harmonized amber-orange). Env=TEAL, LFO=MAGENTA,
        // Seq/Arp=MINT, Perf=AMBER, Util=ORANGE, Mod=PURPLE, Const=INDIGO
        // (same vivid family hues as the other dark themes; palette unchanged)
        juce::Colour (0xffe5484d), juce::Colour (0xff2DD4BF), juce::Colour (0xffE879F9),
        juce::Colour (0xff34D399), juce::Colour (0xff34D399),
        juce::Colour (0xffFBBF24), juce::Colour (0xffFB923C), juce::Colour (0xffA78BFA),
        juce::Colour (0xff818CF8),
        true
    };
    return t;
}

const ParvatiTheme& immutableTheme()
{
    // Immutable — a flat, matte LIGHT interface adopted from a reference module's
    // structural colour system (NOT a copy of Parvati's usual category hues).
    // isDark = false. Reference identities and their translation:
    //  - Main background #E8E8E8 -> backgroundBase (matte light gray, no gradients).
    //  - #333333 is the reference workhorse for labels / scale markers / grid
    //    lines / dividers -> textSecondary + outline; the in-panel divider uses a
    //    slightly lighter #555555 so it recedes below the structural border.
    //  - Control Group 1 (Neutral): off-white #F4F4F0-class indicators on flat-black
    //    #1A1A1A bases -> keyWhite = neutral off-white (the reference's cream,
    //    neutralised); keyBlack = the #1A1A1A bases; #1A1A1A also = primary / active-value text.
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
        "Immutable",
        // Layer 1 — BASE
        juce::Colour (0xffE8E8E8), juce::Colour (0xffDCDCDC), juce::Colour (0xffD2D2D2),
        juce::Colour (0xffC7C7C7),
        // Layer 2 — INFORMATION
        juce::Colour (0xff1A1A1A), juce::Colour (0xff333333), juce::Colour (0xff8A8A8A),
        juce::Colour (0xffBFBFBF),
        // Layer 3 — ACTION: magenta lead / cyan complementary
        juce::Colour (0xffC8216A), juce::Colour (0xff009696),
        // auxiliary: outline, divider, containerFill, tabUnselectedBg, tabSelectedBg,
        // keyWhite (neutral off-white — the reference's cream, neutralised),
        // keyBlack (the reference's flat-black #1A1A1A key bases)
        juce::Colour (0xff333333), juce::Colour (0xff555555), juce::Colour (0xffDEDEDE),
        juce::Colour (0xffD8D8D8), juce::Colour (0xffC8C8C8), juce::Colour (0xffF7F7F5),
        juce::Colour (0xff1A1A1A),
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
const ParvatiTheme& swedishRedTheme()
{
    // Swedish Red — a retro red-chassis instrument: the window fill is the
    // signature red, the module cards are charcoal (near-black "black panel"
    // plastic with grey control tones), and every DISPLAY family trace
    // (osc/filter waveform, envelope, LFO previews) renders in old-school
    // monochrome LCD greens so those read as backlit green screens set into
    // the dark cards. The brand accent is a signal red for knob arcs / active
    // fills; the secondary accent is the cool knob grey (bypass / bipolar).
    // The modulation palette keeps the greens DISTINCT (audio = phosphor
    // yellow-green, env = LCD green, lfo = green-cyan — pairwise distinct so
    // the positional-init guard stays full-strength) while the non-display
    // families go monochrome neutral greys, with the performance family in
    // off-white and utility/modifier/constant in stepped knob-greys — a red
    // machine, grey controls, green screens. Dark theme.
    static const ParvatiTheme t {
        "Swedish Red",
        // Layer 1 — BASE: red chassis, charcoal cards, recessed input + hover
        juce::Colour (0xffB32328), juce::Colour (0xff26262A), juce::Colour (0xff313135),
        juce::Colour (0xff3A3A3F),
        // Layer 2 — INFORMATION: silkscreen off-white values, warm grey labels
        juce::Colour (0xffF4F1E8), juce::Colour (0xffC3BDB0), juce::Colour (0xff8B877E),
        juce::Colour (0xff414146),
        // Layer 3 — ACTION: signal red / cool knob grey
        juce::Colour (0xffE8443A), juce::Colour (0xffB9BDC4),
        // auxiliary: outline, divider, containerFill, tabUnselectedBg, tabSelectedBg,
        // keyWhite (light knob grey naturals), keyBlack (near-black recessed)
        juce::Colour (0xff45454B), juce::Colour (0xff3A3A40), juce::Colour (0xff26262A),
        juce::Colour (0xff1E1E22), juce::Colour (0xff313135), juce::Colour (0xffD9DAD6),
        juce::Colour (0xff1A1A1E),
        // modulation routing palette — the DISPLAY families (audio/env/lfo)
        // are the LCD greens so the waveform/env/LFO previews read as green
        // screens; the rest are neutral greys (seq/arp shared warm grey).
        juce::Colour (0xff9BE24A), juce::Colour (0xff57E05C), juce::Colour (0xff2FD98C),
        juce::Colour (0xffD6D2C4), juce::Colour (0xffD6D2C4),
        // catPerf, catUtil, catMod, catConst: off-white + stepped knob greys
        juce::Colour (0xffF0EDE2), juce::Colour (0xff9BA0A2), juce::Colour (0xffC0BBAC),
        juce::Colour (0xff6B7072),
        true
    };
    return t;
}

//==============================================================================
const ParvatiTheme& y2kTheme()
{
    // Y2K — the HARDWARE restyle (2026-08-24): a silver desktop, dark module
    // panels, black data screens and LED readouts. The base is FLAT SILVER
    // (the Win98 chrome desktop #C0C0C0). Module panels sit mid-grey #808080
    // with the subtle liquid-chrome sweep from paintChromeCard. Data wells
    // are near-black; knob tracks are solid black; the routing palette is
    // PURE saturated hex on the dark surfaces. Type: Michroma headers, PT Sans
    // labels, VT323 LED data text (all OFL, see assets/fonts).
    // isDark = FALSE (audited): every isDark consumer is the dark-dropdown /
    // dark-text convention (drawComboBox fill+chevron, ComboBox::textColourId,
    // SeqLengthStepper) — the false branch gives the fixed near-black
    // dropdown with light text, which is exactly the recessed-well look this
    // world wants. The LIGHT branch of keyboard_view_test then checks the
    // SHARPS on the grey panel (correct: the pearl naturals sit on silver).
    static const ParvatiTheme t {
        "Y2K",
        // Layer 1 — BASE: flat silver desktop; module panels mid-grey;
        // recessed data wells near-black with a one-notch hover lift.
        juce::Colour (0xffC0C0C0), juce::Colour (0xff051A05), juce::Colour (0xff1A1A1A),
        juce::Colour (0xff262626),
        // Layer 2 — INFORMATION: white values on the dark panels, light grey
        // labels, the era disabled grey, SOLID BLACK knob empty tracks.
        juce::Colour (0xffFFFFFF), juce::Colour (0xffC8C8C8), juce::Colour (0xff6E6E6E),
        juce::Colour (0xff000000),
        // Layer 3 — ACTION: electric cyan brand / magenta complement.
        juce::Colour (0xff00FFFF), juce::Colour (0xffFF00FF),
        // auxiliary: outline (dark steel), divider, containerFill (the
        // module-panel grey; #565656 keeps white headers at the 7:1 tier),
        // tabUnselectedBg / tabSelectedBg — DESATURATED: tabs are STRUCTURE,
        // so their fills go neutral (unselected dark steel, selected the
        // panel grey); the neon lives in the active-state INDICATOR strip the
        // L&F draws, never in a full-block fill (visual-vibration fix).
        juce::Colour (0xff404040), juce::Colour (0xff505050), juce::Colour (0xff565656),
        juce::Colour (0xff3A3A3A), juce::Colour (0xff6E6E6E), juce::Colour (0xffE8E8E8),
        juce::Colour (0xff101010),
        // modulation routing palette — PURE saturated hex, the LED colour
        // school: Audio=CYAN, Env=MAGENTA, Lfo=ACID YELLOW, Seq/Arp=PURE BLUE.
        juce::Colour (0xff00FFFF), juce::Colour (0xffFF00FF), juce::Colour (0xffCCFF00),
        juce::Colour (0xff0000FF), juce::Colour (0xff0000FF),
        // catPerf, catUtil, catMod, catConst: RED, GREEN, MAGENTA, WHITE.
        juce::Colour (0xffFF0000), juce::Colour (0xff00FF00), juce::Colour (0xffFF00FF),
        juce::Colour (0xffFFFFFF),
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
        immutableTheme(),
        swedishRedTheme(),
        y2kTheme()
    };
    return v;
}

int kNumBuiltinThemes()
{
    return static_cast<int> (getBuiltinThemes().size());
}

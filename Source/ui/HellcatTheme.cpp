// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.
//
// HellcatTheme — the 8 built-in themes. The factories initialise HellcatTheme
// POSITIONALLY in field order (see HellcatTheme.h), so a missed / extra / mis-
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

#include "HellcatTheme.h"

//==============================================================================
const HellcatTheme& carbonTheme()
{
    // Default look, modernised flat palette: a muted near-black window with
    // slightly LIGHTER panel cards for tonal separation (no skeuomorphic
    // shadows/outlines). Cyan accent retained as the brand colour (was gold).
    static const HellcatTheme t {
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

const HellcatTheme& midnightTheme()
{
    // Dark blue / teal — modernised flat palette (lighter panel cards, no
    // skeuomorphic shadows/outlines).
    static const HellcatTheme t {
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

const HellcatTheme& immutableTheme()
{
    // Immutable — a flat, matte LIGHT interface adopted from a reference module's
    // structural colour system (NOT a copy of Hellcat's usual category hues).
    // isDark = false. Reference identities and their translation:
    //  - Main background: the reference #E8E8E8, lifted to #EFEFEF for the
    //    2026-08-26 WCAG pass (see the factory note) -> backgroundBase.
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
    //    accentSecondary (bypass / visualizer / bipolar) AND catEnv (Hellcat's
    //    teal Envelope hue adopted to the reference cyan).
    //  - Auxiliary Routing/Terminal blocks #6E7B8C with white text -> catUtil /
    //    catMod (utility + modifier routing); a darker variant #5A6776 -> catConst
    //    to keep the constant slot distinct.
    //  - State indicators pale-green #A8C69F + orange-gold #E5B55C -> catSeq/catArp
    //    (sequencer; Hellcat's mint adopted to pale green) and catLfo/catPerf.
    //    The reference defines only ~5 non-neutral hues for 9 routing slots, so the
    //    LFO and Performance slots share the orange-gold (outside the first-five
    //    distinctness guard); catAudio/magenta stays DISTINCT from catLfo/gold so
    //    the positional-init guard holds.
    // Light-theme tonal steps: panel darker than the page and an input hover
    // DARKENS (opposite of the dark themes).
    static const HellcatTheme t {
        "Immutable",
        // Layer 1 — BASE. backgroundBase EFEFEF (2026-08-26 WCAG pass: the
        // reference E8E8E8 left toggled-button text at 4.42:1 on the magenta
        // lead accent; one step lighter clears 4.71:1 and the matte gray
        // identity stays).
        juce::Colour (0xffEFEFEF), juce::Colour (0xffDCDCDC), juce::Colour (0xffD2D2D2),
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
const HellcatTheme& swedishRedTheme()
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
    static const HellcatTheme t {
        "Swedish Red",
        // Layer 1 — BASE: red chassis, charcoal cards, recessed input + hover
        juce::Colour (0xffB32328), juce::Colour (0xff26262A), juce::Colour (0xff313135),
        juce::Colour (0xff3A3A3F),
        // Layer 2 — INFORMATION: silkscreen off-white values, warm grey labels.
        // textSecondary DCD6CA (2026-08-26 WCAG pass: the C3BDB0 silkscreen sat
        // at 3.52:1 on the red chassis — the Mod Matrix rows paint labels on
        // it; one step brighter clears 4.55:1 and the warm grey stays).
        juce::Colour (0xffF4F1E8), juce::Colour (0xffDCD6CA), juce::Colour (0xff8B877E),
        juce::Colour (0xff414146),
        // Layer 3 — ACTION: signal red / cool knob grey. accentPrimary C93028
        // (2026-08-26 WCAG pass: E8443A left toggled-button text at 3.5:1;
        // the deeper red clears 4.72:1 with the silkscreen off-white text).
        juce::Colour (0xffC93028), juce::Colour (0xffB9BDC4),
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
const HellcatTheme& y2kTheme()
{
    // Y2K — the HARDWARE restyle (2026-08-24): a gunmetal desktop, dark module
    // panels, black data screens and LED readouts. The base is FLAT GUNMETAL
    // (#808080 — the brushed-steel desktop that matches the chrome cards;
    // the former Win98 silver #C0C0C0 is retired). Module panels sit the
    // card steel #565656. Data wells
    // are near-black; knob tracks are solid black; the routing palette is
    // PURE saturated hex on the dark surfaces. Type: Michroma headers and
    // labels (all OFL, see assets/fonts). The former VT323 data face is
    // retired; every data readout uses the default app font.
    // isDark = FALSE (audited): every isDark consumer is the dark-dropdown /
    // dark-text convention (drawComboBox fill+chevron, ComboBox::textColourId,
    // SeqLengthStepper) — the false branch gives the fixed near-black
    // dropdown with light text, which is exactly the recessed-well look this
    // world wants. The LIGHT branch of keyboard_view_test then checks the
    // SHARPS on the grey panel (correct: the pearl naturals sit on silver).
    static const HellcatTheme t {
        "Y2K",
        // Layer 1 — BASE: flat gunmetal desktop (matches the chrome cards);
        // module panels the card steel; recessed data wells near-black with
        // a one-notch hover lift.
        juce::Colour (0xff808080), juce::Colour (0xff051A05), juce::Colour (0xff1A1A1A),
        juce::Colour (0xff262626),
        // Layer 2 — INFORMATION: white values on the dark panels, light grey
        // labels (CFCFCF since the 2026-08-26 WCAG pass: C8C8C8 sat at 4.39:1
        // on the steel containerFill the Mod Matrix uses), the era disabled
        // grey, SOLID BLACK knob empty tracks.
        juce::Colour (0xffFFFFFF), juce::Colour (0xffCFCFCF), juce::Colour (0xff6E6E6E),
        juce::Colour (0xff000000),
        // Layer 3 — ACTION: accentPrimary is THE LCD GREEN (the one accent
        // every Y2K indicator carries: knob arcs, pills, readouts, tab strips).
        // accentSecondary is a DIM CYAN-TEAL (bipolar / negative halves).
        juce::Colour (0xff3FBF3F), juce::Colour (0xff2FB8C9),
        // auxiliary: outline (dark steel), divider, containerFill (the
        // module-panel grey; #565656 keeps white headers at the 7:1 tier),
        // tabUnselectedBg / tabSelectedBg — DESATURATED: tabs are STRUCTURE,
        // so their fills go neutral (unselected dark steel, selected the
        // panel grey); the neon lives in the active-state INDICATOR strip the
        // L&F draws, never in a full-block fill (visual-vibration fix).
        juce::Colour (0xff404040), juce::Colour (0xff505050), juce::Colour (0xff565656),
        juce::Colour (0xff3A3A3A), juce::Colour (0xff6E6E6E), juce::Colour (0xffE8E8E8),
        juce::Colour (0xff101010),
        // modulation routing palette — the CALM family (2026-08-25: the
        // pure-hex neon vibrated; hue identity kept, chroma dropped). Y2K
        // routes most accents through the LCD green, so this table is the
        // documented fallback: Audio=LCD GREEN, Env=SOFT MAGENTA,
        // Lfo=AMBER, Seq/Arp=STEEL BLUE, Perf=RUST, Util=OLIVE, Mod=TEAL,
        // Const=LIGHT GREY.
        juce::Colour (0xff3FBF3F), juce::Colour (0xffC45AB8), juce::Colour (0xffD9A441),
        juce::Colour (0xff5577CC), juce::Colour (0xff5577CC),
        // catPerf, catUtil, catMod, catConst.
        juce::Colour (0xffC4705A), juce::Colour (0xff8F9F5F), juce::Colour (0xff2FB8C9),
        juce::Colour (0xffD9D9D9),
        false
    };
    return t;
}

//==============================================================================
const std::vector<HellcatTheme>& getBuiltinThemes()
{
    static const std::vector<HellcatTheme> v {
        carbonTheme(),
        midnightTheme(),
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

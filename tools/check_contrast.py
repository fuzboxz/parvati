#!/usr/bin/env python3
# ---------------------------------------------------------------------------
# tools/check_contrast.py — WCAG 2.1 contrast gate for the Hellcat themes.
#
# Parses Source/ui/HellcatTheme.cpp (the five theme factories), maps every
# juce::Colour literal POSITIONALLY onto the HellcatTheme field order (the
# order documented at the top of that file), then checks the text/background
# pairs the LookAndFeel actually draws (Source/ui/HellcatLookAndFeel.cpp):
#
#   normal text (4.5:1):
#     textPrimary   / backgroundPanel     knob values, card text, headers
#     textPrimary   / backgroundBase      window-level text (not Y2K: its
#                                         desktop is chrome, no text paints
#                                         on it — ParamPage/GroupPager/
#                                         GeneratorHost all skip the fill and
#                                         the Mod Matrix uses containerFill)
#     textPrimary   / backgroundInput     combo + dropdown text
#     textPrimary   / containerFill       functional-panel text (mod bar)
#     textSecondary / backgroundPanel     parameter labels
#     textSecondary / backgroundBase      matrix row labels (not Y2K, as above)
#     textSecondary / containerFill       labels inside containers
#     textPrimary   / tabSelectedBg       front tab label
#     textSecondary / tabUnselectedBg     inactive tab label
#     backgroundBase / accentPrimary      toggled-button text on the accent
#                                         fill (Y2K: backgroundPanel — the
#                                         L&F routes the on-text through the
#                                         panel black on Y2K chrome)
#     accentPrimary / backgroundPanel     Y2K only: the knob value readout is
#                                         routed through the accent (the L&F
#                                         special-cases Y2K chrome)
#   exempt (reported, never fails):
#     textDisabled  / backgroundPanel     WCAG 1.4.3 exempts inactive
#                                         user-interface components
#
# Usage:
#   python3 tools/check_contrast.py            # all themes; exit 1 on any FAIL
#   python3 tools/check_contrast.py Y2K        # one theme by name
#
# The theme field order is positional-init (see HellcatTheme.h); a mis-ordered
# factory misaligns every later field, so this script doubles as a coarse
# guard: a token that lands wildly off-tier shows up as a contrast FAIL.
# ---------------------------------------------------------------------------
import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "Source/ui/HellcatTheme.cpp"

# Positional field order (HellcatTheme.h struct order, after `name`).
FIELDS = [
    "backgroundBase", "backgroundPanel", "backgroundInput", "backgroundInputHover",
    "textPrimary", "textSecondary", "textDisabled", "trackEmpty",
    "accentPrimary", "accentSecondary",
    "outline", "divider", "containerFill", "tabUnselectedBg", "tabSelectedBg",
    "keyWhite", "keyBlack",
    "catAudio", "catEnv", "catLfo", "catSeq", "catArp",
    "catPerf", "catUtil", "catMod", "catConst",
]

# (text token, bg token, class, theme filter or None). The filter is a
# comma list: "A,B" = only those themes; "-A" = every theme except A.
PAIRS = [
    ("textPrimary",    "backgroundPanel",  "normal", None),
    ("textPrimary",    "backgroundBase",   "normal", "-Y2K"),
    ("textPrimary",    "backgroundInput",  "normal", None),
    ("textPrimary",    "containerFill",    "normal", None),
    ("textSecondary",  "backgroundPanel",  "normal", None),
    ("textSecondary",  "backgroundBase",   "normal", "-Y2K"),
    ("textSecondary",  "containerFill",    "normal", None),
    ("textPrimary",    "tabSelectedBg",    "normal", None),
    ("textSecondary",  "tabUnselectedBg",  "normal", None),
    # Toggled-button on-text follows the L&F toggledOnTextColour per theme.
    ("backgroundBase",  "accentPrimary",   "normal", "-Y2K,-Swedish Red"),
    ("backgroundPanel", "accentPrimary",   "normal", "Y2K"),
    ("textPrimary",     "accentPrimary",   "normal", "Swedish Red"),
    ("accentPrimary",  "backgroundPanel",  "normal", "Y2K"),   # knob readout
    ("textDisabled",   "backgroundPanel",  "exempt", None),
]


def filter_matches (theme_name, flt):
    if flt is None:
        return True
    for part in flt.split(","):
        if part.startswith("-"):
            if theme_name == part[1:]:
                return False
        elif theme_name == part:
            return True
    return flt.startswith("-") if flt else True

THRESHOLD = {"normal": 4.5, "exempt": 0.0, "large": 3.0}


def rel_luminance(rgb):
    def chan(c):
        c = c / 255.0
        return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4
    r, g, b = (chan(v) for v in rgb)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def contrast(a, b):
    la, lb = rel_luminance(a), rel_luminance(b)
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)


def parse_themes(text):
    """Extract each factory's name + positional colour list."""
    factories = re.findall(
        r"static const HellcatTheme t \{(.*?)\n    return t;",
        text, re.S)
    themes = []
    for body in factories:
        name = re.search(r'"([^"]+)"', body).group(1)
        cols = [int(h, 16) for h in re.findall(r"juce::Colour\s*\(\s*0x([0-9A-Fa-f]{8})\s*\)", body)]
        if len(cols) != len(FIELDS):
            raise SystemExit(f"theme '{name}': {len(cols)} colours, expected {len(FIELDS)} "
                             "(positional init drifted from HellcatTheme.h)")
        themes.append((name, dict(zip(FIELDS, [(c >> 16 & 255, c >> 8 & 255, c & 255) for c in cols]))))
    return themes


def main():
    text = SRC.read_text()
    themes = parse_themes(text)
    if not themes:
        raise SystemExit("no theme factories parsed — HellcatTheme.cpp drifted")
    only = sys.argv[1] if len(sys.argv) > 1 else None
    fails = 0
    for name, tokens in themes:
        if only and name != only:
            continue
        print(f"--- {name} ---")
        for tok, bg, cls, theme_filter in PAIRS:
            if not filter_matches (name, theme_filter):
                continue
            ratio = contrast(tokens[tok], tokens[bg])
            need = THRESHOLD[cls]
            ok = ratio >= need
            tag = "PASS" if ok else "FAIL"
            tag = "EXEMPT" if cls == "exempt" else tag
            print(f"  {tag} {tok:>14} / {bg:<15} {ratio:5.2f}:1  (need {need:.1f})")
            if cls != "exempt" and not ok:
                fails += 1
    if fails:
        print(f"\ncheck_contrast: {fails} FAIL")
        return 1
    print("\ncheck_contrast: PASS (every enforced pair meets its threshold)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

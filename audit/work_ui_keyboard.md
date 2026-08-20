# Themed keyboard (KeyboardView) — modernization report

## Root cause of the "bone keys"
`keyWhite` carried warm ivory values in every factory (Carbon `0xffeeeae0`,
Midnight `0xffeceef1`, Crimson `0xfff1ebe7`…) — drawn directly as the natural-key
fill. Sharps used `backgroundBase`, which on the light themes is near-white
(Paper: sharps ≈ naturals — near-invisible). All fixed at the token level.

## Changes

**ParvatiTheme** (1 token added, documented in-field):
- `keyWhite` re-specced: *theme-matched elevated surface* — slate step above the
  panels on dark themes, neutral near-white on light themes (never piano ivory).
- `keyBlack` (NEW, after keyWhite): recessed near-background tone (dark themes) /
  dark key-charcoal (light themes).

| Theme | keyWhite (naturals) | keyBlack (sharps) | natural/sharp WCAG |
|---|---|---|---|
| Carbon | `0xff4A5361` slate | `0xff15171C` | **2.31:1** |
| Midnight | `0xff435470` blue-slate | `0xff0F141C` | **2.41:1** |
| Obsidian | `0xff4D4864` violet-slate | `0xff0F0F16` | **2.21:1** |
| Paper | `0xffFDFDFB` neutral white | `0xff33302B` charcoal | **12.90:1** |
| Crimson | `0xff5F4448` rosy-taupe | `0xff120808` | **2.26:1** |
| Legacy | `0xffF7F7F5` neutral off-white | `0xff1A1A1A` | **16.23:1** |

Presses = `accentPrimary` (unchanged semantics, now far from the resting fills on
dark themes), hover = 0.14/0.30-alpha `accentSecondary` wash, separators =
`outline` @ 0.25, panel = `containerFill`. C-labels auto-contrast
(`fill.contrasting()`), now dark-on-slate instead of dark-on-ivory.

**KeyboardView**: new public `KeyboardColours` struct + static
`resolveColours(lnf)` (Parvati L&F theme → else Carbon's tokens; **zero colour
literals remain in the .cpp** — the old fallback literals are gone). Modern
styling per the L&F card idiom: naturals get ~2px rounded key-fronts (bottom
corners, integer-snapped), sharps keep rounded top caps and gain a pressed
accent baseline. `paint()`/`applyThemeColours()` resolve through the same
palette (vestigial stock IDs now theme-true). Hit-testing unchanged (geometry
overrides untouched). Theme switches re-tint via the existing per-paint
resolution + `refresh()`.

## Tests (`tests/keyboard_view_test.cpp` [8], 30 new checks)
Per shipped theme: (a) natural/sharp/pressed equal **no stock JUCE
MidiKeyboardComponent default** (`0xffffffff`, bone `0xfff0f0f0`, `0xff000000`,
`0x80ffff00`, `0xffb6b600`); (b) resolver mirrors the tokens
(`natural==keyWhite`, `sharp==keyBlack`); (c) pressed ≠ idle; (d) natural/sharp
WCAG ≥ 1.6:1 (measured 2.21–16.23); polarity-aware panel step ≥ 1.3:1 (dark:
naturals off the panel; light: sharps off it — light-theme near-white naturals
on a light panel are delineated by separators + dark sharps); (e) all 6 themes
resolve distinct naturals; (f) live re-resolve on `setTheme` + `refresh()`.

## Results
- `parvati_keyboard_view_test`: **PASS (0 failures)** incl. all [0]–[7] interaction checks.
- `parvati_ipad_hig_sizing_test`: **ALL CHECKS PASSED** (strip geometry intact).
- `parvati_multigui_test`: 187 ok; theme positional-init guard [4] fully green
  (keyWhite opaque + distinct per theme). The only 4 failures are section [19]
  (preview refresh) — the concurrent preview-fix lane's files
  (Envelope/FilterResponse/OscPreviewDisplay), which this lane never touched;
  verified via stash isolation that they track those files, not the keyboard.

## Residual risks
- Natural-key luminance on dark themes is a deliberate mid-slate; anyone
  preferring brighter keys retunes one token per factory (test margin 1.6 allows it).
- `keyBlack` ≈ `backgroundBase` on dark themes: sharps read by contrast against
  the naturals (2.2:1+) and their position/shape, as before.
- No CMake change needed (none made).

# Top-bar chrome polish — work report (PluginEditor.{h,cpp} + editor_test.cpp)

Base tree carries concurrent sibling-lane edits (L&F defaults/theme tokens, mod-matrix, stepper font); all results below were measured against that merged state.

## 1. Version ↔ patch-indicator separation
**Root cause found (not just a spacing tweak):** the brand block was sized to the **bold wordmark alone** (`textW + 16`), but the version subtitle `by 805Labs · v<ver>` painted in the same block at 10px is **wider** than the 22px-bold "Parvati" — so the version text overhung the block's slack and nearly touched the preset dropdown.

**Fix:** measure the subtitle glyph width (same font `paint()` uses) and size the block to `max(wordmark, subtitle) + 18px`. Version stays left in the brand block (moving it far-right would collide with the folding icon cluster; documented in-code). Visible gap version→patch indicator now ≥ ~18px (pinned ≥10).

**Budget rebalance:** logo grew ~+17px; reclaimed −20px from Save 64→52, Load 54→48, history separator 6→4 so the 1024px-min-width contract still closes with ~9pt slack. W9 fold-budget comment recomputed (right cluster 486→466 full / 310 / 258; left overhead 106→123).

## 2. Slimmer top-row buttons
Desktop visual height **36pt**, vertically centred in the unchanged 44pt strip, applied to every header control (7 icon/mode cells, Load/Save, `[Synth]/[FX]/[Patch]`, part combo) via a `slimCell` helper. **iOS keeps full 44pt cells** (HIG touch floor) via `#if JUCE_IOS` — preprocessor, not a ternary: `JUCE_IOS` is *undefined* on non-iOS builds and an expression would reject it as an undeclared identifier (compile-verified). `kHeaderH`/`kBarHeight` untouched → HIG static-asserts unchanged and passing. Hit areas on desktop shrink to the 36pt visual bounds (mouse-only platforms; iOS unaffected).

## 3. Clickable affordance for unselected items
New `applyHeaderButtonChrome()`, called from `applyAllColoursFromTheme()` (ctor first-paint + every theme switch):
- **fill:** `accentSecondary.withAlpha(0x2A)` ≈ 16% (≤ 0.35 as specified) — a themed colour wash replacing the flat `backgroundPanel`
- **text:** `textPrimary` (bright tier)
- **selected/on:** unchanged L&F `buttonOnColourId` (solid accent + dark text) → clearly stronger than the wash; hover/press still derive in `drawButtonBackground` (brighter/darker)

Applied to: Synth/FX/Patch, KBD/MOD/MAP, Load/Save, "..." overflow, **and the patch indicator** (PresetBrowser's name button, reached as its single child TextButton — PresetBrowser.h is outside this lane's ownership). No hardcoded colours; everything resolves from the active theme.

## Tests — editor_test [20] (runs in `parvati_multigui_test`)
(a) brand block ≥ subtitle width + 12; visible version→preset gap ≥ 10px; (b) every direct-child header TextButton/ComboBox height ∈ [30,38] and < 44 on this desktop build, ≥8 controls pinned; (c) per-button: fill alpha ∈ (0, 0.35], ≠ `backgroundPanel`, ≠ on-colour, == the accentSecondary wash, text == `textPrimary`; patch-indicator text == `textPrimary` + wash. New test accessor `ParvatiEditor::getLogoAreaForTest()`.

## Results
`parvati_multigui_test` ALL PASS (incl. [20]); `parvati_ipad_hig_sizing_test` ALL PASS; `parvati_lifecycle_test` ALL PASS; `parvati_layout_minwidth_test` / `parvati_layout_overlap_test` / `parvati_ui_mirror_test` / `parvati_editor_test` ALL PASS; full `cmake --build build` 0 errors.

## Residual risks
- Save/Load width trim (52/48) is comfortable for the short labels but assumes no longer localized strings (current FR/DE also short).
- The wash composes over the sibling L&F lane's default changes by design (explicit per-component IDs win); if they later re-spec `buttonOnColourId` the on-state still dominates (alpha 1.0 vs 0.16).
- iOS 44pt path is compile-gated, not exercised by this desktop test run (HIG static-asserts still pin the constants).

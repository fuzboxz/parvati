# Lane B — mod-matrix index + unified disable widget + header colour parity

## 1. Index label ('...' instead of '16') — ROOT CAUSE + FIX
**Root cause (measured, not guessed):** JUCE `Label`'s default border is
**5px per side** (`getBorderSize()` → 5,5,1,1). The 18pt-wide index label
therefore had an **8px text box** while "16" at the 12pt app font measures
**13px** (`GlyphArrangement::getStringWidthInt`) → guaranteed ellipsis.
**Fix:** both `ModMatrixView.cpp` and `FxMatrixView.cpp` rows zero the label
border (`setBorderSize(0)`; the text is centred anyway) and allocate
`kMatrixIndexLabelW = 20` (13px text + slack, floored at the old 18).
Pinned by mod_matrix_ui_test **[7]**: `width >= textWidth + 2`, border 0.

## 2. Unified disable widget — ParvatiModuleLamp
The three prior implementations differed: `MuteLamp`/`FxMuteLamp` filled with
`accentPrimary` (~12pt dot) while the FX card's `PowerToggle` filled with
**`accentSecondary`** — the style mismatch the user reported.
**Fix:** one shared `ParvatiModuleLamp` (declared in HellcatLookAndFeel.h,
painted in the .cpp). All three call sites are now thin `final` subclasses.
- Colour: `accentPrimary` on / `textDisabled` off / `outline` ring
  brightening on hover — identical resolution everywhere.
- Size: dot = `jlimit(8, 30, jmin(w,h) * 0.68)` — the matrix rows' 44pt
  bands render a **28-30pt dot** (the earlier "a bit bigger" request); the
  FX card's 44×22 header band renders **~15pt** (proportional, geometry
  preserved). Hit areas stay the full bounds (44pt HIG).
- The card keeps lamp-centre pinning (`setLampCentreOffset`, now on the
  shared class); `kLampDotW` 12→15 so the title keeps its gap.

## 3. Header colour parity
**Root cause:** two painting paths resolved different tokens — synth module
headers are GroupComponent titles (`GroupComponent::textColourId` =
**`textPrimary`** via the L&F), while `FxSlotCard::paint` used
**`textSecondary`**. **Fix:** the card title resolves `textPrimary` (per
paint, so theme switches re-colour both paths together). Test hooks:
`FxSlotCard::headerTitleColourForTest()` / `powerLampForTest()`,
`ParvatiModuleLamp::resolvedOnColourForTest()` / `dotDiameterFor()`.

## 4. Tests — all green (repo root)
mod_matrix_ui **[1]-[8]** (new [7] index no-ellipsis, [8] shared-widget type
checks + per-theme lamp colour equality synth-vs-FX + FX header ==
theme.textPrimary + 28-30pt dot @ 44 band), workspace_padding, multigui,
lifecycle, keyboard_view, ipad_hig_sizing, layout_minwidth, layout_overlap —
**8/8 PASS**. Test-side note: the parity section installs a real
`ParvatiLookAndFeel` on the hosts (lamps resolve through the inherited L&F;
without it both would fall back and "equal" would be vacuous).

## 5. Commit
One commit `fix(ui): mod-matrix index spacing, unified disable widget,
header colour parity` — NOT pushed. Excludes the sibling lane's untracked
`audit/modrouting_investigation.md` and its in-flight PatchPage/plugin work.

## Residual risks
- The FX card's lamp stays ~15pt (band-limited): forcing 28pt would need a
  44pt-tall header band — the geometry the layout-overlap gate explicitly
  rejected (it overlapped the centred type combo at 800×400).
- `kMatrixIndexLabelW` is duplicated per-view (deliberate: the views build
  independently; [7] pins both).
- accentSecondary is still used by the FX visualizer/type-combo chrome — only
  the disable widget was unified, per the report.

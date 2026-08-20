# Typography + module-header contrast — fix report (UI feedback 2026-08-20)

## 1. Root cause of the "seq dropdown font too big"

TWO offenders, both in this lane's files:

1. **The SEQ length control IS a dropdown**: tapping the number opens a real
   16-row picker (`SeqLengthStepper::showLengthPopup`, 44pt HIG rows). Its
   inline number was **17pt bold** (history: 15pt at birth → 17pt in the iOS
   wave `2f68b5d`) — the single largest text on the SEQ page (knob labels 12pt,
   knob centre readouts ~11–14pt radius-scaled, combos/buttons 14pt).
2. **The PopupMenu item font was 15pt** while the combo/button font was 14pt
   (`getPopupMenuFont` vs `getComboBoxFont`) — every dropdown *list* read a
   step larger than the combo text it came from.

**Fix**: number → **14pt bold** (matches the app-control height exactly; bold
retained as the value-tier emphasis); `getPopupMenuFont` 15 → **14pt plain** —
unified in the L&F so every dropdown (incl. the seq picker, save-format menu,
FX type picker) is consistent, per the task's "fix in the L&F for consistency
everywhere".

## 2. Module headers raised to the textPrimary tier

Group titles ("FX", "Sequencer", "Osc 1", …) were painted with
`GroupComponent::textColourId == textSecondary` (the dim caption tier) over the
`containerFill` card. One-line L&F change (`setColour(... t.textPrimary)` in
`ParvatiLookAndFeel`'s colour map + comment/doc updates in
`drawGroupComponentOutline` and `ParvatiTheme.h`); titles stay 14pt bold — the
complaint was contrast, not size, and the bold weight already carries the
hierarchy.

Measured WCAG contrast (title vs the card it sits on — `containerFill`;
containerFill == backgroundPanel on 5 of 6 themes):

| Theme | before (textSecondary) | after (textPrimary) |
|---|---|---|
| Carbon | 5.75:1 | **14.82:1** |
| Midnight | 5.48:1 | **13.95:1** |
| Obsidian | 5.71:1 | **14.54:1** |
| Paper (light) | 4.61:1 | **15.81:1** |
| Crimson | 5.51:1 | **14.78:1** |
| Legacy (light) | 9.39:1 | **12.94:1** |

All ≥ 7:1 target; light themes IMPROVE (no regression). No other widget uses
the GroupComponent colour ID (grep: single setter, single reader), so nothing
else shifts tier.

## 3. Tests

- **NEW `tests/ui_typography_test.cpp`** → `parvati_ui_typography_test`
  (CMake appended at end): [1] per theme — title == textPrimary, ≠ secondary,
  ≥7:1 vs containerFill AND backgroundPanel, ≥2:1 more contrast than the
  secondary tier, theme-switch re-resolution; [2] combo == popup == button ==
  seq-number font height 14pt.
- **`tests/seq_stepper_test.cpp`**: 17pt pins → 14pt (baseline constant renamed
  `kControlFontHeight`, float-safe compare).

## 4. Results (repo root; all after full `cmake --build build -j8`, 0 errors)

| suite | result |
|---|---|
| parvati_ui_typography_test | ALL CHECKS PASSED (0 failures) |
| parvati_seq_stepper_test | ALL CHECKS PASSED (0 failures) |
| parvati_layout_minwidth_test | PASS (0 failures) |
| parvati_multigui_test | ALL CHECKS PASSED (0 failures) |
| parvati_ipad_hig_sizing_test | ALL CHECKS PASSED (0 failures) |
| parvati_keyboard_view_test | PASS (0 failures) |
| parvati_lifecycle_test | ALL CHECKS PASSED (0 failures) |
| parvati_ui_mirror_test | ALL CHECKS PASSED (0 failures) |

## Files (this lane only)
`Source/ui/ParvatiLookAndFeel.cpp`, `Source/ui/ParvatiTheme.h`,
`Source/ui/SeqLengthStepper.cpp`, `tests/seq_stepper_test.cpp`,
`tests/ui_typography_test.cpp` (new), `CMakeLists.txt` (one appended block).
Untouched as contracted: PluginEditor.*, NoteStepControl (no font there to
fix), CHANGELOG, docs. Nothing staged (sibling lanes' in-flight files present
in-tree, untouched).

## Residual risks
- 14pt popup text in 44pt rows is sparser than before (HIG row height
  unchanged); readable and consistent — flagged in case the dense look is
  preferred on iPad.
- Group titles now share the tier with knob readouts; hierarchy rests on
  bold + all-caps + placement rather than colour (already the case).
- Top-menu buttons / patch indicator / mod-matrix widgets are the sibling
  lanes' scope; no overlap with these files.

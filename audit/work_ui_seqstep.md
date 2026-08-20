# SeqLengthStepper visibility — fix report

## Root cause
NOT colour-before-theme-attach, NOT zero-height. **Occlusion**: `resized()` gives `tapBtn_` the full cell (72x64 → 68x42 band) and the button was created *after* `numberLabel_`, so it painted **over** the number. `ParvatiLookAndFeel::drawButtonBackground` fills `TextButton::buttonColourId` = `backgroundPanel` (solid rounded rect, no early-out for empty text) — on Carbon `0xff1E2228` vs page `0xff15171C` is a near-invisible seam, so the user saw an empty cell. (Introduced by the F-ios-touch-2 stepper redesign: `setBounds({})` "logic-only" comment is contradicted by `resized()`.)

## Fix (3 independent defences, each test-pinned)
1. Add-order: `tapBtn_` added **before** `numberLabel_` (later sibling paints above).
2. `numberLabel_->setAlwaysOnTop(true)` (doesn't intercept clicks, so taps still hit the button).
3. `tapBtn_` fill colours (`buttonColourId`/`buttonOnColourId`) set fully transparent — can never occlude regardless of stacking.

Plus value-tier colour: the number is a VALUE readout but inherited the L&F's `Label::textColourId` = `textSecondary` (dim caption tier). Now `applyNumberLabelStyle()` resolves the active theme via `ParvatiLookAndFeel::getTheme()` and sets `textPrimary` — the same token as the knob centre readout (`Slider::textBoxTextColourId`) — re-resolved via new `lookAndFeelChanged()`/`parentHierarchyChanged()` overrides (the exact ParamControl pattern; base calls preserved). Font unchanged: 17pt bold (≥ app readout baseline 14–15).

## Per-theme contrast (WCAG 2.x, textPrimary vs both surfaces)
| Theme | vs backgroundPanel | vs backgroundBase |
|---|---|---|
| Carbon | 14.82:1 | 16.63:1 |
| Midnight | 13.95:1 | 15.83:1 |
| Obsidian | 14.54:1 | 15.95:1 |
| Paper (light) | 15.81:1 | 17.59:1 |
| Crimson | 14.78:1 | 16.50:1 |
| Legacy (light) | 12.69:1 | 14.20:1 |

All ≥ 4.5:1 with 2.8x+ margin.

## Test
New `tests/seq_stepper_test.cpp` → `parvati_seq_stepper_test` (CMake appended at end). 43 checks: [1] occlusion pins (transparent fills, always-on-top, add-order, ≥44x20 label band, ≥44pt hit target); [2] per-theme colour==textPrimary + contrast + font; [3] value text via real slider backing (set 7→"7", up→"8", down→"7", clamp 99→"16"/0→"1"); [4] live theme switch re-resolution (Carbon→Paper→Obsidian). **43/43 PASS, 3x repeat-stable.**

Consumers: `parvati_layout_minwidth_test` PASS, `parvati_ipad_hig_sizing_test` PASS, `parvati_multigui_test` ALL PASS (from repo root; the 5 failures from `build/` are the known CWD-relative `presets/FACTORY` artifact, identical pre-change), `parvati_lifecycle_test` PASS, `parvati_layout_overlap_test` PASS, `parvati_editor_test` PASS. Full build clean (the one grepped "3 errors" line-count was `-Werror` flag echoes, not diagnostics).

## Files (this lane only)
`Source/ui/SeqLengthStepper.{h,cpp}`, `tests/seq_stepper_test.cpp` (new), `CMakeLists.txt` (appended block only — other lanes edit elsewhere). Untouched as contracted: PluginEditor.*, ParvatiTheme.*, ParvatiLookAndFeel.*, KeyboardView.*, CHANGELOG, docs.

## Residual risks
- `setAlwaysOnTop` also fronts the label for mouse-event order; it is click-transparent, so no interaction change (pinned indirectly via lifecycle test [4] which drives the stepper).
- The number renders on the page fill, not a card; if a future theme makes `textPrimary` low-contrast vs `backgroundBase`, the test fails loudly (contrast asserted on both surfaces).

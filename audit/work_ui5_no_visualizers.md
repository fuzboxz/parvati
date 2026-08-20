# FX-slot graphic visualizers — removal report

## What was removed
- **`Source/ui/FxSlotVisualizer.{h,cpp}` deleted** (737 + 136 lines). It was
  the ONLY FX-module graphic illustration: a per-slot canvas (dimmed grid +
  per-algorithm drawing) hosted inside each FxSlotCard, fed by normalized
  APVTS getters (type/param1..5/drywet). Grep confirmed no other FX-page
  graphic exists (FxRoutingBar/FxMatrixView draw controls only); the SYNTH
  previews (OscPreviewDisplay/EnvelopeDisplay/FilterResponseDisplay) are
  separate classes and untouched.
- `FxSlotCard`: include, the construction block, the `visualizer_` member +
  forward decl, the bypass-alpha array entry, the resized() band block
  (nudge 4px + band ≤30px + gap), applyThemeColors' re-tint, `kVisMax`, and
  the doc-comment sections.
- `PluginEditor.h`: one stale comment reference.
- `tools/editor_test.cpp` [13]: the `hasVisualizer` assertions + include
  (combo/button/knob-count checks kept intact).

## Layout re-flow (no hole)
- The knob grid now owns the card body and centres vertically in the freed
  height (layoutParamGrid unchanged — it always centred; only the band above
  is gone).
- `FxWorkspace::kTopRowNaturalH` **264 → 240** (the band's 24px: 4px nudge +
  ≤30px band + 2px gap, floored by the grid's real need: 2×70 grid + 16
  header + 44 type row + 4 gaps + 12 pad + 16 host margins = 232; 240 keeps
  breathing). Card height at the default window: **248 → 224**; the grid
  region grows 136 → 148 and the fixed 2×70px cells now fit unsqueezed
  (previously the band consumed part of the body and cells shaved ~2px).
  44pt knob hit areas unchanged. The freed 24px flows to the workspace rows
  below the FX top row.

## Tests
- `tools/editor_test.cpp` [13]: visualizer assertions removed (comment marks
  the removal); all other card assertions kept and green.
- No tests/ file referenced the visualizer.

## Battery (repo root, all green)
multigui, editor_test, layout_overlap, layout_minwidth, ipad_hig_sizing,
workspace_padding, mod_matrix_ui, lifecycle, keyboard_view. Full
`cmake --build build -j8`: 0 errors (after a reconfigure dropped a stale
`parvati_tmp_drywet_probe` target left by the concurrent dry/wet session).

## Commit note (important)
The CODE changes above were swept into the sibling dry/wet session's commit
**`fd5dc78`** ("fix(dsp): parallel-topology dry/wet...") while both sessions
edited the same tree — its diff contains the visualizer deletion + all card/
workspace/test edits. This task's own commit adds the CHANGELOG entry only
(refuses to rewrite the sibling's commit). `fd5dc78` is NOT pushed; the
parent should mention the visualizer removal when it writes that push/merge
note.

# Batch-6 lane — patch-table fixed centred widths + zoom controls in Settings

Commit `82cb9b7` (NOT pushed). Files: `Source/ui/PatchPage.{h,cpp}`,
`Source/ui/SettingsPanel.{h,cpp}`, `Source/PluginEditor.{h,cpp}` (zoom-button
removal + overflow-menu wiring only), `Source/ui/Translations.cpp` (one key),
`tests/editor_test.cpp` (patch-table sections), `CHANGELOG.md`.

## 1. Patch table: fixed reasonable widths + centre alignment

- **Column maximums** (kColumnSpecs, was unbounded `1<<30` for combos):
  Voices 90 · Channel 110 · Octave 90 · Legato 90 · Tune 170 · Polyphony 150
  (name stays 160; knob columns keep their 72–88 caps). Minimums unchanged —
  the 1024-floor layout is byte-identical.
- **Centring**: new `tableContentWidth(mask)` = Σ maxes + gaps (per ACTIVE
  tab: Voice 1004pt, MIDI 520pt) and `centredTableBand(band, mask)` =
  `min(band, content)` wide, horizontally centred. Consumers: `PartRow::resized`
  (+ its `lastColumnRects_` hook), `ColumnHeader::paint` AND `columnXForTest`
  (so the alignment pin measures the real geometry), and
  `PartTablePanel::resized` which now gives the summary row + header + rows
  the same centred band (rows/header also centre internally — idempotent).
  At 1280x634 the Voice tab centres with a ~102px offset; at/below the floor
  the band IS the content (no shrink).
- **Test pins** (editor_test [23]): at 1600x900 — band > content,
  content <= Σ maxes + gaps (hardcoded 1004 pin so spec drift fails loudly),
  centring offset >= 0; the tab-strip x pin recomputed as
  `inset + centre-offset + 220 + 12` (±4). Existing 0px header↔cell alignment
  pins pass unchanged on both tabs.

## 2. Zoom buttons -> Settings

- SettingsPanel: `zoomSlider_` replaced by `zoomOutBt_/zoomInBt_/zoomResetBt_`
  (44pt square) + `zoomValueLabel_` (percentage readout). Steps ±0.1 clamped
  [0.75, 2.0], snapped to the historical 0.05 grid; persistence via
  `proc_.setUiZoom` + `onZoomChanged_` (editor applies the global scale) — the
  same contract the slider had. `setZoomValue` now only refreshes the readout
  (buttons have no value-change callback; `suppressCallback_` removed).
- PluginEditor: the three legacy zoom buttons (already invisible/unplaced
  since W9) deleted — members, ctor wiring, applyChromeTranslations tooltips.
  The `zoomOverflowButton_` ("...") **stays**: it is the W9 folded-actions
  host, not a zoom control (deviation from the brief's "remove if it only
  hosts those three" — it doesn't). Its three zoom menu items were removed;
  tooltip re-keyed "Zoom"→"More" (FR "Plus" / DE "Mehr" added;
  check_translations back at the 3 pre-existing violations). Header layout
  unchanged otherwise (the buttons occupied no space); Cmd/Ctrl +/-/0
  shortcuts untouched (`keyPressed(Cmd+0)` test still green).

## Validation (repo root)
multigui (0 failures incl. new [23](c) pins), layout_overlap, layout_minwidth,
ipad_hig_sizing, lifecycle, ui_mirror, patch_arrangement, translations — all
PASS. `cmake --build build -j8` 0 errors.

## Residual risks
- The Voice-tab content max (1004pt) means editors ~1100–1250pt wide still
  near-fill the table; centring reads most strongly ≥1280pt (as designed).
- The MIDI tab centres with a large offset at default width (520pt content in
  a ~1208pt band) — per the brief, but visually noticeable; easy to revisit.
- `zoomValueLabel_` readout only refreshes on button clicks and
  `setZoomValue` (shortcut-driven changes reach it through the editor's
  existing mirror call — verified wired at construction).

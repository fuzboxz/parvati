# ModMatrixView interaction redesign — work report (UI feedback 2026-08-20)

## 1. Drag-source removal (rows are not drag sources)
- The row drag source lived **entirely in `ModMatrixView.cpp`**: the file-local
  `ModSourceDragGrip` (six-dot handle, `startDragging("parvatiModSrc:<enum>")`,
  tap-to-assign on clean tap, themed drag image). **Deleted whole** — member,
  creation, listeners, layout slot. The row's source COMBO never had drag
  behaviour (plain `juce::ComboBox`), so nothing else to opt out.
- **No PluginEditor seam needed**: `ParamControl` (PluginEditor.cpp) is the
  DROP-TARGET side (`isInterestedInDragSource`/`itemDropped` →
  `ModMatrixHighlight::requestAssign`) — untouched and pinned working by the
  test ([1]: `requestAssign` assigns source+dest+depth through the APVTS).
  CentralModBar pills (the sanctioned source, CentralModBar.cpp:308) untouched.
- `PluginEditor.h` include dropped from ModMatrixView.cpp (grip was its only
  consumer).
- **For the gate**: two OTHER `parvatiModSrc` drag sources remain, deliberately
  out of this task's scope: (a) `FxMatrixView`'s `FxSourceDragGrip` (FX matrix
  rows — task said keep FxMatrixView untouched); (b) `GroupPager`'s
  `DraggableTabButton` (generator sub-tabs E1/L1… can be dragged onto knobs,
  GroupPager.cpp:12). If "pills only" is wanted everywhere, those are
  follow-ups.
- Comment refs to the deleted class updated in WheelsComponent.cpp /
  GroupPager.cpp (comments only, no behaviour).

## 2. Clear → X (far right)
- `IconButton` gained `Icon::Close` (path-drawn X: two rounded-cap strokes;
  themed text/accent like the other glyphs) + `setGlyphInset(float)` so a large
  hit area can render a compact glyph (PowerToggle's pinning idiom as an inset;
  default 4 keeps Undo/Redo/Gear pixel-identical).
- Row's `clearButton_` is now `IconButton(Icon::Close)`, **rightmost control**
  (44pt HIG hit target, ~20px glyph at inset 11), same action as Clear
  (`owner_.clearSlot(slot_)` → amount 0, mute dropped). Accessible title
  `TRANS("Delete modulation")` + tooltip; **FR/DE entries added** to
  Translations.cpp ("Supprimer la modulation" / "Modulation löschen") — no new
  translation-check violations (the 3 current ones pre-exist from sibling-lane
  in-flight edits; verified identical on a stashed tree).

## 3. Mute/bypass relocated + restyled
- New file-local `MuteLamp` (ModMatrixView.cpp) — **the FxSlotCard PowerToggle
  widget style**: compact bordered indicator dot, full-bounds hit area, accent
  fill while ON, `textDisabled` grey while OFF, outline ring brightening on
  hover. Accent = `accentPrimary` (mod-matrix family; FX uses accentSecondary).
  Semantics = module-disable parity: **accent while the routing is ACTIVE,
  grey while MUTED** (row drives `setToggleState(!muted)`).
- Position: **far LEFT of the row** (left of index + both combos; the freed
  drag-grip slot). Same seam as before: click → `toggleMute(slot)` — stash
  amount → write 0 (true engine bypass) → restore on re-click; editor-only,
  never persisted (dtor restore contract unchanged).
- Old "M" TextButton + "Clear" TextButton removed; the combos gained the grip's
  freed 44px.

## 4. Tests — `tests/mod_matrix_ui_test.cpp` → `parvati_mod_matrix_ui_test` (CMake appended at end)
[1] No drag sources: contract hook `canStartDragFromRowForTest()` (false) +
behavioural sweep (synthetic down/drag-60px/up on **every child of every active
row** — 50 components — inside a recording `DragAndDropContainer` host → **0
drag operations started**); drop-assignment bus still assigns (pill-drop path).
[2] X: exactly one IconButton per row, rightmost child, ≥44pt, titled; click →
amount 0, mute dropped, row inactive. [3] Lamp: leftmost button, LEFT of both
combos, ≥44pt; ON while active; click mutes (engine 0, stash kept, row visible)
/ restores. [4] Layout sanity across all 5 active rows (init patch carries
routings). **40/40 PASS.**

## Validation
`parvati_mod_matrix_ui_test` rc=0 (40 checks); consumers green from repo root:
multigui, modbar_click, ipad_hig_sizing (kRowHeight/kAddButtonH static-asserts
intact), editor, lifecycle, translations (key parity incl. the new key),
mod_audio, ui_typography (sibling lane's). Full `Parvati` lib rebuilt clean
with all in-flight lane sources.

## Residual risks
- The behavioural no-drag sweep relies on JUCE requiring a real dragging
  MouseInputSource inside `startDragging` (headless synthetic drags can never
  fire) — the contract hook + source-level removal are the authoritative pins.
- GroupPager tab-drag + FX-matrix grip remain drag sources (out of scope, see
  §1).

# Batch-4 lane A — patch-table alignment + tab placement + MIDI regrouping

Commit `e2e6be4` (not pushed). Files: `Source/ui/PatchPage.{h,cpp}`, `tests/editor_test.cpp`, `CHANGELOG.md`. No Translations change needed (no new labels; caption set reuses existing keys).

## 1. Header/controls misalignment — root cause + fix

`PartRow::resized()` computed columns from `getLocalBounds().reduced(4)` (the row's inset band) while `ColumnHeader::paint()` computed captions from `getLocalBounds()` (no inset). Both are children of `PartTablePanel` laid out at the same width, so every caption sat exactly **4px left of its column**. Fix: shared constant `kTableContentInset = 4` applied at BOTH call sites (`PatchPage.cpp:433` row, `:1101` header). New test hooks compute the header's painted x (`ColumnHeader::columnXForTest`, from the exact paint geometry) and row 0's cell x (`PartRow::columnXForTest`, from the rects stored at resize time); editor_test [23] asserts per-column equality — **0px drift, both tabs, ≥8 columns checked per tab**.

## 2. Tab strip moved left

`PartTablePanel::resized()` summary row reordered: `[Voice|MIDI] strip (≤150pt) → 12pt gap → Arrangement combo (220pt) → Voices Y/96`, replacing the old right-aligned strip. Pinned: `tabStripXForTest() <= 8` (leftmost control).

## 3. MIDI-column regrouping (decision documented)

Split regrouped per Ambika note-path semantics:
- **MIDI tab** = Part, Ch, Zone Lo/Hi, **Octave** (transpose acts on the note stream), **Polyphony** (Mono/Poly/Unison/Cyclic/Chain = the note→voice allocator) — 6 captions.
- **Voice tab** = Part, Voices, Porta, Lgo, Vol, Fine, Spr, Tune — 8 captions.

Reasoning: the principle is note-handling/routing on MIDI, sound-shaping on Voice. Portamento and Legato straddle the line (they shape note *transitions*, not timbre) — kept on Voice per the task brief, where the sound character lives. `applyTableTab` visibility updated (Ch/Zone/Oct/Poly ↔ Voices/Porta/Lgo/Vol/Fine/Spr/Tune); `captions(midi)` follows the masks exactly. Hidden cells stay constructed: [23] pins a Voice-hidden Octave write landing in engine byte 1 and a MIDI-hidden Porta write persisting across the tab round-trip.

## 4. Tests + battery

editor_test [22] caption count 12→8 (Voice) + order pin; [23] rewritten: mask truth-tables for both tabs, alignment pins on both tabs, leftmost-strip pin, hidden-cell seams both directions. **All green from repo root**: multigui (0 failures), ui_mirror, layout_overlap, layout_minwidth, patch_arrangement, multitimbral, partstate, translations. check_translations unchanged (3 pre-existing).

## Residual risks

- The alignment pin uses row 0 only; all rows share `partColumnRects` and identical widths, so row N cannot differ (the panel lays them out at the same width).
- `tabStripXForTest() <= 8` assumes the strip starts inside the panel's 4px-inset band with ≤4px slack — the bound is the panel inset (4) doubled for safety; a future redesign that adds leading padding would need the pin updated.
- MIDI tab at the 1024 floor: 6 columns × minimums (120+56+44+44+48+96=408 + 5×4 gaps) leaves ~536pt of flex — no narrow-band fallback engagement (verified by layout_overlap at 800px width too).

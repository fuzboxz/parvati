# Patch-Page Table Absorption — Completing (Vol / Fine / Spr)

User: "ideally everything would go into the table except for global." The
Patch page now renders exactly [Global panel: vca_curve / filter_card /
filter_drive + arrangement + the 6-part table]. The compact "Part / Play"
row is gone; ALL EIGHT part knobs are table columns.

## 1. Width math (MEASURED by probe at the 1024x500 floor, not assumed)

Chain: editor content 992 → vertical scrollbar 8 → hosted page/ScrollBody
984 → Global group panel x=16 **w=952** → table panel 952 → row (4px insets)
**w=944**.

| Element | Width | Row-local x-range |
|---|---|---|
| Name | 156 (+6 gap) | 4–160 |
| Voices | 76 | 166–242 |
| Ch | 68 (+4 gap) | 242–314 |
| ZoneLo / ZoneHi knobs | 48 + 48 (+8 gap before) | 314–410 |
| Oct / Porta / Lgo | 48×3 (4pt gaps) | 418–570 |
| **Vol** (new) | **36** (+2 gap) | **574–610** |
| **Fine** (new) | **36** (+2 gap) | **612–648** |
| **Spr** (new) | **36** (+2 gap) | **650–686** |
| Tune | 110 (gap 8→**4**) | 690–800 |
| Poly | jmin(140, rem)=136 (gap 8→**4**) | 804–940 |

**Row content ends at 940 ≤ 944** with the symmetric 4px inset intact;
row = 944 ≤ the 948 target. Pre-change content ended at 836 (slack 108, not
the briefed ~130): even 33pt cells would not have fit without reclaiming
chrome. The two 8pt gaps before Tune/Poly were tightened to the 4pt idiom
the Oct/Porta/Lgo trio already uses — **no cell was shrunk** (documented
deviation from "new gaps only", forced by the measured numbers).

## 2. Knob size / HIG decision

**36pt dials in 36×44 bands** (the L&F squares via jmin(w,h)): full
44pt-tall tap bands, 36pt wide. Three 44pt-wide cells (132pt + gaps) are
arithmetically impossible at the 108pt slack, and overlapping bounds would
steal neighbour hits — the only compliant reading of the fallback clause.
Table knobs are not pinned by ipad_hig_sizing_test (verified: it pins named
constants only). Readouts match SynthParamLabels exactly: Vol "% of 127",
Fine "±ct via x·100/128" (byte 2 SIGNED), Spr "% of 40".

## 3. Implementation

- Cells: PartRow volSlider_/fineSlider_/sprSlider_ (setupKnob idiom),
  engine-direct writes via the existing writeCharacterByte (bytes 0/2/3,
  byte 2 as int8_t), refresh() readbacks, TRANS captions Vol/Fine/Spr
  (FR: Vol/Fin/Spr, DE: Vol/Fein/Spr); the dead "Part / Play" FR/DE strings
  removed.
- PluginEditor: part_volume/tuning/spread added to the generation skip list
  (parameters stay fully valid APVTS/host-automation params — only the page
  knob is not generated); the dead groupForId "Part / Play" branch removed.
- Mirror: SynthEngine::applyPartByte isMirrorOffset extended to 0/2/3 —
  host automation / NRPN / undo of all three now bumps the display version
  and pollPatchPageMirror re-reads the rows.

## 4. Tests + results

- tests/editor_test.cpp [21]: Global page = exactly the 3 global knobs + no
  part_* anywhere + no "Part / Play" group; byte-drive checks extended to
  Vol(96) / Fine(±64, −127 signed) / Spr(40) incl. APVTS re-sync.
- tests/ui_mirror_test.cpp: mirrorMatches now checks Vol/Fine/Spr vs engine
  bytes for all 6 parts; the automation block writes part_volume/tuning/
  spread through APVTS + pollPatchPageMirror.
- tools/editor_test.cpp: coverage skips the 3 new ids; Global identity 6→3.

Battery (repo root): multigui (+ `--windowed`), ui_mirror, layout_minwidth,
layout_overlap, ipad_hig_sizing, apvts, host_param_text, editor,
translations, patch_arrangement, multitimbral, partstate — **all PASS**.
check_translations: the 3 pre-existing violations only (identical on HEAD).

## 5. Residual risks

- 36pt knob width < the 44pt HIG ideal (see §2; documented arithmetic).
- Engine-direct table writes bypass APVTS undo (same documented gap as the
  Poly/Tune/Oct/Porta/Lgo columns — unchanged class).
- sectionForId's `part` → Section::Global branch is now unreachable (kept,
  harmless, no test depends on it).

## 6. Commit

`refactor(ui): complete Patch-page table absorption — Volume/Tuning/Spread join the part table`
(10 files, +361/−87; NOT pushed). Release Standalone rebuilt (05:xx build,
carries the change).

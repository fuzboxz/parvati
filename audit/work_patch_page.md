# PATCH-PAGE SIMPLIFICATION — work report (2026-08-20)

Brief: `audit/patch_page_design.md`. All steps complete; per-step outcomes below.

## Step 1 — Part/Play placement: OPTION (2) deviation (approved)
The brief moved volume/tuning/spread to the **Mixer page**. Measured on fresh
editors (1280x634): top-row viewport budget **293px**; OSC and Filter pages sit
exactly at 293 (stretch-filled); baseline Mixer 252 (fits). With the extra
panel the Mixer page = **348 → +55px overflow → a NEW top-row scrollbar** at the
default size (compact cellH-56 variant: 348; 4th sectioned row inside the
Mixer panel: 314, still +21). Slack is 41px; no 3-knob panel fits (~104px
minimum). Escalated; supervisor approved **option (2)**: volume/tuning/spread
remain on the **Patch-hosted Global page** as one compact "Part / Play" row
above the part table — the Patch page already scrolls (T4), so **zero new
scrollbars anywhere**. sectionForId keeps `part*` → Global; the brief's Mixer
branch in configureGroupLayouts was reverted. Measured after revert: Mixer 252
(no part_* knobs), top-row overflow 0.

## Step 2 — Table columns
`PartRow` gained **Oct** (5-item "-2..+2" combo, id = value+3, byte 1 SIGNED
int8), **Porta** (44pt NoTextBox knob, 0..63, %-centre readout, Zone-knob
idiom, byte 6), **Lgo** (On/Off combo, byte 5). Writes via
`writeCharacterByte()` = the Poly/Tune pattern (setCurrentPart +
applyPartByte + restore + postPartEdit + `loadPartIntoApvts(currentPart)`).
Captions TRANS("Oct"/"Porta"/"Lgo") + FR/DE table entries (+On/Off;
DE Aus/Ein). check_translations: 3 violations, identical to HEAD's pre-existing
set. New HIG-correct taps: combos in 44pt bands (`parvatiComboVisualH` 24).

## Step 3 — Descriptor/parameter decision
**Parameters KEPT, knobs removed via page-generation filter** — descriptors
drive APVTS creation (`createParvatiParameterLayout` iterates the same table),
so deleting rows would delete host automatable parameters. The mechanically
safe mechanism: skip the five ids (`part_octave/part_legato/part_portamento/
part_raga/part_polyphony`) in PluginEditor's bucket loop (alongside the
existing part_select skip). Verified consumers unaffected: apvts_test /
host_param_text_test / host_state_test / presets / state blob (grep: all use
the APVTS/byte paths, none the page knobs). `hostGroupForId` untouched.

## Step 4 — Mirror
`SynthEngine::applyPartByte`: display-version bump extended to offsets
{15, 4, 1, 5, 6} via `isMirrorOffset()` (change-only, so the 30 Hz poll stays
quiet). `PartRow::refresh()` reads bytes 1 (int8-cast)/5/6 back into the
cells. ui_mirror_test: mirrorMatches now checks all three columns for all 6
parts; battery [1] adds APVTS writes (oct −2 signed, porta 52, legato on) +
byte pins; [2] adds engine-direct writes on part 4.

## Step 5 — Tests
editor_test **[21]** (16 checks): Global page = 6 controls incl. the compact
row; NO page generates the absorbed knobs; Mixer carries no part_*; Oct/Lgo/
Porta drive bytes 1/5/6 (+signed −2 case); APVTS re-sync pin; layoutIsSane on
the slimmed Global page. tools/editor_test: coverage-count skip + Global-page
identity updated (11 → 6; part_raga → part_volume). Column math:
156+6+76+68+4+48+48+8+48+4+48+4+48+8+110+8+140 = **824pt** vs ~952pt budget
at the 1024 floor (~130pt slack; the 3×52pt volume/tuning/spread absorption
rejected — 980 > 952).

## Step 6 — Battery (repo root)
multigui (incl. **--windowed**), ui_mirror, layout_minwidth, layout_overlap,
ipad_hig_sizing, apvts, host_param_text, editor, translations,
patch_arrangement, multitimbral, partstate — **all green, 0 failures**.

## Step 7 — Ship
CHANGELOG entry (with the option-2 deviation); ONE commit; Release Standalone
rebuilt. Residual risks: engine-direct table writes bypass APVTS undo
(documented; same as Poly/Tune); volume/tuning/spread remain page knobs (user
asked to "try" Mixer; measured infeasible without a new scrollbar).

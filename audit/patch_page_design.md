# Patch-Page Simplification — Design Brief (read-only scout)

## 1. INVENTORY (what the Patch page shows today)

The page = `PatchPage` (custom component) hosting the editor-owned `Section::Global` ParamPage, rendering **[Part / Play panel] → [Global panel: 3 knobs + external 6-part table]** (`PatchPage.cpp:839-858`, `PluginEditor.cpp:2739-2740,2952-2958`).

**(a) Genuinely managerial (stays):** arrangement combo + "Voices Y/96" summary row (`PatchPage.cpp:560-580`); per-row: editable part name, Voices 0-16, MIDI Ch (Omni+1-16), Zone Lo/Hi knobs, Tune (raga, byte 4), Poly (byte 15) — `PatchPage.cpp:181-236,443-485`. All engine-direct, no APVTS.

**(b) Per-part synth knobs on the hosted "Part / Play" panel** (APVTS, current-part; descriptors `ParameterLayout.cpp:421-429`; group `PluginEditor.cpp:1594`): `part_volume` "Volume" (byte 0), `part_octave` "Octave" (-2..2, byte 1), `part_tuning` "Tuning" (±127→±99ct, byte 2), `part_spread` "Spread" (0..40, byte 3), `part_raga` "Scale" (byte 4 — **duplicate of the row Tune combo**), `part_legato` "Legato" (byte 5), `part_portamento` "Portamento" (0..63, byte 6), `part_polyphony` "Polyphony" (byte 15 — **duplicate of the row Poly combo**).

**(c) Global engine options** ("Global" group, `PluginEditor.cpp:1538-1539`; descriptors `ParameterLayout.cpp:437-447,482-517`): `vca_curve` "VCA Curve", `filter_card` "Filter Card", `filter_drive` "Filter Drive". Patch-wide, ride `.parvati` only — belong on the Patch page.

## 2. PROPOSAL

**(c) stays** on the Patch page inside the existing "Global" group (the table already rides its external-decoration slot, `PatchPage.cpp:857`). Optional retitle to "Engine" is display-only via `TRANS` — **keep the English key "Global"** (stable identity for `setGroupExternalDecoration` matching, `PluginEditor.cpp:2232-2234`).

**(b) split — table-first:**
- **Delete the two duplicate knobs** (`part_raga`, `part_polyphony`): the row combos are the single UI. Mechanism: skip those ids in the bucket loop like `part_select` (`PluginEditor.cpp:2410-2411`). APVTS params remain (host automation, saves, `loadPartIntoApvts` `PluginProcessor.cpp:1052-1064`).
- **Table absorbs** `part_octave`, `part_legato`, `part_portamento` as three compact per-row columns (48pt knob / 64pt 2-item combo / 48pt knob). Column budget at the 1024pt floor: usable row width ≈ 992 (viewport) − 14 (scrollbar, T4 asserts overflow at 1024×500, `editor_test.cpp:266-303`) − 16 (page margin) − ~18 (group pad) − 8 (inset) ≈ **936pt**. Current consumption = 680 (`PatchPage.cpp:184-236`: 156+6+76+68+4+48+48+8+110+8+140 + 8 inset). Additions +178 (3 cols + gaps) → **858 ≤ 936**, ~78pt slack. ✓
- **Synth page gets** `part_volume`, `part_tuning`, `part_spread` (continuous sound-shaping knobs) — **and `part_portamento` too if the implementer prefers a knob over a row stepper** (user named it for the synth page; both work, pick one). Host: new **"Part / Play" group panel on the Mixer page** — `groupForId("part_*")` already returns "Part / Play" (`PluginEditor.cpp:1594`), so routing `sectionForId` for those ids → `Section::Mixer` drops the panel onto the existing Mixer ParamPage (main-left 20% column, `SynthWorkspace.cpp:64-71`, column split `SynthWorkspace.cpp:241-263`). Reuse the Mixer's narrow-cell layout (`cellW 60, 3 cols`, `PluginEditor.cpp:1694-1699`) — 3×60+16=196 fits the ~198pt column. **No new component types**: plain ParamControls in a plain GroupComponent.

End state: Patch page = Engine panel (3 knobs) + table (+3 columns); the "Part / Play" panel vanishes from the hosted page naturally (no descriptors left in `Section::Global`).

## 3. MIGRATION MECHANICS

- Moving a control = **edit `sectionForId`** (`PluginEditor.cpp:82-118`; `part_` prefix rule at :117). Pages are built from `sec[]` buckets (:2402-2418) and ParamControls are generated per-descriptor with their own APVTS attachments — nothing regenerates, byte-bridge untouched. Host grouping (`ParameterLayout.cpp:645-660`) is independent — unaffected.
- `part_select` interplay: knobs edit the current part via `parameterChanged → applyParameterToEngine`; the header combo (`PluginEditor.cpp:2575-2580`) still drives `onPartSelect → loadPartIntoApvts` (`PluginProcessor.cpp:983-996`), which refreshes the synth-page knobs. New row controls (octave/legato/portamento) must copy the **engine-direct + `loadPartIntoApvts` re-sync** pattern (`PatchPage.cpp:443-460` byte-15 idiom) so saves (which gather current-part bytes from the APVTS, `tuning_test.cpp:585-588`) stay correct.
- Tests asserting membership: `editor_test.cpp:191-245` [5] pins filter_card/vca_curve/filter_drive to the "Global"-group page — unchanged since (c) stays. `editor_test.cpp:486-494` (poly/tune row mirrors) unchanged. T4 overflow check (:266-303) may need re-tuning if removing the "Part / Play" panel changes body height — verify, don't assume.

## 4. RISKS + ORDERING

1. **Land order**: (1) remove dup knobs (skip-list, smallest blast radius) → (2) new table columns + `refresh()`/`refreshLanguage()`/test-hook extensions → (3) `sectionForId` moves to Mixer → (4) optional "Engine" retitle.
2. **HIG 44pt**: new row combos need the `parvatiComboVisualH 24` property + 44pt tap band idiom (`PatchPage.cpp:130-133,199-236`); knobs 44pt like the zone pair (:217-227); `setScrollWheelEnabled(false)` (:161-163) and the row's `setViewportIgnoreDragFlag(true)` (:77-80) so drags don't scroll the Viewport.
3. **30 Hz mirror** (`pollPatchPageMirror`, `PluginEditor.cpp:3360-3377,3481-3485`): new row controls must join `PartRow::refresh()` so out-of-band APVTS/NRPN writes surface; verify the engine's display version bumps for bytes 1/5/6 mutators.
4. **Arrangement inference** (`PatchArrangement.h:67-73`) reads engine state only — unaffected by UI moves.
5. Column total must stay ≤ ~900pt (scrollbar + localization slack); longest locale strings can blow the Tune/Poly columns — budget the 78pt slack as localization reserve.

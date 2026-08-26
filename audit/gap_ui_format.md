# Test-Coverage Gap Audit — UI helpers + formats

Repo: /Users/fuzboxz/parvati (read-only audit; no files changed). Sources under `Source/`, tests under `tests/`.

## Prioritized missing tests (max 6)

### 1. ParamHelp — descriptor↔help parity (NO test at all; test would FAIL today)
`Source/ui/ParamHelp.cpp` (121-entry curated map, lines 14–186) vs `Source/ParameterLayout.cpp` descriptor table (FX family added at lines 521–560).
- **Zero** tests reference ParamHelp. Confirmed by grep: no `getParamHelp/hasParamHelp` in tests/.
- **0 of the FX paramIDs have help**: `fx{1..3}_{type,enabled,drywet,param1..5}` (24), `fx_topo`, `fx_order`, `fx_mix`, `fx_eq_{low,mid,high}`, `fxmod{1..16}_{source,dest,amount}` (48) — ~79 IDs where tooltips silently show nothing.
- Suggested: new `tests/paramhelp_parity_test.cpp` — iterate `getPatchParamDescriptors()`, assert `hasParamHelp(d.paramID)` for every non-option ID; plus generated seq cases: `hasParamHelp("seqnote_vel15")==true`, `getParamHelp("seqnote_vel15")` contains "velocity", `hasParamHelp("seq1_stepX")==false` (non-numeric suffix), `hasParamHelp("bogus")==false`. Effort: **S** (test; M to author the missing FX strings).

### 2. Translations — FR/DE key parity + fallback (NO test at all)
`Source/ui/Translations.cpp` (FR table lines 24–175, DE table 181–331, `installLanguage` 341–370). Zero tests reference it.
- Missing/drifted key in one table silently falls back to English — nothing catches it (tables are hand-synced ~120 keys each).
- Cases: parse both tables via `juce::LocalisedStrings(text,true)` and assert identical key sets; `installLanguage("fr")` → `TRANS("Settings")=="Réglages"`, `TRANS("not a key")=="not a key"`; `installLanguage("zz")` → mappings null; `getAvailableLanguages()` order `{auto,en,fr,de}`. Effort: **S**. Suggested: `tests/translations_test.cpp`.

### 3. paramValueTextSynth — untested families (`Source/ui/SynthParamLabels.cpp`)
`tests/hellcat_synth_paramtext_test.cpp` (137 lines) covers osc/mix_balance/filter1_cutoff/env1_attack/synced rate idx 10/mod/seqnote_step/arp/part_tuning/part_raga. UNTESTED branches:
- `seqnote_vel*` (lines 158–163): `T("seqnote_vel3", 0x80|100)=="79%L"`, `T("seqnote_vel3",100)=="79%"` — the legato-bit decode has zero assertions.
- `filter_env`/`filter_lfo` (fall-through pct/63): `T("filter_env",63)=="100%"`.
- `part_spread`(÷40)/`part_portamento`(÷63)/`part_volume`(÷127): `T("part_spread",40)=="100%"`.
- `mix_crush` non-zero: `T("mix_crush",31)=="100%"`; `seq1_step5`: `T("seq1_step5",127)=="100%"`.
- env2_/env3_ prefix dispatch + synced/free boundary: `T("env3_lfo_rate",14)=="1/64T"`, `T("env3_lfo_rate",15)` contains "Hz".
Effort: **S** — extend existing test file.

### 4. ParameterLayout valueFromString — parse-from-string failures (`Source/ParameterLayout.cpp` 726–815)
`tests/host_param_text_test.cpp` [5] covers 9 happy paths. UNTESTED:
- `fx_eq_low` parse: `valueForText("fx_eq_low","off")==0`; typed displayed text `"1k5"` must not silently become 1 (documented non-invertibility).
- `fx_eq_high` (only mid tested): `valueForText("fx_eq_high","-12")==0`.
- `fx{N}_paramK` stays raw-int: `valueForText("fx1_param1","100")==100` (NOT percent-mapped).
- `fxmod1_amount` negative: `valueForText("fxmod1_amount","-100")==-63`; garbage `""`/`"abc"` → 0 clamped by range.
Effort: **S** — extend host_param_text_test [5].

### 5. NoteStepControl::sliderToByte/byteToSlider + readout (`Source/ui/NoteStepControl.cpp` 24, 58–70)
Public statics (header lines 53–55), zero direct tests (param_thread_test covers only thread deferral).
- Cases: `sliderToByte(0)==0`, `sliderToByte(1)==0x80`, `sliderToByte(128)==0x80|127`, `byteToSlider(0x80|60)==61`, **asymmetric decode pinned**: `byteToSlider(60)==0` (gate-off byte <128 → Rest — data-loss by design), `byteToSlider(0xFF)==128`; `textFromValue` readout `"Rest"`/`"C4"` at v=61. Effort: **S** (new tiny test or fold into parvati_synth_paramtext_test).

### 6. PresetBrowser — multi-bank factory ordering + nested user dirs in stepping (`Source/ui/PresetBrowser.h` 461–486 flattenLeaves/stepSelection)
editor_test [18] uses only bank A + a FLAT user dir. UNTESTED:
- Bank interleave A→B→F→S then Multi: with `FACTORY/{A,B,F,S}` each holding one .PRO, `selectNext()` order must be a,b,f,s,m1.MUL,u1,t1.
- Nested user tree order (subs before leaves): `USER/Sub/u0.parvati` + `USER/z.parvati` — flatten must visit Sub's leaf first.
Effort: **S** — extend editor_test [18a].

## Already covered (one-liners)
- **FxSlotLabels paramValueText/paramLabel/activeParamCount**: full sweep every type×idx×0..127 with ≤6-char budget + 26 exact anchors (fx_param_coverage_test #10, lines 1025–1100).
- **fxEqLowToString/fxEqDbToString host text**: Off/1k5/0dB/+6dB anchors via APVTS getText (host_param_text_test [4]).
- **Host grouping/order/automation flags**: host_param_text_test [1]–[3], [6].
- **ModDestMap**: full paramID↔dest round-trip, no-knob set, aggregation/slotsForDest, FX offset boundaries (mod_dest_map_test [1]–[4]).
- **ParvatiPreset format**: unknown keys ignored ([4]), corrupt-reject (T7), clamped hand-edits (T8), hostile names (T4), sparse multi (T3), options lockdown (T9), version-99/truncation corpus (loader_fuzz_test 537/655), golden + full round-trips (roundtrip_golden_test, roundtrip_test).
- **PatchFile .PRO/.MUL**: byte-exact reference rewrite (export_bytes_test), unicode name16 code-point truncation (mul_strategies_test [C]), garbage rejection (patch_test [1]).
- **MulExport**: strategy-matrix invariants incl. chain split + previewLines/summarize (mul_strategies_test, export_fallback_test 206–224).
- **PresetBrowser cache**: scan/parse once-per-generation, mtime self-invalidate, absent-root creation, ghost residual pinned (editor_test [17]); stepping wrap/anchor/empty-tree ([18]).
- **SynthParamLabels**: ~30 anchor cases across osc/mix/filter/env/LFO-rate/mod/seq-step/arp/part families + raw-int fallback.

## Residual notes
- `midiNoteName` (NoteName.h) out-of-range → `{}` never directly asserted (indirect via seqnote_step C4); fold into gap #5.
- ModSourceCatalog::entryFor/clustersInOrder has no dedicated test (assert-only out-of-range path; low risk).
- `envTimeToString` ∞ glyph unreachable via LUT (documented defensive).

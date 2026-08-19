# UI/Format Test-Gap Lane — Work Report

## Item 1 — ParamHelp parity (code + test landed together)
**Code:** 78 help strings added to `Source/ui/ParamHelp.cpp` — `fx{1..3}_{type,enabled,drywet,param1..5}` (24), `fx_topo/fx_order/fx_mix/fx_eq_{low,mid,high}` (6), `fxmod{1..16}_{source,dest,amount}` (48) — one-sentence English, existing style. Slot/fxmod families are **loop-generated inside buildHelpMap()** mirroring the descriptor table's `addFx` loops (cannot drift); the 6 chain/master entries are literal.
**Test** `tests/paramhelp_parity_test.cpp` → `parvati_paramhelp_parity_test` (**PASS**): every descriptor id (options + FX included — the documented all-184 contract) has help; 78/78 FX ids; generated seq cases (`seqnote_vel15` contains "velocity", `seq1_step7` shows 1-based "8"); `seq1_stepX`/`seqnote_vel`/`bogus`/`""` → empty; FX content spot-checks (topology names, unity byte, ±63 range). Result: **198 curated + 64 generated = 262/262 ids covered** (0/78 → 78/78 for FX).

## Item 2 — translations_test (PASS)
`tests/translations_test.cpp` → `parvati_translations_test`. Key parity read from `LocalisedStrings::getCurrentMappings()->getMappings()` after `installLanguage` (the *shipped* tables, not a copy — the table text is anonymous-namespace, so no Source access needed): FR/DE key sets identical (any drift reported by key name); language order `{auto,en,fr,de}`; `TRANS("Settings")`→"Réglages"/"Einstellungen"; unknown-key passthrough; `"en"` and unrecognised `"zz"` → null mappings + raw English. Identity restored at exit.

## Item 3 — synth_paramtext extensions (PASS)
New `testUntestedFamilies()`: `seqnote_vel3` 0x80|100→"79%L" / 100→"79%" / vel7 0x80|0→"0%L"; `filter_env`/`filter_lfo` 63→"100%", 0→"0%"; `mix_crush` 31→"100%", 16→"52%" (÷31); `seq1_step5` 127→"100%", `seq2_step0` 64→"50%"; `part_spread` 40 / `part_portamento` 63 / `part_volume` 127 → each "100%" (per-denominator rails); env2/env3 dispatch: `env3_lfo_rate` 14→"1/64T", 15 contains "Hz", `env2_lfo_rate` 0→"1/1", `env2_sustain` 127→"100%".

## Item 4 — host_param_text [5] extensions (PASS)
`fx_eq_low` "off"→0; **"1k5"→1 pinned as documented non-invertibility** (display of 127 parses to 1 — semantic Hz strings stay raw-int typed entry); `fx_eq_high` "-12"→0, "+12"→127 (both rails); `fx1_param1` "100"→100 (raw, NOT percent); `fxmod1_amount` "-100"→-63; garbage ""/"abc"/"??"→0 clamped.

## Item 5 — note_step_control_test (PASS)
`tests/note_step_control_test.cpp` → `parvati_note_step_control_test`. **Required a minimal visibility change**: `sliderToByte`/`byteToSlider` were `private` (the audit's "public statics" was wrong) — declarations moved to public in `Source/ui/NoteStepControl.h`, zero behavior change. Pins: 0→Rest, 1→0x80, 128→0xFF, 61→0xBC, negative→0, **129 wraps to note 0 (&0x7f mask, not clamp)**; asymmetric decode `byteToSlider(60)==0` (gate-off data-loss by design), 127→0, 128→1, 0xFF→128; bijective round-trip over all 128 gate-on bytes; `midiNoteName` -1/128→{} + C4/C-1/G9; **constructed-control readout** via a probe subclass exposing the protected slider's `textFromValueFunction`: "Rest"/"C4"/"C#4"/"G9" on a real processor.

## Item 6 — editor_test [18a] (PASS)
New (a2) block: banks A/B/F/S each one .PRO → step order exactly `a.PRO, b.PRO, f.PRO, s.PRO, m1.MUL, u0.parvati (Sub dir first), z.parvati, t1.parvati`, plus wrap Templates→bank A.

## Validation
All 6 targets + 4 adjacent consumers (editor, lifecycle, ipad_hig, param_thread) green. Tree carries sibling lanes' concurrent edits (AmbikaVoice.h, render_quality_test.cpp, etc.) — untouched by this lane.

## Residual risks
- `ParamHelp.h` header comment still says "120 curated entries / 184 parameters" — file not in this lane's ownership; now 198/262. One-line follow-up.
- Other lanes' parallel CMake/target edits coexist in the tree; final full-suite gate is the parent's.

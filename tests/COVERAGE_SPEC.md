# Parvati Test Coverage Specification — Intended vs Real Outcome

This document is the **authoritative reference** for what every parameter and
every module *should* do. The consolidated coverage binaries
(`parvati_synth_param_coverage_test`, `parvati_fx_param_coverage_test`) assert
each row's INTENDED outcome against the REAL outcome. A failure = a drift /
bug, to be fixed (if easy) or recorded in `COVERAGE_FINDINGS.md` (if a design
decision / action required).

## Conventions

- **Byte routing** (patch params): setting APVTS param `id` to value `v` and
  calling `syncAllParamsToEngine()` MUST write `parvatiValueToPatchByte(d, v)`
  into `engine.getPart(0).patchBytes[d.byteOffset]` (or `partBytes` for
  `isPart`). Signed params reinterpreted as `int8_t`.
- **Audio effect**: rendering a held note with the param at two different
  settings MUST produce measurably different audio (peak or RMS or spectral
  centroid differs), UNLESS the param is documented as inert (e.g. a routing
  field that needs a mod source to act).
- **Engine-state routing** (arp/seq/option/fx params): setting via APVTS MUST
  reach the engine setter (verified through a debug accessor or a downstream
  observable effect).

All tests render at 48000 Hz, block 512, Part 1, MIDI channel 1, note 60,
velocity 0.8 unless noted.

---

# SYNTH PARAMETERS (181 total)

## Oscillators (8 params; patch bytes 0..7)

| param | byte | intended | how verified |
|-------|------|----------|--------------|
| `osc1_shape` | 0 | wave selector; NONE(0)=silent, SAW(1)=audible | byte round-trip; audio: SAW peak>0.01, NONE peak << SAW |
| `osc1_param` | 1 | osc1 timbre/PWM-ish param 0..127 | byte; audio: param=0 vs 127 differs (for a shape that uses it, e.g. SQUARE) |
| `osc1_range` | 2 | octave range, signed -24..24 semitones | byte (signed); audio: range=0 vs +12 => pitch ~2x |
| `osc1_detune` | 3 | fine detune, signed -64..64 (1/128 st) | byte (signed); audio: detune=0 vs +64 => beat freq shifts |
| `osc2_shape` | 4..7 | as osc1 but osc2 | byte; audio (isolate osc2 by muting osc1 via shape=NONE) |

NOTE: osc1/osc2 audio tests must ISOLATE the oscillator under test (mute the
other by setting its shape=NONE) so the measurement reflects only that osc.

## Mixer (8 params; patch bytes 8..15)

| param | byte | intended | how verified |
|-------|------|----------|--------------|
| `mix_balance` | 8 | osc1/osc2 crossfade 0..63 (32=center) | byte; audio: balance=0 (full osc1) vs 63 (full osc2) swaps energy |
| `mix_op` | 9 | operator: SUM/SYNC/RING/XOR/FOLD/BITS (0..5) | byte; audio: SUM vs RING differ |
| `mix_param` | 10 | operator amount 0..63 | byte; audio: param=0 vs 63 differs (op=SYNC) |
| `mix_sub_shape` | 11 | sub-osc wave 0..10 | byte; audio: shape=0 vs 5 differs (sub level>0) |
| `mix_sub` | 12 | sub-osc level 0..63 | byte; audio: level=0 vs 63 differs |
| `mix_noise` | 13 | noise level 0..63 | byte; audio: level=0 vs 63 differs (broadband) |
| `mix_fuzz` | 14 | fuzz/distortion 0..63 | byte; audio: level=0 vs 63 adds harmonics (centroid up) |
| `mix_crush` | 15 | bit-crush 0..31 | byte; audio: crush=0 vs 31 adds aliasing |

## Filter (5 patch params; bytes 16..23)

| param | byte | intended | how verified |
|-------|------|----------|--------------|
| `filter1_cutoff` | 16 | filter cutoff 0..127 | byte; audio: cutoff=10 (dark) vs 127 (bright) — spectral centroid shifts down |
| `filter1_reso` | 17 | resonance 0..63 | byte; audio: reso=0 vs 63 adds peak near cutoff |
| `filter1_mode` | 18 | LP/BP/HP/Notch (0..3) | byte; audio: LP vs HP invert the low-band energy |
| `filter_env` | 22 | env->cutoff amount 0..63 | byte; audio: amount=0 vs 63 changes the attack-time cutoff sweep |
| `filter_lfo` | 23 | lfo->cutoff amount 0..63 | byte; audio: amount=0 vs 63 adds periodic cutoff motion |

NOTE: `filter_card` (option) + `filter_drive` (option) are in Options below.
`filter[1]` bytes 19..21 are intentionally NOT exposed (reserved slot).

## Envelopes (12 params; bytes 24,32,40 +4 stride)

`env{1,2,3}_attack/decay/sustain/release`. Each 0..127.
- byte routing for all 12.
- audio: a LONG attack (127) vs INSTANT (0) produces a clearly different
  amplitude envelope on a note's first ~50 ms (when env routes to VCA — env3
  does by default init patch). env1/env2 default route to osc params (no direct
  VCA), so test them via a mod-routing probe (route env->VCA, then vary attack).

## Env-LFO (9 params; bytes 28,36,44 shape/rate + 31,39,47 sync)

`env{1,2,3}_lfo_shape` (0..3 Triangle/Square/S&H/Ramp), `env{1,2,3}_lfo_rate`
(0..(kNumSyncedLfoRates+127)), `env{1,2,3}_lfo_sync` (0..2 Free/Slave/Master).
- byte routing for all 9.
- rate: synced rates (index < kNumSyncedLfoRates) are tempo-divisions; free
  rates are Hz. audio: route lfo->cutoff and vary rate -> modulation period
  shifts.
- sync mode: byte round-trip.

## Voice LFO (2 params; bytes 48..49)

`voice_lfo_shape` (0..3), `voice_lfo_rate` (0..127).
- byte routing; audio: route voice_lfo->cutoff, vary rate -> period shifts.

## Modulation matrix (42 params; bytes 50..91 stride 3)

`mod{1..14}_source/dest/amount`.
- byte routing for all 42 (source/dest as choice index, amount signed -63..63).
- functional: the VCA destination has a BASELINE = `part_volume<<1` (voice.cpp)
  and the VCA mod is MULTIPLICATIVE (not additive). So amount=0 never closes
  the VCA (the baseline keeps it open). The mod depth is observed by lowering
  part_volume for headroom and using CONST sources of very different value:
  CONST_256->VCA+63 is loud, CONST_4->VCA+63 is quiet; -63 inverts. An ENV
  source also opens the VCA at its sustain level. This proves the matrix routes.

## Modifiers (12 params; bytes 92..103 stride 3)

`modif{1..4}_in1/in2/op`.
- byte routing for all 12.
- functional: modifier produces a mod-source usable as a matrix source; op
  changes the result. (Lower priority — verify byte routing primarily.)

## Part params (7 params; PartData bytes)

| param | part byte | intended | how verified |
|-------|-----------|----------|--------------|
| `part_volume` | 0 | output level 0..127 | byte; audio: vol=127 vs 10 => peak ratio |
| `part_octave` | 1 | octave shift, signed -2..2 | byte (signed); audio: +1 octave => 2x freq |
| `part_tuning` | 2 | fine tune, signed -127..127 | byte (signed) |
| `part_spread` | 3 | per-voice detune spread 0..40 | byte; audio: 0 vs 40 => stereo/voice detune (RMS, may be subtle) |
| `part_legato` | 5 | legato on/off | byte; functional: monophonic glide behavior |
| `part_portamento` | 6 | glide time 0..63 | byte; audio: portamento on + 2 rapid notes => pitch glide |
| `part_polyphony` | 15 | Mono/Poly/Unison2x/Cyclic/Chain (0..4) | byte; functional: voice allocation differs |

## Voice counts & arrangement (engine-level, not patch bytes)

Voice slots are ENGINE state (`SynthEngine::voiceSlots`, 1..16 per Part from
the 96-voice pool), so they are covered functionally, not via the byte bridge:

- **Counts are the user model.** `setPartVoiceSlots` clamps 1..16 (the
  public setter can never disable); a Part is enabled iff its count >= 1.
  The Patch page's Voices combo offers a real "0" item (0 disables the Part,
  riding the legacy zero-mask path); 0-voice rows stay dimmed + interactive.
- **Cards are derived.** The 6-voicecard bitmask comes from
  `mul_export::deriveMasks` (contiguous proportional share, min 1 per active
  Part) — one source of truth shared with `.MUL` export. Assert the derived
  masks for representative count vectors (e.g. 10/8/6 -> 3/2/1 cards).
- **Five arrangement presets** — Mono (1 part, 1 voice, MONO poly = true
  mono), Poly (1 part, 16 voices, POLY), Unison (1 part, 16 voices, MONO +
  spread), Multitimbral (6 parts × 16 voices, MONO, channels 1..6), Drum Kit
  (6 parts × 1 voice, MONO, GM drum key zones). 0-voice parts are first-class
  (every preset disables its unused parts). Coverage: apply each, assert
  per-Part counts + zones + channels + polyphony, and that inference
  round-trips the preset (non-matching edits read Custom).
- **MONO/unison = the count** (MONO + N voices = N-voice unison);
  CHAIN doubles a Part's voice set (up to 32).
- **Round-trips:** `.parvati` carries `voice_slots` verbatim; `.MUL`/legacy
  host-state blobs materialize counts from the stored mask's popcount
  (0 -> disabled).
- **Editor-level load wiring** (`tests/editor_test.cpp` [7b]/[7c]/[7d]):
  the real user entry points — `filesDropped` → `applyPatchFile` → load →
  Patch-page refresh with NO manual refresh calls — for `.parvati` multis,
  `.PRO` programs (incl. the stale-custom-tuning clear) and corrupt files
  (validate-before-mutate), plus the stock `presets/TEMPLATES/*.parvati`
  through the real load path and the hidden-page reveal refresh.

## Sequencer (67 params; controller PartData)

`seq_length_{1,2,3}` (1..16), `seq1_step{0..15}`, `seq2_step{0..15}` (0..127),
`seqnote_step{0..15}` (0..255), `seqnote_vel{0..15}` (0..255).
- engine-state routing via `setSequenceLength`/`setSequenceDataByte`.
- functional: a 1-step note-seq with a note fires when arp_mode=Sequencer +
  transport playing (verify a note sounds). step data round-trips into the
  engine Sequencer's data array (probe via rendering or a debug read if
  available — otherwise verify the engine accepts all 256 byte values without
  crashing / OOB, which is the real contract for the 0..255 note/gate bytes).

## Arp (5 params; controller-side)

`arp_mode` (0 Off/1 Arp/2 Sequencer), `arp_direction` (0..5),
`arp_octave` (1..4), `arp_pattern` (0..21), `arp_resolution` (0..14).
- engine-state routing via engine arp setters.
- functional: arp_mode=Off => no generated notes; arp_mode=Arp + held chord +
  transport playing => multiple notes per beat. resolution change => note spacing.

## Options (4 params)

| param | intended | how verified |
|-------|----------|--------------|
| `vca_curve` | Linearized/Exponential (0/1) | engine state via setVcaExponential; audio: exp curve changes the VCA shape |
| `part_select` | 1..6 multitimbral part selector | functional: switch changes which Part bytes are edited |
| `filter_card` | LM13700/SSM2164/SVF (0..2) | engine state via setFilterTopology; audio: 3 topologies sound different at high reso |
| `filter_drive` | ladder saturation drive (choice 0..7) | engine state via setFilterDrive; audio: drive scales tanh saturation (high-reso ladder) |

---

# FX PARAMETERS + MODULES (78 params, 16 FX types)

## Per-effect module coverage (all 16 FxType values)

For each `FxType` (None, Diffuser, PitchShifter, Reverb, LoopingDelay,
WSOLAStretch, Spectral, Wavefolder, FrequencyShifter, RingModulator, Resonator,
ClockedDelay, Ensemble, PlateReverb, VinylCompressor, Phaser):
1. `createFxProcessor(t)` returns non-null (None returns null) and `type()`
   matches `t`.
2. `prepare(48000, 256)` + `reset()` do not crash; a 1024-sample stereo block
   renders FINITE (no NaN/Inf/subnormal) at 0.0, 0.5, 1.0 param settings.
3. With a steady sine input, the wet output DIFFERS from the dry input (the
   effect is audible), at full wet — EXCEPT None (passthrough) which is
   bit-identical (within 1e-9).
4. `latency()` reports: 0 for all except Wavefolder (8) and RingModulator (8);
   ClockedDelay/Phaser/etc. = 0.

## Per-effect 5-param sweep (the 15 non-None types)

For each effect, hold a steady sine and sweep each of the 5 slot params
independently over {0.0, 0.25, 0.5, 0.75, 1.0} while the others sit at a neutral
mid. Assert the rendered output (RMS over the block, after a warmup) CHANGES for
at least one param setting — i.e. the param is LIVE (not dead/no-op).

Param semantics (from FxProcessors.h — the INTENDED mapping):

| effect | p0 | p1 | p2 | p3 | p4 |
|--------|----|----|----|----|----|
| Diffuser | (none; amount pinned) | — | — | — | — |  → expect NO single-param change (all inert); the only control is chain dry/wet |
| PitchShifter | Pitch | Size | Spread | — | — |
| Reverb | PreDelay | Diffusion | Time | Tone(LP) | LowCut(HP) |
| LoopingDelay | Position | Size | Pitch | Freeze | — |
| WSOLAStretch | Pitch | Position | Size | Freeze | Tone |
| Spectral | Pitch | Warp | Position | Blur | Freeze |
| Wavefolder | Drive | Fold | Bias | Tone | — |
| FrequencyShifter | Shift | Shape | Feedback | Spread | — |
| RingModulator | Carrier | Shape | Amount | — | — |
| Resonator | Pitch | Decay | Bright | Position | Structure |
| ClockedDelay | Sync(div) | Feedback | Age | Grit | — |
| Ensemble | Rate | Depth | — | — | — |
| PlateReverb | Decay | Damping | PreDelay | — | — |
| VinylCompressor | Comp | — | — | — | — |
| Phaser | Rate | Depth | Feedback | — | — |

"---" = unused for that effect (the effect's setParams ignores it); the sweep
will confirm those are inert (output identical) — which is itself a documented
contract. A param marked with a name MUST move the output.

## FX topology (3) x order (6) routing

- All-disabled chain = bit-identical dry passthrough (no latency, mix full).
- Series A->B->C: each enabled slot processes the running signal.
- Parallel12to3: A,B each process a copy of the dry (equal-gain sum) -> C.
- Parallel1to23: A -> (B || C) equal-gain sum.
- Every order permutation of {0,1,2} renders finite for an all-enabled chain.

## FX master section (4 params)

| param | intended | how verified |
|--------|----------|--------------|
| `fx_mix` | global wet/dry 0..127 (127=full wet) | audio: mix=0 => dry, mix=127 => wet |
| `fx_eq_low` | low-cut (HP) 0..127 (0=off) | audio: a low sine is attenuated more at high setting |
| `fx_eq_mid` | mid peak gain 0..127 (64=0dB) | audio: a mid sine gain changes |
| `fx_eq_high` | high shelf gain 0..127 (64=0dB) | audio: a high sine gain changes |

## FX mod matrix (16 slots x {src,dest,amount})

18 destinations = 3 slots x (drywet + 5 params). For each destination, set a
mod slot: source = CONST_128 (a steady 0.5), dest = that destination,
amount = +63. Render with FX enabled. The modulated value MUST differ from the
unmodulated value — verified via `FxChain::debugGetParam(slot, idx)` (the value
the DSP actually consumes). This proves every dest reaches the DSP at full
depth through the engine path.

Also: amount=0 => no movement; source=CONST_4 (low) vs CONST_256 (high) =>
different param values (source depth is honored).

---

# FAILURES / DRIFTS

Any check that fails is either:
- (A) a real BUG -> fix in Source and re-verify (record in COVERAGE_FINDINGS.md),
- (B) an INTENDED behavior mismatch (the spec was wrong) -> correct the spec,
- (C) a DESIGN DRIFT requiring user action -> record in COVERAGE_FINDINGS.md
      with the symptom, root cause, and recommended action.

---

## Deterministic tooling

- **parvati_loader_fuzz_test** (`tests/loader_fuzz_test.cpp`, built by default):
  loader fuzzer + rollback checker — ~300 deterministic mutated-file cases over
  the four real save formats (.PRO/.MUL x2/.parvati multi+patch built via the
  real save paths) plus truncated engine-state blobs. Pins two properties: a
  load returning FALSE leaves the processor state BIT-IDENTICAL
  (validate-before-mutate), and a load returning TRUE renders 32 blocks inside
  a 10 s watchdog with finite audio (loaded-bytes audio-thread-hang class).
  Canary self-checks prove the comparator and the watchdog detect what they
  must. Run: ./build_release/parvati_loader_fuzz_test
- **parvati_shadow_state_test** (`tests/shadow_state_test.cpp`, built by
  default): shadow-state defaults property — loading a DEFAULTS-ONLY
  .parvati multi (saved through the real path) into an engine polluted on
  every mirrored surface (custom tuning tables, part names, staged FX slot
  types, arp config, voice slots, channel/key zone, PartData bytes 3/4/15)
  must leave it BIT-IDENTICAL to a fresh engine. The canary proves the diff
  comparator reports every pollution category before the load. Run:
  ./build_release/parvati_shadow_state_test
- **parvati_ui_mirror_test** (`tests/ui_mirror_test.cpp`, built by default):
  UI mirror consistency — after any engine mutation path (apvts writes,
  engine-direct slot/channel/zone/name writes, processor-level loads, live-
  editor state recall) the Patch page must display exactly the engine state
  via both real seams (the reveal refresh and the pollPatchPageMirror
  displayVersion poll); all 6 rows + arrangement + "Voices Y/96" compared, name
  labels found by row layout geometry, canary proves the comparator detects a
  stale pair. Run: ./build_release/parvati_ui_mirror_test
- **parvati_load_invariants_test** (`tests/load_invariants_test.cpp`, built by
  default): edge-corpus load invariants — any load that SUCCEEDS leaves the
  engine inside its invariant ranges on every mirrored surface (staged arp/seq
  config + the live objects after service, routing with lo<=hi, voice slots,
  resolved tuning, installed FX slot types, sanitized names) and renders 32
  blocks under a 10 s watchdog. Hand-written .parvati multi corpus with one
  edge per case (arp_mode/octave/direction/pattern/resolution, seq lengths,
  channel 0/1/16/17/255, keyzone clamp + inversion swap, slots 0/1/16/17/99,
  spread/poly/raga bytes, tuning_mode 33/34, 40-char + quoted names); canary
  proves the checker detects a hand-broken engine. Caught + fixed a live
  instance at authoring time (the .parvati YAML path staged arp bytes raw —
  arp_mode: 5 = silent part). Run: ./build_release/parvati_load_invariants_test
- **parvati_check_combo_clear** (`tools/check_combo_clear.py`, ctest, no
  build): static guard for the stale-async-onChange class — every
  juce::ComboBox `.clear()`/`->clear()` in Source/ must pass
  `juce::dontSendNotification` (JUCE's default `sendNotificationAsync`
  queues an onChange that a later dontSendNotification setSelectedId cannot
  cancel, so a rebuild fires the handler with no user action; caught twice
  in the 2026-08 hunt — PatchPage refreshLanguage + SettingsPanel osCombo).
  ComboBox-typed receivers resolved by declaration tracking + the known
  combo-name set; an embedded canary (--self-test) must flag a seeded bad
  snippet before the real scan runs. Allowlist
  tools/check_combo_clear_allowlist.txt (aim empty). Run: ctest -R
  parvati_check_combo_clear.
- **parvati_check_async_this** (`tools/check_async_this.py`, ctest, no
  build): static guard for the raw-`this`-in-async-callback crash-window
  class — every lambda capturing `this` handed to an async UI sink
  (PopupMenu showMenuAsync / item setAction / addItem on a tracked
  juce::PopupMenu, ModalCallbackFunction::create, chooser launchAsync,
  static dialog `launch` helpers, DialogWindow LaunchOptions spans) must be
  SafePointer/WeakReference-guarded (guard in the capture init, earlier in
  the statement, or the 5 preceding lines — a later sibling lambda's guard
  deliberately does NOT mask an outer raw capture). Allowlist carries
  hand-verified safe sites with justifications (member-owned FileChooser
  completions — safe by the JUCE ~FileChooser cancellation contract).
  Run: ctest -R parvati_check_async_this.
- **parvati_roundtrip_golden_test** (`tests/roundtrip_golden_test.cpp`, built
  by default): golden byte round-trip property — save -> load (fresh
  processor) -> save is BYTE-IDENTICAL for every format pair (.PRO/.MUL/
  .parvati patch/.parvati multi) over an adversarial program-name corpus
  (double-quote / backslash / four UTF-8 kinds / >16-byte UTF-8 truncation /
  40 chars / padded / control chars) on a rich seeded state (custom tunings,
  part names, FX slot type, slots+routing, options), and the loaded title
  equals the format-documented normalized form (16-byte chunk: control-strip
  + code-point-safe truncation + trailing trim; .parvati doc name:
  control-strip). The .parvati fixed point runs with title == file basename
  (the documented filename-retitling); the adversarial corpus pins the
  un-escaped-name class (a quote/newline name used to save an unloadable
  file). Canary proves the byte comparator flags a 1-flipped-byte file.
  Run: ./build_release/parvati_roundtrip_golden_test
- **parvati_undo_property_test** (`tests/undo_property_test.cpp`, built by
  default): undo round-trip property — one user op, one transaction, one
  undoSafe() and the FULL host-visible state (APVTS tree + engine blob) must
  return byte-identical to pre-op. Battery: float patch-byte / PartData
  choice (byte 15) / controller arp choice / signed mod amount / FX param /
  FX type (with the FxSlotCard engagement seeding firing re-entrantly inside
  the same transaction — a live editor is created so the seeding path is
  real). Plus the two corruption classes: the W7 seed-clobber (undo of a type
  switch restores the USER values) and the W2 part-switch doctrine (stack
  swept to canUndo==false, no cross-part replay, part A keeps its edit, the
  switch survives). Canary: the byte comparator rejects a 1-byte-doctored
  snapshot; mutation-tested (doctrine removal and undo-replay engine-push
  skip both fail the suite; the unguarded seed replay is additionally
  structurally no-op'd by JUCE's mid-undo perform() discard — documented in
  the file header).
  Run: ./build_release/parvati_undo_property_test
- **parvati_layout_minwidth_test** (`tests/layout_minwidth_test.cpp`, built by
  default): min-width header layout sweep — every placed interactive header
  child (Button/ComboBox, direct + one level deep, effectively visible,
  in-band, not offscreen; the folded "+"/"-"/"0" zoom trio is the documented
  unplaced exception) keeps positive width AND height at every legal desktop
  size (1024..1800 x 600); the preset browser / Patch button / part combo /
  [FX] button keep >= 80% of their documented natural widths (156/64/88/50);
  no pairwise header sibling overlap (2px tolerance). Canary: the raw
  predicates flag a seeded zero-extent rect, a deep overlap, the historical
  38-of-50 squeeze, and pass adjacent/hairline/compliant cases. Sub-1024 is
  deliberately unswept (AUv3 collapse = known deferred item).
  Run: ./build_release/parvati_layout_minwidth_test
- **parvati_check_translations** (`tools/check_translations.py`, ctest, no
  build): TRANS-key completeness — every TRANS() literal in Source/ must be a
  key in BOTH the FR and DE tables of Source/ui/Translations.cpp, parsed
  exactly as juce::LocalisedStrings parses them at runtime (escaped quotes,
  \xNN hex byte escapes, adjacent-literal compiler concatenation; '+-joined'
  TRANS fragments are each their own key). Fails on FR/DE asymmetry and on
  duplicate keys within a block. Two-section allowlist
  (tools/trans_allowlist.txt): ## intentional proper nouns (12-EDO, SYNTH/FX
  glyphs), ## known-missing tracked gaps (56 tooltip-prose keys awaiting
  translation) — a NEW missing key is in neither section and fails (ratchet).
  Dynamic TRANS(<variable>) calls (21) are counted/reported, out of scanner
  scope. All three scanners also run via one build target:
  `cmake --build build_release --target parvati_static_checks`.
  Run: ctest -R parvati_check_translations.

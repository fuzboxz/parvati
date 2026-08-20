# FX Feature — Final Code Review

**Branch:** `fx` vs `main` (8 commits, 32 files, +4181/-40)
**Reviewer scope:** Complete diff — FX DSP core, engine integration, processBlock,
params/APVTS bridge, serialization, UI, tests.
**Verification:** build is green; `parvati_fx_preset_test`, `parvati_host_state_test`,
`parvati_concurrency_test`, `parvati_editor_test`, `parvati_apvts_test`,
`parvati_arp_test` all re-run and PASS during this review. (`parvati_patch_load_test`
is pre-existing on `main` and excluded.)

---

## Verdict: **APPROVE**

The per-part FX feature is correct, faithful to `FX_IMPLEMENTATION_PLAN.md`, threading-sound,
and well-tested. No blockers, no CONTRACT deviations, no safety issues. The items below are
optional nits/observations for a future pass — none block shipping this v1 placeholder.

---

## Blockers

**None.**

---

## Non-blockers / nits (optional improvements)

1. **AT allocation on FX-type change (acceptable, matches existing precedent).**
   `FxChain::setSlotType` (`Source/dsp/fx/FxChain.cpp:35-52`) calls `createFxProcessor`
   (`make_unique`) + `prepare` on the audio thread when a slot's type changes (serviced from
   `renderPartFx`). This is a real-time allocation, but it is (a) **not in the per-block
   `process()` path** — only on an actual type change, guarded by the `slotType_ == tv`
   early-out at line 38 so it fires at most once per change; and (b) **identical to the
   existing precedent** (`AmbikaVoice::setOversamplingFactor` / `setFilterTopology` defer a
   rebuild to the voice's audio thread). For an infrequent config change this is the
   established codebase pattern. If strict RT-safety is later required, stage the rebuild via
   a double-buffered processor swapped under `fxDirty_`. **Not a blocker.**

2. **Parallel dry/wet is a global blend, not per-slot (`Source/dsp/fx/FxChain.cpp:148-175`).**
   In Parallel topology each enabled slot processes a copy of the input with equal weight
   (`/ activeCount`); the per-slot `dryWet` only contributes to the *mean* blend `W`, so it
   does not independently scale that slot's wet contribution. This matches the plan's stated
   "simple equal-gain sum scaled by 1/activeCount, then dry/wet blend" — intended, not a bug —
   but worth a tooltip/label so users don't expect per-slot wet control in parallel.

3. **Default Gain+Pan params are surprising (`Source/dsp/fx/FxProcessors.cpp:28-35`).**
   With all params at the contract default of 0, Gain+Pan yields **-12 dB, hard-panned left**
   (gain param 0 → -12 dB; pan param 0 → hard-left). A first-time enable is quiet and
   off-centre. Params default to 0 per the contract, so this is by-spec; consider a non-zero
   default pan (centre) for the placeholder only if desired. Cosmetic.

4. **Reverb `param2` (wetLevel) is redundant with the slot dry/wet.** With `dryLevel=0`
   (`FxProcessors.cpp:135`), `wetLevel` (param2) scales the whole reverb output, overlapping
   the chain's per-slot dry/wet. Two wetness controls on one slot. Fine for a placeholder;
   noting for the eventual DSP pass.

5. **`vi < kNumParts` bounds check is coincidentally correct (`Source/SynthEngine.cpp:1203`).**
   The mono-sum loop guards `voiceCardBuffers_[vi]` with `vi < kNumParts` (6), but `vi` is a
   *voice* index; the correct semantic bound is `kNumVoices` (also 6). Correct today because
   `kNumVoices == kNumParts`, but the intent reads clearer as `vi < kNumVoices`. Pure nit.

6. **Reverb/Chorus re-apply params every block (`Source/dsp/fx/FxProcessors.cpp:141,188`).**
   `FxChain::process` calls `proc->setParams(...)` every block, which sets `dirty_=true`;
   `process()` then applies the params every block. Cheap and correct, just slightly wasteful.
   A "params unchanged since last setParams" short-circuit would avoid it. Micro-optimisation.

7. **`applyParvatiPatch` publishes intermediate (torn) FX frames during load
   (`Source/ParvatiPreset.cpp:519-527`).** Each `setValueNotifyingHost` fires
   `parameterChanged` → `applyFxParameter` with no `loadingPartIntoApvts_`-style guard, so
   `setFxModSlot` may briefly read stale sibling values mid-loop. The **end state is fully
   consistent** because `applyParvatiPatch` finishes with `syncAllParamsToEngine()`
   (`ParvatiPreset.cpp:529`) which re-applies every FX param with all siblings present, and the
   test `[2]` confirms a 0-mismatch round-trip. At worst a single-block glitch on rapid patch
   load. Contrast with `applyParvatiMulti`, which writes `fxState` directly + sets `fxDirty_`
   **once** after the loop (`ParvatiPreset.cpp:715`) — the cleaner pattern. Non-blocking.

8. **`renderPartFx` evaluates the FX mod matrix every block even when fully bypassed
   (`Source/SynthEngine.cpp:1223-1243`).** When `!anyEnabled()` the chain dry-copies, but the
   block still samples mod sources + evaluates the 16-slot matrix + calls `setSlot*`. Harmless;
   an early-out on a per-part `anyEnabled()` check would save the work. Minor.

---

## What's verified & sound

- **Threading model is correct.** `PartFxState` fields are all `std::atomic`; MT setters
  (`SynthEngine.cpp:269-308`) do relaxed field stores + a single `fxDirty_` release-store; the
  AT services it with `fxDirty_.exchange(false, acq_rel)` (`SynthEngine.cpp:1172`) and reads the
  frame with relaxed loads — the release/acq_rel pair establishes happens-before, so the whole
  frame is visible. `setFxModSlot` writes all three matrix fields under one `fxDirty_` publish,
  eliminating torn matrix slots. `lastModSources_`, `fxCached_`, `fxMonoScratch_` are AT-only
  (verified: only touched in `prepare` and `renderPartFx`). No unguarded MT/AT shared mutable
  state. TSAN-clean run of the concurrency test (FX mutations enabled) corroborates this.

- **Dry-copy bypass is audibly-identical to the pre-FX path.** `FxChain::anyEnabled()` is false
  by default (all `enabled=0` → `slots_[s]` null for None); `process()` then copies `in→out`
  (`FxChain.cpp:68-73`). `processBlock` now sums the per-part stereo FX-output buffers into the
  main bus (`PluginProcessor.cpp:228-234`), which equals the old sum-of-voicecards (modulo float
  reassociation across the per-part grouping — explicitly accepted). Aux buses stay dry
  (`PluginProcessor.cpp:250-258`, unchanged source).

- **Series/Parallel mixing math is sane.** Series: per-slot `dry*(1-dw)+wet*dw` over the
  running signal with a pre-process dry snapshot (`FxChain.cpp:84-110`). Parallel: equal-gain
  wet sum `/activeCount` blended with the original input by the mean dry/wet (`FxChain.cpp:120-184`).
  No aliasing between in/out/wet/dry buffers; input (`mono`) is never mutated by an effect.

- **No audio-thread allocation in the per-block render path.** `FxChain::process`,
  `FxProcessor::process`, and `renderPartFx` use only pre-sized member buffers + stack arrays
  (`modOffset[15]`, `effDryWet[3]`, `effParam[3][4]`). All scratch buffers are `assign`'d in
  `prepare`. (The only AT allocation is the infrequent type-change rebuild — see nit #1.)

- **juce::dsp prepare/reset correctness.** Delay/Reverb/Chorus each get a `ProcessSpec{rate,
  block, 2}` in `prepare` and a `reset`; the DelayLine max-delay is sized to the 0..1 s param
  range (`FxProcessors.cpp:62-66`). Reverb/Chorus defer param application to `process()` via a
  `dirty_` flag (set single-threaded on the AT).

- **Param decode is correct (no off-by-one).** `applyFxParameter` (`PluginProcessor.cpp:402-456`)
  maps `fx{1..3}_*` → slot 0..2, `param{1..4}` → idx 0..3, `fxmod{1..16}_*` → slot 0..15, with
  `fx_topo`/`fx_order` excluded from the slot branch by the `id[3]=='_'` guard. FX-mod amounts
  re-read all three siblings (torn-slot guard). `loadPartIntoApvts` mirrors the reverse decode.
  `parvatiValueToPatchByte` returns 0 for `isFx` (no patch byte). Descriptor count = 252 = 105
  synth + 5 arp + 4 options + 67 seq + **71 fx** (confirmed by `arp_test`).

- **The `loadingPartIntoApvts_` guard is correct and necessary.** It suppresses the
  `parameterChanged`→engine feedback during `loadPartIntoApvts` (redundant for byte/arp/seq,
  **harmful** for the FX matrix whose `applyFxParameter` reads stale siblings mid-load). RAII
  guard restores it; message-thread-only (synchronous listener delivery). Sound.

- **Serialization is correct and endian/truncation-safe.**
  - Binary v2 (`SynthEngine.cpp:316-373`): version 1→2, fixed 71-byte FX block per part with a
    4-byte LE length prefix; `restoreState` (`SynthEngine.cpp:398-432`) accepts v1 *and* v2,
    rejects truncated FX reads (`getNumBytesRemaining() < fxLen`), and the `take()` lambda guards
    `o < fxLen`. No FX bytes leak into the Ambika core (the FX block is appended *after* the
    routing bytes, length-prefixed).
  - `.parvati` multi: `partRaw` (`ParvatiPreset.cpp:381-419`) + `applyParvatiMulti`
    (`ParvatiPreset.cpp:662-715`) read/write `fxState` directly and set `fxDirty_` **once** per
    part after the loop (clean, no torn frame). `.parvati` patch applies via APVTS +
    `syncAllParamsToEngine` (end state consistent).
  - **Ambika formats unchanged:** `loadProgramFromBytes` / `saveProgramFile` / `saveMultiFile`
    all skip `|| d.isFx` (`PluginProcessor.cpp:588,631,776`). Verified by `fx_preset_test` [3]/[4]
    — `.PRO`/`.MUL` leave fxState at defaults.
  - **v1 back-compat:** a hand-crafted v1 blob (FX block stripped, version rewritten 2→1) loads
    with FX at defaults while the core round-trips (`host_state_test` [4], passing).
  - Endian-independent: every multi-byte value is the version byte or the manually LE-encoded
    length prefix; all other fields are single bytes.

- **`sectionForId` routing is collision-free (`PluginEditor.cpp:86-91`).** `fxmod*`→`FxMatrix`
  and `fx{1,2,3}_*`/`fx_*`→`Fx` are checked before the generic synth prefixes; `fxmod` does not
  start with `mod`, and `fx_topo`/`fx_order` don't match the slot-digit guard. Teardown order is
  correct: `fxMatrixView_` declared before `fxWorkspace_`
  (`PluginEditor.h:582-583`), so the non-owned view is detached before deletion — mirroring the
  `modMatrixView_`/`synthWorkspace_` discipline. The shared-generator reparenting
  (`setFxMode`, `PluginEditor.cpp:2882-2907`) releases from the outgoing workspace before
  reparenting into the incoming one (single parent). `editor_test` confirms the FX tab +
  `FxMatrixView`-as-direct-child-of-`FxWorkspace` wiring.

- **Tests are adequate.** `fx_preset_test` (multi/patch round-trip + `.PRO`/`.MUL` drop + legacy
  forward-compat), `host_state_test` (binary v2 round-trip + v1 back-compat with a hand-derived
  v1 blob), `concurrency_test` (FX section **enabled** so the real chain runs on the AT under
  TSAN), `editor_test` (FX tab + matrix wiring). Coverage spans every format and the MT/AT race.

---

## CONTRACT faithfulness (vs `FX_IMPLEMENTATION_PLAN.md`)

- `PartFxState` struct fields, types, and the `fxDirty_` pattern: **exact match**
  (`SynthEngine.h:177-189`). `kNumFxSlots=3`, `kNumFxMatrixSlots=16`, `kNumFxSlotParams=4`,
  `FxType`/`FxTopology`/`FxModDestination` enums, `fxOrderPermutation(0..5)`: all present in
  `dsp/fx/FxTypes.h` as specified.
- 71 param IDs (`fx{1,2,3}_type/enabled/drywet/param{1..4}` + `fx_topo` + `fx_order` +
  `fxmod{1..16}_source/dest/amount`) with the specified ranges/defaults: **match**
  (`ParameterLayout.cpp:485-509`).
- Engine signatures (`prepare` extension, `renderPartFx`, `getFxOutputBuffers`, the 7 MT setters):
  **match** (`SynthEngine.h:415-442`).
- `isFx` descriptor flag + `applyFxParameter` + dispatch in `parameterChanged`/
  `applyParameterToEngine`/`loadPartIntoApvts`: **match**.
- Serialization (`.parvati` via partRaw/applyParvatiMulti isFx branches; binary v2
  length-prefixed; `.PRO`/`.MUL` `|| d.isFx`): **match** (the design's separate `fx:` YAML block
  was deliberately simplified to flat `params:` — same data, documented).
- UI (header `[Synth]/[FX]` toggle, `FxWorkspace` clone, `FxMatrixView` clone, shared generator):
  **match**.

---

## Recommended next actions

- **Ship as-is.** No blockers; the feature is correct, faithful, and tested.
- Optional, non-blocking follow-ups (any subset): nit #1 (stage the FX-type rebuild off the AT
  if strict RT-safety is later required), nit #2/#3/#4 (placeholder DSP/UX tuning), nit #7
  (give `applyParvatiPatch` the same once-per-load `fxDirty_` discipline as `applyParvatiMulti`
  to eliminate the transient torn frames).

# FX Feature — Design Decisions (authoritative)

These resolve the open questions in `FX_SCOUT_BRIEF.md` (§Open design questions).
The planner and worker MUST follow these. Rationale is kept short.

## Architecture

- **FX runs at HOST sample rate, STEREO.** Voicecard buffers are mono (host-rate);
  the FX stage sums each part's voicecards to a mono sum, duplicates it to L+R as
  FX input, runs the FX chain in stereo, and writes a per-part **stereo** FX-output
  buffer. The processor mixes the per-part stereo FX buffers into the main bus.
- **Insertion: Option A (in `SynthEngine`, post-render).** Add
  `SynthEngine::renderPartFx()` called from `PluginProcessor::processBlock`
  **after** `engine_.renderNextBlock` and **before** the main-bus sum. SynthEngine
  owns the per-part stereo FX-output buffers (sized in `prepare` like
  `voiceCardBuffers_`). The processor reads them for the main mix.
- **Aux buses (VC1..VC6) stay DRY (no FX).** They remain raw per-voicecard taps.
- **Block granularity:** FX + FX-matrix evaluation run once per **full host block**
  in `processBlock` (after `renderNextBlock` completes), exactly like the existing
  DC-blocker pass. NOT inside `renderVoices` (sub-block).

## Modulation sources for the FX stage

- **Decision: sample the most-recently-active voice's mod sources per part.**
  In `renderPartFx`, for each part pick the first *active* voice in the part's
  `voiceIndices` and read its `getModulationSource(idx)` (0..255). Sources are
  control-rate (~1 ms); a per-host-block read is sufficient for v1. If no voice is
  active, sources hold their last value (keep a per-part atomic snapshot updated
  whenever a voice IS active so tails still modulate — simplest: snapshot every
  block when any part voice is active, else reuse last snapshot).
- **Sources are the SAME `MOD_SRC_*` enum + `makeModSources()` choice list +
  `ModSourceCatalog` + `CentralModBar`.** No new sources. Modulators come FROM the
  synth (Env/LFO/Seq/Arp/Op) — editing them edits the SAME synth-side values.

## FX mod matrix

- **Decision: fixed-max APVTS params, 16 slots, IDs `fxmod1..16_source/_dest/_amount`.**
  This reuses the entire APVTS + `ModMatrixView` machinery verbatim, keeps FX-matrix
  amounts host-automatable/undoable, and is plenty for v1. Inactive (amount==0)
  slots are hidden in the UI exactly like the synth matrix.
- **FX destinations are a NEW `FxModDestination` enum** (`FX_DST_*`): dry/wet of
  each of the 3 slots + each slot's generic params (`fx1_drywet, fx1_param1..4,
  fx2_*, fx3_*`), plus maybe a couple of global FX targets. Distinct from
  `MOD_DST_*`.
- **Completely separate from the synth matrix:** separate param IDs, separate
  `FxMatrixView` instance, separate dest combo list. The two matrices never share
  state.

## FX slots / effects (placeholder set for v1)

- **3 slots** per part. Each slot has:
  - `fx{1,2,3}_type`  — choice: `None / Gain+Pan / Delay / Reverb / Chorus`
  - `fx{1,2,3}_enabled` — on/off (0/1)
  - `fx{1,2,3}_drywet` — 0..1 (0 = fully dry, 1 = fully wet)
  - `fx{1,2,3}_param1..4` — four generic 0..1 params, meaning depends on type
- **Placeholder effects** (implement with JUCE `juce::dsp` where possible):
  - `None`  — bypass
  - `Gain+Pan` — stereo gain + L/R pan (param1=gain, param2=pan)
  - `Delay` — `juce::dsp::DelayLine` (param1=time, param2=feedback, param3=... )
  - `Reverb` — `juce::dsp::Reverb` (param1..4 = room/damp/wet/mix sub-params)
  - `Chorus` — `juce::dsp::Chorus` (param1=rate, param2=depth)
  Dry/wet is applied per slot by the chain, not inside the effect.
- **Order is changeable** (a per-part `order[3]` permutation) and **topology is
  series OR parallel** (a per-part enum). v1 UI: a topology toggle + up/down
  reorder buttons on the slot panels (drag-reorder can be a later refinement).

## Parameters / storage / threading

- **New descriptor flag `isFx`** on `PatchParamDescriptor`. FX descriptors:
  `byteOffset = -1`, `isFx = true`. Routed via a new `applyFxParameter` (mirrors
  `applyOptionParameter`), dispatched in `parameterChanged` and
  `applyParameterToEngine`; loaded in `loadPartIntoApvts` (mirrors `isArp` branch).
- **Per-part FX storage** = a new `struct PartFxState` living in `Part`
  (`SynthEngine.h`). Stores: slot types/enabled/drywet/params, FX-matrix
  16×{source,dest,amount}, order[3], topology. Use `std::atomic` per field (or an
  `AtomicByteArray` snapshot) following the existing MT-write/AT-read discipline.
- **Dirty flag** `Part::fxDirty_` (atomic bool). `applyFxParameter` writes the
  current part's FX storage + sets `fxDirty_`. The AT FX render path
  `exchange(false, acq_rel)`s it and copies staged params into the active FX DSP
  objects single-threaded, then renders (exactly the `optionsDirty_`/`frameDirty_`
  pattern). The FX objects themselves (`juce::dsp`) live AT-side in the engine.
- **FX param smoothing:** each modulated/target FX param uses a
  `juce::SmoothedValue` (always on, ~20 ms ramp) for zipper-free knob/automation
  moves. Independent of the voice `ui_smoothing` toggle.

## Serialization

- **Ambika `.PRO`/`.MUL`:** FX is naturally dropped. Add `|| d.isFx` to the
  `skip` conditions in `saveProgramFile`/`saveMultiFile` (and any descriptor
  iteration that writes patch bytes). FX descriptors have `byteOffset=-1` so they
  never touch patch/part bytes.
- **Binary host-state blob:** bump version **1 → 2**. After the per-part routing
  bytes, append the FX state as a **length-prefixed trailing block per part** (so a
  future v3 or a truncated blob degrades gracefully). `restoreState`: read magic +
  version; v1 → core only, FX defaults; v2 → core + trailing FX block. Keep the
  strict-version reject behavior (caller falls back to legacy APVTS restore).
- **`.parvati` YAML multi:** add a per-part `fx:` block inside each `parts:` entry
  (slot types/params, order, topology, fxmod slots). Absent `fx:` in old files →
  FX defaults. The hand-rolled YAML parser already supports nested maps; unknown
  keys are ignored (forward-compat).
- **`.parvati` YAML patch (single part):** also carry an `fx:` block for the
  current part so a single-part Parvati patch round-trips its FX too.

## UI

- **Header:** becomes `Part [Part 1] [Synth] [FX] [Multi]`. Insert a
  **[Synth]/[FX] mode toggle** between `partCombo_` and `multiButton_`. `Global`
  stays as its own button (it's a global-options overlay). The toggle is a
  view-mode selector (NOT an APVTS param), like `multiButton_`/`globalButton_`.
  Copy the existing button styling/L&F; exact pixel placement will be tuned later
  ("you don't need to care about the design, just copy the current one").
- **Mode toggle mechanism:** promote the hidden single-tab `pageSelector_` to a
  visible 2-tab bar `[SYNTH][FX]`, OR swap `pageSelector_` content
  (`synthWorkspace_` ↔ `fxWorkspace_`) on toggle. Either is fine; pick the least
  disruptive to the existing layout constants. Reuse the same overlay mechanism
  for Multi/Global.
- **`FxWorkspace`** (clone of `SynthWorkspace`, same 3-row skeleton):
  - TOP: **3 FX-slot panels** (FX1/FX2/FX3) — ParamPages generated from the
    `fx*_` descriptors, reparented non-owned (like OSC/MIX/FILTER).
  - MIDDLE: **reuse the SAME `CentralModBar`** (modulators come from synth; the
    generator editors modify the same values).
  - BOTTOM-LEFT: the **shared** active-generator editor (the SAME editor-owned
    generator ParamPages as SYNTH — reparented, single active selection).
  - BOTTOM-RIGHT: **`FxMatrixView`** (clone of `ModMatrixView`, FX destinations,
    `fxmodN_*` IDs).
- **Shared modulator editor:** the generator ParamPages are SHARED between SYNTH
  and FX (single active selection). Mode toggle moves which workspace hosts them.
  Do NOT duplicate generator pages (would duplicate APVTS attachments).
- **Reuse:** clone `ModMatrixView` → `FxMatrixView`; clone `SynthWorkspace` →
  `FxWorkspace`; reuse `CentralModBar`, `ModSourceCatalog`, the drag payload
  `"parvatiModSrc:<enum>"`, the `ParamControl`/`ParamPage` generation, the
  `ParvatiLookAndFeel`, the theme system.

## Build / test

- New files under `Source/` are auto-globbed — no CMake edit needed for the plugin.
- Add a new test executable mirroring `parvati_preset_test` (FX YAML round-trip)
  and extend `host_state_test` (FX binary round-trip). Add FX param mutations to
  `concurrency_test`'s MT op set.
- Develop against the **Debug** build dir `build/`. Keep the diff minimal and
  faithful to existing conventions. Update `CHANGELOG.md`.

## Out of scope for v1 (explicit non-goals)

- Sample-accurate FX modulation (block-rate is fine).
- Drag-to-reorder slots (up/down buttons suffice).
- A rich effect library (5 placeholder effects suffice).
- Per-part post-FX aux buses (aux stays dry).
- Undo/redo beyond what APVTS already gives the FX params (free via APVTS).

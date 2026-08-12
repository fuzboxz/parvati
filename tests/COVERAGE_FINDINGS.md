# Parvati Test Coverage — Findings & Drifts

Findings from the consolidated coverage binaries
(`parvati_fx_param_coverage_test`, `parvati_synth_param_coverage_test`).
Each row compares the INTENDED vs REAL outcome. Categories:
- **(A)** real bug → fixed in Source (recorded under "Source fixes applied").
- **(B)** spec/test premise wrong → corrected the assertion, real behaviour documented.
- **(C)** design drift → surfaced for triage; **action required from you**.

---

## Source fixes applied (FX coverage)

### SF-1: FV-1 RateBridge biquad denormal flush (real bug — fixed)

- **File:** `Source/dsp/fx/fv1/Fv1Engine.h` — `BiquadLP::process()`.
- **Symptom:** `parvati_fx_param_coverage_test` testPerEffectFinite found the
  VinylCompressor emitted **subnormal (denormal) floats** at mid/max settings
  (198 subnormals measured after a silence tail at Compress=0). Denormals cause a
  ~50× CPU stall on x86 audio threads — a real-time audio killer.
- **Root cause:** the RateBridge's input/output BW-limit biquads (4th-order
  Butterworth @ 15 kHz, used by **every FV-1 effect**: ClockedDelay / Ensemble /
  PlateReverb / VinylCompressor / Phaser) are Direct-Form-II-Transposed filters
  whose `z1/z2` state decays toward 0 but never reaches it in finite steps when
  fed silence (a paused track, the gap between notes, or a reverb tail). The state
  passes through the subnormal range (~1e-38..1e-45).
- **Fix:** flush `z1`/`z2` to 0.0f when their magnitude drops below
  `std::numeric_limits<float>::min()`. Inaudible (subnormals are ~280 dB below
  full scale at a 15 kHz target) and removes the stall. Added `<limits>` include.
- **Validation:** `parvati_fx_param_coverage_test` — VinylCompressor
  `@0.0/@0.5/@1.0: no subnormals` now PASS (was FAIL). All 5 FV-1 standalone
  tests + clouds_fx_test + fx_routing_test still PASS (no behavioural change).

### SF-2: test-only debug accessors (instrumentation)

- **Files:** `Source/dsp/fx/FxChain.h` (`debugGetDryWet`), `Source/SynthEngine.h`
  (`debugGetChainValue`).
- **Why:** the FX mod-matrix coverage needs to prove every one of the 18
  `FxModDestination` values reaches the DSP at full depth **through the full
  engine path** (engine → `renderPartFx` → `setSlotDryWet`/`setSlotParam` →
  `params_`/`dryWet_`). `FxChain::debugGetParam` already covered the 5 slot
  params; the dry/wet field (3 of the 18 dests) had no read-back. Added
  `debugGetDryWet` (1 line, mirrors `debugGetParam`) and a combined
  `debugGetChainValue(part, slot, field)` accessor. Zero behavioural change;
  test-only, consistent with the existing `debugFxProcessCallCount` pattern.

---

## FX parameter + module coverage

**Result: 292/292 checks PASS** (all 16 FxType values; every effect finite at
{0,0.5,1}; every effect full-wet differs from dry; None is a bit-identical
passthrough; all 58 active/inactive param-sweep checks; latency; 3×6 topology
routing; master mix + 3-band EQ; **all 18 FX mod-matrix destinations reach the
DSP at full depth, verified exactly via `debugGetChainValue`**; 4 condition-
dependent Clouds param probes). The 3 generic-sweep invariances are RESOLVED
below (verified live under their correct DSP condition — not bugs).

### Real bug fixed
- VinylCompressor (and all FV-1 effects) denormal flush — **SF-1** above.

### RESOLVED (verified live under correct DSP condition — NOT bugs)

The generic steady-tone sweep (section 4) correctly sees these 3 Clouds params
as invariant because their effect requires a specific DSP state. Section 9 of
`parvati_fx_param_coverage_test` exercises that state and HARD-ASSERTS each
param moves the output. Root cause is faithful upstream Clouds semantics.

#### LoopingDelay `Size` (p1) and `Pitch` (p2) — RESOLVED (live in FREEZE mode)
- **Intended behaviour (confirmed):** the Clouds `LoopingSamplePlayer` has TWO
  paths. When `freeze` is OFF it is a single-tap **position delay** that reads
  `parameters.position * max_delay` and **ignores `size` and `pitch`** entirely
  (`looping_sample_player.h`: the `!parameters.freeze` branch never references
  them). Only the **freeze** branch uses `loop_duration = (0.01 + 0.99*size³) *
  max_delay` (Size = loop length) and `phase_increment = SemitonesToRatio(pitch)`
  (Pitch = playback ratio).
- **Proof (section 9, `@freeze`):** record a chirp into the 4 s buffer with
  freeze OFF, then enable freeze and sweep. Size 0→1: RMS **0.0407 → 0.0000**
  (short loop rings out to silence). Pitch −24st↔+24st: RMS **0.2014 → 0.1138**.
  Both move decisively. **Intended behaviour — no fix needed.**
- **Readout/UI note (optional):** the Looping Delay's Size/Pitch knobs are
  inaudible until Freeze is engaged; a tooltip/label hint could help users.

#### Spectral `Position` (p2) — RESOLVED (it is a texture ADDRESS, not a scrub)
- **Intended behaviour (confirmed):** in the Clouds phase vocoder,
  `StoreMagnitudes` writes the current analysis frame into `textures_[position]`
  and `ReplayMagnitudes` reads back from `textures_[position]` — the **same**
  slot (`frame_transformation.cc:84-88, 350-358`). So a **static** position
  gives identical output at every value: each slot independently tracks the
  same live input. Position is only observable when content is **retained**
  (freeze) and you switch to a different address, replaying a differently-
  retained spectral frame. This is upstream-faithful.
- **Proof (section 9, `@freeze-choreography`):** write `textures_[0]` with an
  evolving spectrum (freeze OFF, pos 0), freeze, then read pos 0 vs pos 1:
  RMS **0.3370 → 0.2968** (a real, deterministic ~12% change — there is inter-
  slot bleed so the other slot is not silent, but it is clearly different).
  **Intended behaviour — no fix needed.**
- **Readout/UI note (optional):** Spectral Position is meant to be **modulated**
  (it morphs between retained spectral frames); a static value is a no-op by
  design. A modulation hint on the label could help users.

### (Why the generic sweep sees invariance — methodology)
A **continuous, stationary tone** is loop-/comb-invariant and has a constant
spectrum, so it (correctly) cannot expose: a feedback comb that settles slowly
(ClockedDelay), a buffer effect's loop length/pitch (looper, non-frozen), or a
PV texture-address that needs retained content (Spectral Position). Section 4
uses per-effect inputs + warmups and surfaces these as drifts; section 9 then
resolves them with the correct condition. Inactive params (idx ≥
`activeParamCount`) are confirmed **exactly inert** (bit-identical waveform,
deviation 0.0) — the documented contract that `setParams` ignores them.

---

## Build / run
```
cmake --build build_release --target parvati_fx_param_coverage_test
./build_release/parvati_fx_param_coverage_test
```

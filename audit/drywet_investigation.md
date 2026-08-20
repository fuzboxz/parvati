# DRY/WET Behaviour Investigation — Parvati FX chain

**User report:** "lowering dry/wet for an effect didn't cut the delay properly until I moved the module after it."

## VERDICT: **A (series, expected) + B (real bug in parallel topologies). C ruled out.**

- **Series chains behave correctly** (Hypothesis A): lowering the delay's dry/wet kills its wet in ~0.15 s, but a *downstream* sustaining effect keeps re-emitting the delay's earlier wet for ~1.4 s. Not a bug — correct serial-FX behaviour. UX note recommended (below).
- **Parallel topologies have a real dry/wet bug** (Hypothesis B): `FxChain::renderParallel` applies the *mean* dry/wet to the *summed* wets instead of each branch's own dry/wet. A branch set to dry/wet = 0 is **never silenced** (leaks at −6 dB, echoes keep repeating forever at feedback), and sweeping one branch's dry/wet **changes the other branch's gain by −6 dB** (unlike a fully-bypassed branch, which exits `activeCount`).
- **C ruled out**: `fx{N}_drywet` is slot-indexed end-to-end; `fx_order` isn't even user-exposed.

## Measured evidence (headless FxChain, 48 kHz, 256-block, click 0.9, echo 100 ms fb 0.85, plate ≈ 2 s decay)

**Test 1 — Series, echo dry/wet 1.0 → 0.0 at t = 2 s (time until echo-periodic output < −60 dB below established level):**

| order | echo repeats persist | envelope < −60 dB |
|---|---|---|
| [Echo, Plate] (echo first) | **1.40 s** (k = 14 repeats) | 1.49 s |
| [Plate, Echo] (echo last) | **0.10 s** (k = 1) | 0.15 s |

⇒ A downstream sustainer multiplies the perceived "cut" time ~14×. Moving the delay *after* the sustainer makes the knob act in ~0.15 s (the 20 ms dw one-pole + one in-flight repeat). Expected serial behaviour.

**Test 2 — Parallel12to3 (A||B), click, A = echo 100 ms fb 0, B = echo 300 ms fb 0 (isolated pulses at t_A/t_B):**

| dwA | peak at t_A (branch A) | peak at t_B (**other** branch, dwB = 1 fixed) |
|---|---|---|
| 1.0 | −16.77 dB | −16.91 dB |
| 0.5 | −19.26 dB | −19.41 dB |
| 0.0 | **−22.79 dB (−6.02 dB, NOT silent)** | **−22.93 dB (−6.02 dB drop)** |

**Test 2b — A dw = 0 from the start, fb = 0.85, B = plate:** first echo repeat measures −22.79 dB and the feedback loop keeps repeating (−1.4 dB/repeat for seconds). **FAIL: branch leaks.**

**Test 3 — Series [Echo, Plate] blend at plate dw = 0.25:** `out = 0.75·dry + 0.25·wet` exact to 2.98e-8 (149.6 dBc). Series blend math verified; documents that a downstream slot's *dry* keeps the delay (expected).

**Test 4 — routing:** `setSlotDryWet(N)` writes exactly slot N; order permutation does not remap targets (probe asserts + code audit).

## Code audit (hypothesis C)

- `PluginProcessor.cpp:911` — `applyFxParameter` decodes `slot = id[2]-'1'` (fixed slot 1..3) → `SynthEngine::setFxSlotDryWet(slot,…)` (`SynthEngine.cpp:363`, `fxState.slotDryWet[slot]`) → `chain.setSlotDryWet(slot,…)` in `renderPartFx` (~:2124). Order is pushed *separately* via `chain.setOrder` (:2116). No cross-wiring.
- `ui/FxSlotCard.cpp:316-317` — card binds fixed `prefix_ = "fx{slot_+1}_"`; knob always targets its own slot. `ui/FxRoutingBar.h:17` — "`fx_order` … is not user-exposed": **there is no drag-reorder UI at all**.

## Bug B: exact location + suggested fix

`Source/dsp/fx/FxChain.cpp:854-857` computes the *mean* dry/wet `W = Σ(dwCurₐ·fadeₐ)/activeCount`, then `:908` `outL[i] = dL*dry + sumWL*inv*W` scales the **summed raw wets** by that shared mean — per-slot dw never scales its own branch's wet. Affects both `Parallel12to3` (:685) and `Parallel1to23` (:734).

**Fix:** apply each slot's own effective dw to its wet inside the sum loop (`:883-885`), and use the un-normalised sum:

```cpp
const float dwA = actDwCur[a] * actFade[a];       // per-slot, computed in the dwSum loop
sumWL += wl * dwA;  sumWR += wr * dwA;            // replaces: sumWL += wl;
...
outL[i] = dL * dry + sumWL * inv;                 // replaces: ... + sumWL * inv * W
```

Keep `W` (mean) for the dry gain `dry = 1 − W` (unchanged equal-gain character at dw=1). Result: branch dw=0 → truly silent; each branch's level becomes invariant to the *other* branch's dw (matching series semantics); bypass/fade transitions (activeCount exit) unchanged.

## Recommendation

1. **Fix B** above (bug; severity: medium-high — a knob at 0 that still passes feedback echoes).
2. **UX note for A** (no code change): tooltip/help text on `fx{N}_drywet`: "Dry/wet is per-slot. A delay placed *before* a reverb/echo is still sustained by that module's tail for seconds after you dry it out — move it later in the chain for an immediate cut."
3. Series path, master-mix ring (N1) and dry-delay ring (D1) verified clean by tests 1/3.

*Probe source: temporary `tests/_tmp_drywet_probe.cpp` + CMake target, both removed after measurement.*

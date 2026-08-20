# FX DSP Review — Tail Cutoff, Popping & Other Audio Artifacts

**Scope:** `Source/dsp/fx/*`, the FX render path in `SynthEngine::renderPartFx`,
the master-section commit (`6454549`), and the UI engagement path
(`FxSlotCard`).
**Complaint:** FX cut off tails, pop, and exhibit other artifacts.
**Prior review (`FX_REVIEW.md`):** APPROVE / 0 blockers — but it verified
threading, serialization, and topology *correctness* only. **No test checks
audio continuity** (smoothing, click-free bypass, tail decay). The two FX tests
(`fx_preset_test`, `fx_routing_test`) assert only finiteness and "output differs
from dry." Every bug below is invisible to that surface.

---

## TL;DR — the single root cause

**The entire FX path applies every parameter — dry/wet, master mix, every effect
param, every enable/disable, every FX-mod-matrix value — as a per-block constant
with zero smoothing, and the default bypass mode hard-cuts.** Every state
transition therefore produces a step discontinuity at a block boundary (a click),
and bypass / type-change / preset-load instantly destroys reverb & delay tails.

There is **no smoothing primitive anywhere in `Source/dsp/fx/`** (the only
`smooth`/`Smoothing` hits are the unrelated synth-voice `setParameterSmoothing`
in `PluginProcessor.cpp` / `SynthEngine.cpp`, which the FX chain never touches).
The lone "fade" that exists — `wetFade_` — is **binary by default** and only
fades *out*, never *in*.

---

## The smoking gun (default config)

`fx_keep_tails` defaults to **0** (`ParameterLayout.cpp:510`). With
`keepTails_ == false`, `FxChain::updateTailState` makes `wetFade_` a hard binary:

```cpp
// FxChain.cpp:144-152
if (! keepTails_) {
    for (int s = 0; s < kNumFxSlots; ++s) {
        wetFade_[(size_t) s] = enabled_[(size_t) s] ? 1.0f : 0.0f;  // binary
        ...
    }
    return;
}
```

and the series blend is:

```cpp
// FxChain.cpp:309
const float dw  = dryWet_[(size_t) s] * wetFade_[(size_t) s];  // → 0 instantly on bypass
const float dry = 1.0f - dw;
outL[i] = dryL_[i] * dry + outL[i] * dw;
```

So the instant you bypass a slot, `dw` collapses to 0 and the wet path —
including whatever reverb tail / delay feedback was ringing — vanishes in a
**single sample**. That is simultaneously the tail-truncation and the click.

---

## Bugs, ranked

### B1 — CRITICAL: Hard-cut bypass is the default → tail truncation + click on bypass
**Where:** `FxChain.cpp:144-152` (binary `wetFade_`), `ParameterLayout.cpp:510`
(`fx_keep_tails` default 0).
**Mechanism:** Described above. `wetFade_` is 1.0 while enabled, 0.0 the moment
it's not. The wet signal is multiplied to zero in one sample.
**Trigger:** Clicking a slot's power/bypass toggle; loading a preset that
disables a slot; any `setFxSlotEnabled(slot, 0)`.
**Symptom:** Reverb/delay tail cut dead + an audible click proportional to the
energy in the tail at the cut instant.
**Note:** The `keepTails_` *feature* that would prevent this is off by default,
undocumented as a default-off safety, and most users will never discover it.

### B2 — CRITICAL: Zero smoothing on every FX parameter → zipper noise / clicks
**Where:** All of `FxProcessors.cpp` and the `dw`/`dry` blends in `FxChain.cpp`
(309, 341, 374, 439).
**Mechanism:** Every effect stores a bare `float` target updated once per block
in `setParams()` (called every block from `renderPartFx`), then multiplies it as
a constant across the whole block:
- `FxGainPan::process` — `L[i] *= gainLinear_ * panLGain_` (`FxProcessors.cpp:21-22`), targets set in `setParams` (`:26-34`). A gain/pan knob move = a step in the per-sample multiplier.
- `FxDelay` — `feedback_`, `timeSec_`, `spreadFrac_` (`FxProcessors.cpp:68-84`, `:87-91`). A feedback knob move = step in the loop gain; a time move = an unsmoothed delay-length jump (also a read-pointer discontinuity → pitch glitch/click).
- `FxReverb` / `FxChorus` — params re-applied via `dirty_` every block (`FxProcessors.cpp:130-138`, `:183-190`).
- Per-slot `dryWet` blend (`FxChain.cpp:309,341,374`) and global `masterMix_` (`FxChain.cpp:399-406`) are block constants.

**Trigger:** Turning any FX knob; any FX-mod-matrix modulation (see B5); any
parameter automation.
**Symptom:** Zipper noise on slow moves, outright clicks on fast moves. A gain
knob sweep audibly "staircases." Reverb size changes click.

### B3 — HIGH: Enable has no fade-in, even with Keep Tails on (asymmetric)
**Where:** `FxChain::updateTailState`, the enabled branch (`FxChain.cpp:165-167`):
```cpp
if (en) { wetFade_[(size_t) s] = 1.0f; }   // instant, no ramp
```
**Mechanism:** `keepTails_` only implements the *fade-out* half of a bypass
transition. Engaging a slot snaps `wetFade_` 0→1 in one sample. The freshly-engaged
processor (reverb/chorus freshly ringing, or delay with stale `lastWet_`) is
mixed in at full `dw` instantly.
**Trigger:** Clicking a slot's power toggle to *enable*; loading a preset that
enables a slot.
**Symptom:** Click on engage. This defeats the purpose of "keep tails" — you get
click-free bypass-off but a click on bypass-on.

### B4 — HIGH: Type change destroys the processor and resets to zero, blended at full dw
**Where:** `FxChain::setSlotType` (`FxChain.cpp:46-58`):
```cpp
slots_[(size_t) slot].reset();                       // destroy old (tail gone)
if (t != FxType::None ...) {
    slots_[slot] = createFxProcessor (t);            // allocate + prepare
    slots_[slot]->prepare(...); slots_[slot]->reset(); // zero state
}
```
combined with the engagement-default writes in `FxSlotCard::parameterChanged`
(`FxSlotCard.cpp:365-376`): selecting a type **simultaneously** writes
`enabled`, `drywet`, and `param1..4`.
**Mechanism:** On a type change four things happen at the next serviced block:
(1) old processor's tail is destroyed, (2) new processor starts from zero state,
(3) `enabled` jumps 0→1 (B3, no fade-in), (4) `drywet` jumps 0→~0.63 and all four
params jump to per-type defaults — all unsmoothed (B2).
**Trigger:** Picking a new effect from the Type dropdown.
**Symptom:** Old effect's tail cut + a compound click as the new effect slams in
at full wetness. The "audible per-type defaults" feature is deliberately
designed to slam in (commit `6454549`), so this is hit on the most common UI
action.

### B5 — MEDIUM: FX mod matrix is evaluated at block rate → staircase modulation
**Where:** `SynthEngine::renderPartFx`, the whole step "4" (`SynthEngine.cpp` ~1252-1290).
**Mechanism:** `effDryWet`/`effParam` are computed once per host block from
`modOffset[dest] += amount/63 * srcValue/255`, where `srcValue` is a single
per-block snapshot of the first active voice's mod source. The result is pushed
to the chain as a block constant. An LFO/envelope routed to dry/wet, feedback,
chorus rate, etc. therefore modulates in discrete per-block steps, not
continuously.
**Trigger:** Any active FX mod-matrix row whose source moves within a block
(LFO, env, wheel, velocity, aftertouch).
**Symptom:** Zipper/clicking on the modulated parameter; on time-based effects
(delay/chorus) the stepped control reads as pitch jitter.

### B6 — MEDIUM: `masterMix_` is a block constant → zipper on global mix knob
**Where:** `FxChain.cpp:399-406`.
**Trigger:** Moving the FX Mix knob or automating `fx_mix`.
**Symptom:** Zipper noise on the global wet/dry crossfade. Same class as B2 but
applies to the whole-chain blend.

### B7 — MEDIUM: `prepare()` clears delay/chorus state and zeroes all tail fades
**Where:** `FxChain::prepare` (`FxChain.cpp:25-43`) re-`prepare`s every processor
(juce `DelayLine::prepare` and `Chorus::prepare` reallocate/reset their buffers)
and unconditionally does `wetFade_.fill(0.0f); prevEnabled_.fill(false);`.
**Trigger:** Host sample-rate or buffer-size change mid-session (some DAWs do
this on device changes, plugin-bypass toggles in certain hosts, or when the
device control panel is touched).
**Symptom:** Delay and chorus tails cut instantly; any slot that was mid-tail is
reset to "not tailing." The reverb tank survives `prepare` (juce `Reverb::prepare`
doesn't clear the combs), but its `wetFade_` is zeroed, so it is silent until the
next `enabled` transition re-arms it — i.e. a playing reverb can go silent until
you toggle it.

### B8 — LOW: Reverb's internal `wetLevel` (param2) compounds with the chain dry/wet
**Where:** `FxReverb::setParams` sets `p.wetLevel = param[2]`, `p.dryLevel = 0`
(`FxProcessors.cpp:131-137`); the chain then blends `dry*(1-dw) + reverbOut*dw`.
**Effect:** Effective wetness = `wetLevel * dw`. Two controls multiply one
perceived quantity, full-wet is unreachable (max ≈ `wetLevel`), and the reverb
"Level" knob reads as a second wetness control nested inside the slot Mix —
confusing and quieter than expected. (Acknowledged as nit #4 in `FX_REVIEW.md`
but it is a real audible "why is my reverb quiet" wart.)

### B9 — LOW: Engagement defaults publish a torn-ish frame (5 separate `fxDirty_` writes)
**Where:** `FxSlotCard.cpp:365-376` writes `enabled`, `drywet`, `param1..4` as
five separate `getParameterAsValue` assignments, each → `applyFxParameter` → a
relaxed store + `fxDirty_` release. The AT services whichever frame is current at
each block, so a type-select can be observed mid-write (e.g. enabled=1 but
drywet still 0) for one block. End state is consistent; the transient is
one block of wrong-ish wetness. (Same class as `FX_REVIEW.md` nit #7.)

### B10 — LOW (design): mono-in duplicated to L+R defeats true stereo effects
**Where:** `SynthEngine.cpp` `chain.process (mono, mono, outL, outR, numSamples)`.
Both delay lines, both chorus taps, and the reverb input receive **identical** L
and R. The Delay "Spread" and Chorus only differ by delay *time*, not content, so
the stereo image is narrower than the params imply. Stated as intended
("mono-in / stereo-out"), but worth flagging since "Spread"/"Width" imply more
than they deliver.

---

## Why the first review missed all of this

`FX_REVIEW.md` was a *correctness* review (threading, serialization, topology
math, CONTRACT faithfulness) and it is accurate on those axes. But:

- Its "What's verified & sound" section praises the per-block constant dry/wet
  math as "sane" without considering **smoothness across blocks**.
- It explicitly lists the keep-tails/reverb-wetness items as *cosmetic nits*
  (#3, #4) rather than the click/tail sources they actually are.
- The test suite asserts `allFinite()` and `differsFrom(dry)` — both pass on a
  clicking, tail-truncating chain. **No test renders a bypass, an engage, a knob
  sweep, or a mod sweep and bounds the sample-to-sample delta.**

---

## Recommended fixes (priority order)

1. **Always smooth `wetFade_`, sample-accurately, both directions.** Stop making
   it binary. `keepTails_` should control the *decay time* (e.g. 5 ms vs 40 ms),
   not whether fading happens at all. Apply the fade per-sample inside the blend
   loop, not as a block multiplier. This kills B1, B3, and the enable/bypass half
   of B4 in one change.
2. **Add one-pole smoothers for `dryWet`, `masterMix_`, and every effect's
   primary param** (GainPan gain/pan, Delay feedback/time/spread, Reverb
   size/damp/level/width, Chorus rate/depth). Drive them from `setParams`/`setSlot*`,
   read them per-sample in `process()`. This kills B2 and B6 and softens B5/B9.
   (The codebase already has a `setParameterSmoothing` pattern for voices — FX
   should smooth unconditionally, not behind an option.)
3. **Crossfade on type change.** Double-buffer: keep the old processor alive ~5 ms
   fading out while the new one fades in, then drop the old. Eliminates the
   tail-cut and the zero-state slam of B4. At minimum, ramp `dw` to 0, swap,
   ramp back up.
4. **Evaluate the FX mod matrix per-sample (or per sub-block) for smoothed
   targets** so LFO/env-driven FX params don't staircase (B5).
5. **Make `prepare()` non-destructive to tails** (B7): don't zero `wetFade_`/
   `prevEnabled_` on a re-prepare; only recompute EQ coeffs + resize scratch.
   Processor `prepare` is unavoidable but should be paired with a state-preserve
   where possible.
6. **Flip `fx_keep_tails` default to ON** (or render it obsolete via fix #1).
7. **Collapse the engagement-default writes** into a single staged frame (one
   `fxDirty_` publish) (B9), and decide whether Reverb `param2` should be an
   internal level or just delegate wetness to the slot Mix (B8).
8. **Add an audio-continuity test:** render a steady tone through each FX, then
   mid-stream (a) bypass, (b) engage, (c) change type, (d) sweep dry/wet, (e)
   sweep an FX-mod row; assert the max |Δsample| stays below an audible threshold
   (e.g. 0.01) except where the DSP legitimately discontinues (and assert a tail
   is *present* N ms after note-off / bypass before falling below threshold).

---

## Verdict

The FX feature is thread-safe and structurally correct, but **it is not
click-free and it does not preserve tails by default.** B1 + B2 are the bugs the
user is hearing; B3/B4 are the next most audible; B5–B10 compound them. None were
caught because the test surface only checks "is it finite / did it run." The
fixes are localized to `Source/dsp/fx/` (+ sample-level evaluation in
`renderPartFx`) and are low-risk relative to the audible payoff.

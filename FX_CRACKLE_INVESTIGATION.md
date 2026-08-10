# Clouds FX Crackle — Root Cause & Fix (final)

**Symptom:** "When playing the synth the clouds-related FX modules crackle — a
short discontinuity within the FX pipeline. No modulation. Not a CPU dropout
(Ableton Live + the internal probe both showed no xruns)."
**Root cause:** **`HostRateBridge` zeroed its output on 1-sample blocks** (`m==0`),
and `SynthEngine::renderPartFx` periodically feeds it 1-sample sub-chunks.
**Fix:** hold the last processed sample instead of zeroing. **Status:** fixed +
regression test added; all FX tests pass.

---

## The bug (one line)

`Source/dsp/fx/HostRateBridge.h`, `internalToHost()`, the `m <= 0` branch:
```cpp
if (m <= 0) { for (int i = 0; i < n; ++i) { L[i] = 0.0f; R[i] = 0.0f; } return; }   // <-- ZEROES the output
```
When the bridge is called with a **1-sample block** and the phase carry ≥ 1,
`hostToInternal(1)` produces **`m = 0`** internal samples (no internal boundary
crossed), and this branch then **writes a 0** to that host sample — a
**full-amplitude dropout**.

## Why a 1-sample block reaches the bridge

`SynthEngine::renderPartFx` sub-chunks each host block at the synth's ~980 Hz
internal-block cadence with a **drift-corrected fractional phase**:
```cpp
double nextBoundary = fxSubPhase_[p];
int sub = (int)(nextBoundary - (double)written);
if (sub <= 0) sub = 1;                       // <-- 1-sample sub-chunk
...
chain.process(mono + written, ..., sub);     // each Clouds FX's bridge gets `sub`
```
As the carry `fxSubPhase_` cycles, `sub` lands on **1** periodically (the
`if (sub <= 0) sub = 1` guard). That 1-sample sub-chunk is passed straight into
each Clouds FX's `HostRateBridge`, hitting the `m == 0` zero branch → **a click**.
The cadence of the clicks tracks the drift cycle (sparse — a few per second per
FX), which is exactly the reported "short discontinuity."

## Evidence (full-engine reproduction — the real `processBlock` path)

A dedicated harness rendered a sustained note through the actual
`ParvatiAudioProcessor` with each Clouds FX enabled (no modulation), then counted
curvature-immune impulses (HP outlier vs a 64-sample window) on the main bus:

| FX (48 k) | impulses | worst Δ | → after fix |
|---|---|---|---|
| Diffuser  | 6  | **0.124** (42 % of peak) | **0** ✓ |
| Reverb    | 2  | **0.193** | **0** ✓ |
| Spectral  | 10 | 0.082 | **0** ✓ |
| LoopingDelay | 0 | — | 0 ✓ |
| WSOLA     | 652 | 0.120 | 648, **0.037** (remaining = intrinsic splice grain) |
| PitchShifter | 308 | 0.017 | 308, 0.017 (intrinsic dual-tap crossfade grain) |

Direct bridge probe (`n = 1` blocks, 1 s tone): **`m == 0` in 16 000 of 48 000
calls, zeroing 16 001 output samples, worst Δ = 0.30 (full signal amplitude).**

Key point: the per-FX DSP is clean — isolated `fx->process()` tests showed **0**
impulses. The discontinuity only appears through the **full engine path**, which
is why every earlier headless/isolated test missed it. The `m == 0` zero branch
was the single source of the loud clicks.

## The fix

`internalToHost()`, `m <= 0` branch now **holds the last processed internal
sample** instead of zeroing:
```cpp
if (m <= 0)
{
    // No internal sample was produced this call (a 1-sample block whose phase
    // carry didn't cross an internal boundary — renderPartFx's drift-corrected
    // sub-chunking emits these). Zeroing would be a full-amplitude dropout; hold
    // the last processed internal sample (zero-order hold) so the output is
    // continuous.
    const float hl = hasTail_ ? prevTail_.l : 0.0f;
    const float hr = hasTail_ ? prevTail_.r : 0.0f;
    for (int i = 0; i < n; ++i) { L[i] = hl; R[i] = hr; }
    return;
}
```
Localized to `HostRateBridge.h`; no change to the drift, the engine rate, or any
FX DSP. The hold introduces at most a 1-sample zero-order-hold error on the rare
`m == 0` block — inaudible vs the 0.12–0.19 dropout it replaces.

## Regression tests (two layers)

1. **`tests/parvati_fx_bridge_tinychunk.cpp`** (`parvati_fx_bridge_tinychunk_test`,
   built by default) — drives the bridge directly with 1-sample blocks and a
   drift-like `{49,49,49,1}` pattern; asserts no zeroed outputs and a bounded Δ.
   Passes with the fix (Δ 0.014), fails without it (Δ 0.300, 104 zeros). Guards
   the bridge's `m==0` branch specifically.

2. **`tests/parvati_fx_engine_continuity.cpp`** (`parvati_fx_engine_continuity_test`,
   built by default) — the layer that actually *found* the bug. Renders a
   sustained note through the real `ParvatiAudioProcessor::processBlock` (the
   only path that generates renderPartFx's drift-induced 1-sample sub-chunks)
   with each Clouds FX enabled, no modulation, and asserts the main bus is
   click-free (worst curvature-immune impulse < 0.06). Covers 48 k @ buffers
   128/256 and 44.1 k @ 256, all six Clouds FX. Passes with the fix (smooth FX =
   0.000; PitchShifter/WSOLA intrinsic grain ≤ 0.043); **fails 14 of 18 configs
   without it** (worst impulses 0.08–0.44). This is the test the isolated
   `fx->process` suite structurally cannot be — it catches orchestration-level
   regressions, not just per-FX ones.

The threshold (0.06) sits in the gap between the modes' intrinsic grain
(PitchShifter/WSOLA ~0.017–0.043) and the m==0 dropout bug (≥0.082).

## What was ruled out (briefly)

xruns / CPU (probe + Ableton DSP meter clean) · GUI render load (no xruns) ·
per-FX DSP clicks (isolated `fx->process` clean) · bridge boundary discontinuity
(seamless at −57 dB for `n ≥ 2`) · 12-bit reverb quantization · aliasing ·
modulation stepping. The earlier "dulling/comb" notes (linear-interp passband
loss, uncompensated bridge delay) remain valid *quality* observations but were
**not** the crackle.

## Remaining (not this bug)

PitchShifter (≈308 small impulses, Δ ≈ 0.017) and WSOLA (≈648, Δ ≈ 0.037) still
show small artifacts — these are those modes' **intrinsic granular character**
(dual-tap crossfade / WSOLA window splice), at ~1.7–3.7 % of full scale. Separate
from the m==0 dropout; can be revisited if still audible after this fix.

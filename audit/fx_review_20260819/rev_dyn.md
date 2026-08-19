All assigned files, the shared framework (`Fv1Engine.h`, `Fv1FxProcessor.h`), the tail table (`FxTypes.h`), the chain's latency/dry-wet alignment (`FxChain.cpp`), the vendored Warps SRC, and the existing tests have been read; all arithmetic below was recomputed, not eyeballed. Report follows.

# FX DSP Review — batch "Dynamics + distortion (FV-1)" — 2026-08-19

Scope: `Fv1Compressor`, `Fv1VinylCompressor`, `Fv1Gate`, `Fv1Overdrive`, `Fv1LutDistortion` (+ shared framework only as evidence). Read-only; no files edited.

---

## 1. Fv1Compressor — VERDICT: FINDINGS (1 minor bug, notes)

- [BUG-minor] `Fv1Compressor.cpp:26` — `level14_ = q14 (p3 * 2.0f)` vs documented "Level (p3): 0..2 output trim" (`Fv1Compressor.h:17`). `q14()` (Fv1Engine.h) clamps `c >= 1.0f` to 8191 (≈0.99988), so every `p3 > ~0.50006` maps to the same ~1.0 gain: **the upper half of the Level knob is dead**. Fix: reuse the ki/kf decomposition already used for `g` two lines below, or correct the spec to 0..1.
- [OK-PINNED] Gain path arithmetic: `g` clamped to [0, 3.5] → `ki ≤ 3`, saturating adds + `f24_sat` — no overflow. p0=0 ⇒ `th_ = 1.0` while `|xf| ≤ (2^23−1)/2^23 = 0.99999988 < 1.0` ⇒ compressor never engages (transparent) — verified. `pow(th_/env_, e)` with `env_ > th_ > 0` — no NaN/Inf path; `env_` is a convex update of non-negatives.
- [OK-PINNED] Attack/release computed at the fixed 32768 Hz internal rate and applied per internal sample — correct under any host rate; no division by zero (atkMs ≥ 0.5 ms, relMs ≥ 20 ms).
- [NIT] `env_` (float one-pole, release a ≥ 1.5e-3) decays through the subnormal range for ~10³–10⁴ samples after silence — perf-only (no flush), same family-wide pattern as the biquads which *do* flush.
- [RISK] Stepped `setParams` (makeup 1→3, threshold) change output gain block-instantly; no per-sample coefficient glide. Consistent with the rest of the family; only matters for host-side stepped automation.

**Test suggestions:** pin `p3` monotonicity above 0.5 (would fail today); pin p0=0 bit-transparency (currently only "loud reduced/quiet pushed" is pinned in `tests/fv1_newfamily_test.cpp:233-256`).

---

## 2. Fv1VinylCompressor — VERDICT: CLEAN (notes)

- [OK-PINNED] Delay bounds: `readDelay = 1638 + 300·sin + 24·sin ≤ 1962`; `readFrac` reads `di` and `di+1` ⇒ max index 1963 < 2048 ring (and `write`/`read` never alias). Wow pitch math verified: 2π·0.4·300/32768 = 2.30%; flutter 2π·3.1·24/32768 = 1.43% — matches comments.
- [OK-PINNED] Saturation math: `xKnee = 1/√(3a)`, a ∈ [0.06, 0.31] ⇒ xKnee ∈ [1.037, 2.36] and `satClampQ_ = xClamp·2^23 > kMaxQ23`, so the pre-cubic clamp never binds on Q.23 input (comp is already saturated) — harmless; `yMax = (2/3)·xKnee` peak math correct; all muls saturating, no overflow.
- [OK-PINNED] Noise: `|tickEnv_ + hiss·0.002·p| ≤ 0.182` < 1 — matches the "~0.18 ceiling, never full-scale" contract; LCG usage safe.
- [OK-PINNED] Lifecycle: `resetInternal()` clears delay/age-LP/env/phases/LCG/tick and re-applies the cutoff from cached `pAge_`; `prepareInternal` → `resetInternal`; re-prepare at a new rate needs nothing rate-dependent. Tail: `tailSecondsForFx(VinylCompressor) = 0`, but `clampTailSeconds` floors at 0.2 s (`FxTypes.h:219,322`) ≥ the 50 ms wet delay, so bounces are not truncated.
- [NIT] The crackle/hiss floor persists indefinitely on silent input (pinned by `tests/fv1_vinyl_compressor_test.cpp`); with a 0 s declared tail a host cut drops only a ≤ −54 dB noise floor — acceptable.
- [RISK] Stepped param changes (makeup 1→6, `satA14_`, LP cutoff) apply per block with no glide — amplitude/waveform steps on fast automation; family-wide pattern.

**Test suggestions:** pin wow depth at a mid Wow setting (ratio vs 300-sample spec); pin delay-ring clear on reset→re-run (no stale audio leaks).

---

## 3. Fv1Gate — VERDICT: CLEAN (notes)

- [OK-PINNED] Threshold=0 disable: `th_ = p0·0.7` and the `th_ <= 0.0f || env_ > th_` short-circuit keeps the gate open; pinned by `tests/fv1_newfamily_test.cpp:257-276`. Hold state machine: hold re-arms on every above-threshold sample, decrements once/sample during dips, then closes — monotone and correct; `holdSamp_ = p2·0.150·32768` (0..4915.2) fine as float.
- [OK-PINNED] Clickless: the open/close decision is instantaneous but the applied gain rides a one-pole (`gain_ += a·(target − gain_)`), min attack 0.05 ms ≈ 1.6 internal samples (a = 1−e^(−0.61) = 0.457) — continuous, no step. `q14(clamp(gain_,0,1))` bounds the fixed-point multiply.
- [NIT] The 0.02 detector coefficient (~2.7 ms) is independent of the Attack knob (attack only shapes the gain one-pole) — as documented in the comment, just worth knowing.
- [NIT] `env_`/`gain_` decay through float subnormals on long closures (closeA down to 6.1e-3) — perf-only.
- [OK-PINNED] No delay lines; reset clears all state; rate-independent coefficients (fixed 32768 Hz). Tail 0 is correct (a gate's "tail" is silence).

**Test suggestions:** pin hold-window behavior (dip shorter than Hold must not re-trigger the release slope); pin re-arm after hold expiry.

---

## 4. Fv1Overdrive — VERDICT: FINDINGS

- [BUG] Table-index calibration, `Fv1Overdrive.cpp:133` — `idx = (v >> 13) + 512 + biasIdx_`. With Q.23 input, `v>>13 = 1024·x`, and the table maps `xT = (idx−512)/128`, so the table is read at **xT = 8·D·x** (D = drive), not `D·x`. Consequences, computed: at Drive=1 (D=1) the small-signal gain is `0.72 · f′(0) · 8 = 0.72·1.35·8 = 7.78×` (+17.8 dB) — directly contradicting "Unity slope near 0 keeps low-Drive transparent" (`Fv1Overdrive.cpp:22`); the documented "Drive 1..16x" is effectively 8..128×. A `>>16` shift maps input 1:1 onto the domain (xT = D·x). Related: `biasIdx_ = ±77 idx = ±0.602 domain units` (`Fv1Overdrive.cpp:61`) vs the "±0.3" comment (×2 off). Fix: change to `>>16` + bias `±0.3·128 = ±38` (changes the tuned sound — owner decision) **or** fix the docs. `Fv1LutDistortion` has the identical issue (below).
- [BUG] Non-monotonic curve / wrong comments, `Fv1Overdrive.cpp:29-38` — positive half `1.35x/(1+0.42x²)` peaks at 0.7715 (x = 1.543/1.35 = 1.143) then **droops to 0.4076** at x=4; "saturates ~1.6" is false and `std::min(y, 1.35f)` (:31) is dead code (max 0.7715). Negative half clamps flat at −1 for |x| > 0.756 (the `max(y,−1.55)` at :38 is dead; peak −1.4149). Net effect at high drive: positive peaks ≈ 0.29, negative ≈ −0.72 ⇒ ~ −0.21 DC and *quieter at max Drive than mid Drive*. Fix approach: monotone saturating curve (e.g. tanh-family) or accept and document; consider a DC block.
- [RISK] Bias = DC generator: at full bias and **zero input**, output reads the curve at xT = ∓0.602 ⇒ DC ≈ ∓0.61 (0.72·0.841). Tone LP passes DC; enabling the slot at nonzero Bias thumps. Reduce range or DC-block.
- [BUG-minor] `Fv1Overdrive.cpp:63` — same `q14(p3·2)` Level dead zone as the Compressor: p3 > ~0.5 pins Level to ≈1.0 vs documented "0..2".
- [BUG-med] Latency: the 6x OS pair has group delay `SRC_UP::delay() = 48/6/2 = 4` + `SRC_DOWN::delay() = 48/2 = 24` at 6x = 4 ⇒ **8 internal samples ≈ 11.7 host samples @48 kHz**, but `latency()` inherits 0 (`Fv1FxProcessor.h:64`). The `FxProcessor.h:53-58` contract says oversampled FX report the SRC group delay (Wavefolder/RingModulator do; `FxChain.cpp:367-371` delays dry by `latency()` for comb-free dry/wet). With 0, a 50/50 mix combs (first notch ≈ 2.0 kHz @48k). Fix: capture `round(8·hostRate/32768)` in `prepareInternal` and return it.
- [OK-PINNED] OS plumbing: up-stage emits exactly 6·m; `SRC_DOWN` requires a multiple of 6 — satisfied by construction; `m==0` guarded and the bridge ZOHs. Scratch sizing `worst·6+8` vs the bridge's `maxM_` (`ceil(maxBlock·32768/rate)+4` in double vs float): a ceil disagreement is bounded by 1 ⇒ 6 extra floats, inside the +8 slack. Reset re-Inits the SRCs; no delay lines to clear.

**Test suggestions:** pin Drive=1 small-signal gain ≈ 1 (fails today, documents the calibration); pin Level monotonicity above center; pin `latency() > 0` for the OS pair; pin max-Drive ≥ mid-Drive output level.

---

## 5. Fv1LutDistortion — VERDICT: FINDINGS

- [BUG] Same index calibration, `Fv1LutDistortion.cpp:227` — `idx = (v >> 13) + 512` reads the tables at xT = 8·D·x; documented "Drive 1..8x" (`.h:15`) is effectively 8..64× (e.g. shape Soft: small-signal gain `0.75·1.5·8 = 9.0×` = +19 dB at minimum Drive; Clip engages above |x| = 0.078 at Drive=1). Fix as for Overdrive (`>>16`) or re-document.
- [BUG-minor] Mono-average saturation, `Fv1LutDistortion.cpp:245` — `f24_addSat(lutShape(lin), lutShape(rin)) / 2`: the **sum saturates before the halving**, so with L==R (mono material, and every existing test) any curve output > 0.5 is hard-capped at 0.49999988 with clip distortion. Table max is 0.75, so −3.5 dB of headroom is lost and a clip knee rides the output. Fix: `f24_addSat(f24_mulk(yL, 4096), f24_mulk(yR, 4096))` (average before saturation). The trailing `f24_sat` is then/already dead code [NIT]. Note `tests/fv1_newfamily_test.cpp:160-172` explicitly rationalizes this knee as a "smooth morph artifact" — it is a real gain-path defect.
- [RISK] Shape DC at zero input (post-0.75 scaling, `Fv1LutDistortion.cpp:41,58`): Cheby2 = 0.75·(−0.95) = **−0.71 DC**, OctUp = 0.75·(−0.45) = **−0.34 DC**, Asym (t=−0.15) ≈ −0.16. Selecting these shapes puts a large sustained DC on the wet bus; Tone LP passes DC; nothing removes it. Fix: DC-block the output or re-reference shapes to 0 at x=0.
- [RISK] Mid-fade re-target, `Fv1LutDistortion.cpp:114-121` — a shape change while a fade is in flight replaces `fadeFrom_` with the *current target* and resets `fade14_ = 0`; the effective curve jumps from the blend (e.g. 50/50) to 100% of the old target — a step up to (1−f)·|A−B| (curves differ by up to ~1.3) ⇒ click on rapid shape spinning/automation. Fix: keep a third "snapshot" approach or complete fades parametrically.
- [BUG-med] Latency 0 despite the same 6x OS group delay (8 internal ≈ 11.7 host @48k) — identical to the Overdrive finding; see §4.
- [OK-PINNED] Jitter: `jitLp_` is a convex one-pole of noise ∈ [−1,1) ⇒ readPos ∈ [−11, 13], clamped ≥ 1; `readFrac` on a 64 ring — in bounds; Jitter=0 = `readFrac(1.0)` = the just-written sample (identity). Fade clock: `kFadeStep14 = 8191/128+1 = 64`; 128 steps × 64 = 8192 ≥ 8191 ⇒ terminates in exactly 128 internal samples (3.9 ms) on the 1x timeline; fade advances only when m > 0 (stalls safely). `resetInternal` clears the fade and rings. Crossfade gains sum to 8191/8192 (−0.001 dB) — fine. All 16 table values ∈ [−12287, 12287] Q.14 ⇒ int16-safe; `×512` ⇒ ≤ 0.75 Q.23.

**Test suggestions:** pin L==R input ceiling ≠ 0.5 cap (fails today); pin DC of Cheby2/OctUp at silence < some bound (fails today); pin rapid shape-change slope excess (extends the existing single-switch metric at `tests/fv1_newfamily_test.cpp:173-233`); pin Drive=1 small-signal gain.

---

## Cross-cutting (rate bridge + vendored adapter)

- [OK-PINNED] All five effects correctly treat 32768 Hz as the fixed internal rate; host-rate variance is absorbed by `RateBridge` (not in scope, but verified where it interacts with these files: m==0 ZOH path, buffer sizing, in-place processing safety — `internalToHost` reads `il/ir` after the effect wrote them, no aliasing).
- [OK-PINNED] Vendored Warps `SampleRateConverter` (`Source/dsp/clouds/warps/dsp/sample_rate_converter.h`): `SRC_UP` accepts any size; `SRC_DOWN` requires a multiple of 6 — both call sites feed exactly 6·m. The vendored patch (fast path removed) specifically covers the variable-chunk usage pattern these two effects generate. `DISALLOW_COPY_AND_ASSIGN` makes the SRCs non-movable, so `Fv1Overdrive`/`Fv1LutDistortion` are non-movable — and the factory builds them via `make_unique` (`FxProcessors.cpp:810-813`), so the internal `x_ptr_` can never dangle via a copy/move. No algorithmic suspicion in the vendored code as used.

---
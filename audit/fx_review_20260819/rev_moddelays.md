All verification complete. Here is the review report.

# FX DSP Review — Modulated delay/phase family (FV-1)

Batch: `Source/dsp/fx/fv1/Fv1Chorus.{h,cpp}`, `Fv1Flanger.{h,cpp}`, `Fv1Ensemble.{h,cpp}`, `Fv1Phaser.{h,cpp}` (framework: `Fv1Engine.h`, `Fv1FxProcessor.h`; cross-refs: `FxTypes.h`, `FxChain.{h,cpp}`, `ui/FxSlotLabels.cpp`, `docs/FX_FV1_DESIGN.md`, `tests/fv1_{newfamily,phaser,ensemble}_test.cpp`). Read-only; no files edited.

## Shared framework findings (apply to all four)

- **[RISK] Tail-table mismatch (`FxTypes.h:300-313`)** — `tailSecondsForFx` returns `0.0` for Ensemble/Phaser/Chorus/Flanger, clamped to the 0.2 s floor (`FxTypes.h:219,322-323`). Actual feedback-loop t60 (`FxTypes.h` feedback-decay law, `tail_detail::feedbackTail`):
  - Ensemble: |q14(±0.9)| = 7372/8192 = 0.90039 → 65.8 passes × 25 ms loop = **1.65 s** (8.3× the reported 0.2 s).
  - Flanger: q14(0.92) = 7536/8192 = 0.9200 → 82.8 passes × 6 ms base = **0.50 s** (up to 0.87 s at the 10.5 ms sweep peak).
  - Chorus: q14(0.5) = 4096/8192 = 0.5000 → 9.97 passes × 25–31 ms = **0.25–0.31 s** (marginal).
  - Phaser: loop ≈ 6 allpass group delays ≈ 0.1–0.2 ms → t60 ≈ 10 ms; floor is fine.
  Consequence: hosts size offline bounces from `getTailLengthSeconds()`; high-Center/high-Feedback Ensemble and max-Feedback Flanger ring-outs get truncated. Fix approach: add `feedbackTail(T, g)` cases in `FxTypes.h` mirroring the setParams mappings (T from Center/Manual, g from Feedback).
- **[OK-PINNED] Rate assumptions** — LFO increments are computed against `kInternalRate` exactly (e.g. `Fv1Chorus.cpp:25`), so LFO Hz is host-rate-invariant. `RateBridge` uses a persistent fractional phase (`Fv1Engine.h:436` loop, guard `m < maxM_`) → drift-free; `m==0` falls to zero-order-hold (no dropout); internal buffers sized `ceil(maxBlock·ratio)+4`; `numSamples ≤ maxBlock` is a documented chain contract (`FxChain.h:139`). Param values are `jlimit`-clamped upstream (`FxChain.cpp:292`).
- **[NIT] Latency vs `FxProcessor::latency()`** — all four report 0 (inherited, `Fv1FxProcessor.h:60`); the 4 cascaded 15 kHz biquads add ≈60 µs (~3 samples @48k) wet-path group delay. Consistent family-wide; only relevant when an FV-1 wet is parallel-blended against a non-FV-1 wet (mild HF comb at partial mix).
- **[RISK] Stepped parameter changes** — raw params applied at the ~980 Hz cadence is a documented chain policy (`FxChain.h:273-287`), but for modulated delays a Center/Manual step of Δp jumps the read pointer by Δp·655 (Chorus), Δp·754 (Ensemble), Δp·192 (Flanger) samples instantly → wet waveform discontinuity (click), scaled by mix. Triggered by fast knob jumps or FX-mod-matrix modulation of Center/Manual/Depth. The chain comment itself flags jump-detect de-click as future work.
- Item 7 (vendored Clouds/Rings/Warps adapters): **not applicable** — none of these four wrap vendored cores.

## 1. Fv1Chorus — FINDINGS

- **[RISK] min-delay clamp at Center=min/Depth=max** — `Fv1Chorus.cpp:26-27`: center min = 5 ms = 163.84 samples, depth max = 6 ms = 196.608 → min read = 163.84 − 196.61 = **−32.8**, clamped to 1.0 by `DelayLine::readFrac` (`Fv1Engine.h:226`). Pinned whenever sin < (1−163.84)/196.61 = −0.8293 → **18.9%** of each LFO cycle at the 1-sample floor → flat-topped sweep. Fix approach: clamp `depthSamp_ ≤ centerSamp_ − 1` in `setParams`.
- **[NIT] Header/doc mismatch** — `Fv1Chorus.h:4-5` claims "two slightly detuned SIN LFOs (the canonical AN-0001 technique)", but both voices share one `inc_` (`Fv1Chorus.cpp:25,49-50`); implementation is equal-rate with a fixed 108° offset (`Fv1Chorus.cpp:42`), not detuned.
- **[OK-PINNED] Numerics** — max read 819.2 + 196.61 = 1015.8 (+1 interp) < 2048 ring; `static_assert` 2×2048 = 4096 ≤ 32768 (`Fv1Chorus.cpp:13`); feedback 0.5 exact; all adds/muls saturating (`f24_addSat`/`f24_mulk`); phase wrap via `floor` is exact; no float denormals in the fixed-point path.
- **[NIT] Lifecycle inconsistency** — no `prepareInternal` override, so delay contents survive a re-prepare (tail-preserving; matches chain intent `FxChain.cpp:88-92`), unlike Ensemble/Phaser which wipe. `resetInternal` clears both rings + phase (`Fv1Chorus.cpp:31-36`). Tail vs table: 0.25–0.31 s vs 0.2 s floor (shared finding, marginal).

**Test suggestions:** impulse tap position vs Center (5/15/25 ms); L≠R divergence with depth>0; modulation period vs Rate; finite + documented behavior at p2=0/p1=1 corner; re-prepare tail survival; process() with sub-maxBlock chunks.

## 2. Fv1Flanger — FINDINGS

- **[RISK] severe min-delay clamp** — `Fv1Flanger.cpp:27-28`: base min = 0.15 ms = 4.9152 samples, depth max = 4.5 ms = 147.456 → min dl = **−142.5** → pinned at 1.0 whenever sin < (1−4.9152)/147.456 = −0.02657 → **49.2% of every LFO cycle** at Manual=0/Depth=1 (`Fv1Flanger.cpp:44`). The sweep flat-tops at ~zero delay half the time (R channel likewise, opposite half); the jet collapses toward a near-through path. Fix approach: clamp `depthSamp_ ≤ baseSamp_ − 1` in `setParams` (keeps all documented ranges, only out-of-range combinations affected).
- **[OK-PINNED] loop stability at max feedback** — q14(0.92) = lround(7535.72) = 7536 → 0.9200 exact (`Fv1Flanger.cpp:29`); damper `a = 1−exp(−2π·8000/32768) = 0.78425` → 6424/8192, `(1−a)` → 1767; coefficient sum 8191/8192 (−1.2e-4 DC droop per pass, negligible). |g| < 1 at DC and above; all arithmetic saturating. Max read 344.1 (+1) < 1024 ring; static_assert OK (`Fv1Flanger.cpp:14`).
- **[RISK] tail mismatch** — 82.8 passes × 6 ms mean loop ≈ **0.50 s** t60 at Manual max/Feedback max vs 0.2 s reported (shared finding; worst-case sweep peak 10.5 ms → 0.87 s).
- **[OK-PINNED] lifecycle** — `resetInternal` clears line, damper, phase (`Fv1Flanger.cpp:33-38`); contents survive re-prepare (no `prepareInternal`); `damp_.setCutoff(8000)` re-derived per `setParams` — coefficients never used stale.

**Test suggestions:** pin the Manual→base-delay mapping (impulse tap at 0.15/3/6 ms); clamp-corner render (p2=0,p1=1) documenting the pinned fraction; feedback tail decay at max (pins the 0.5 s t60 table gap); 180° L/R offset check; rate range 0.05–3 Hz period measurement.

## 3. Fv1Ensemble — FINDINGS

- **[RISK] worst tail-table mismatch in the family** — `Fv1Ensemble.cpp:52,55`: center max 25 ms, fb = ±0.9 → q14 = ±7372 (0.90039) → 65.8 passes × 25 ms = **1.65 s** t60 vs 0.2 s reported by `FxTypes.h:300-313` + floor. High-Center/high-|Feedback| ring-outs get cut in host bounces ~8× early.
- **[RISK] min-delay clamp** — center min = 2 ms = 65.536 samples (`Fv1Ensemble.cpp:52`) vs depth max 15 ms = 491.52 (`:49`) → min read = **−426** → pinned at 1.0 for sin < −0.1313 → **45.8%** of the cycle at Center=0/Depth=1 (`:69-70`). Arguably authentic BBD behavior (real ensembles cannot go through zero), but undocumented; same fix option as Chorus.
- **[NIT] `prepareInternal` wipes the rings** (`Fv1Ensemble.cpp:20-25`) — truncates the tail on a mid-session host rate/block re-prepare, which the chain explicitly tries to avoid (B7 comment, `FxChain.cpp:84-92` preserves fade/tail state); Chorus/Flanger don't wipe. Harmless in practice (RateBridge re-inits anyway) but inconsistent.
- **[OK-PINNED] numerics & mapping** — max read 819.2 + 491.52 = 1310.7 (+1) < 2048 (matches header comment); 2×2048 = 4096 ≤ 32768 (`:17`); param mapping matches `docs/FX_FV1_DESIGN.md` §2 and `FxSlotLabels.cpp:113-118` exactly (Rate/Depth/Center/Feedback); `phaseB_` is derived each sample (`:82`) and consistent with the 0.25 reset; both lines fed `lin` (mono-in by chain design); saturating fixed-point throughout; no DC trap (|fb| < 1).

**Test suggestions:** measure the impulse-train decay envelope at Center=max/Feedback=max (pins ~1.6 s t60 and the table gap); min-center/max-depth corner; 90° L/R phase-offset pin (cross-correlation of L vs R reads); re-prepare tail survival; negative-feedback (p3=0) odd/even comb character.

## 4. Fv1Phaser — CLEAN (nits only)

- **[NIT] allpass coefficient approximation** — `Fv1Phaser.cpp:81-82`: c = (1−x)/(1+x) with x = π·fc/32768 is the bilinear 1st-order AP form with x standing in for tan(x); the realized corner is ω = 2·atan(x), i.e. fc=7000 → **6163 Hz (−12%)**, fc=3500 → 3380 Hz (−3.4%), <1% below ~1 kHz. Deliberate (comment says "small-angle tan approx"), but the top-of-sweep notches sit noticeably below the Center+Depth arithmetic.
- **[NIT] bottom clamp engages over most of the sweep at wide depth** — `Fv1Phaser.cpp:79-80` clamps fc to [50, 7000]; at Center=200 Hz (`:44`, p3=0) with Depth=1500 Hz (`:42`, p1=1), the target sweeps to −1300 Hz → pinned at 50 Hz whenever tri < −0.1 → **45%** of the cycle bottom-parked. Typical phaser behavior; flagging for awareness.
- **[OK-PINNED] feedback stability** — six unity-gain allpasses, shared coefficient quantized via `setCoef` (clamps ±0.999, q14); |fb14| ≤ 7372/8192 = 0.9004; every add saturates; `out = f24_sat(lin + s)` (`:97`); existing test pins peak < 1.1 and non-growing late blocks (`tests/fv1_phaser_test.cpp` sanity 1). Loop memory ≈ 6 AP states → t60 ≈ 10 ms ≪ 0.2 s floor: tail table OK here.
- **[OK-PINNED] numerics** — `lutTri32` wrap entry t[32] = −1 = t[0] (periodic, no OOB); phase wrap `phase_ + inc_ − floor(...)` exact; inc_ ≤ 2.44e-4 ≫ float ε at phase≈1; `prepareInternal`/`resetInternal` clear stages/phase/prevOut (`:52-66`); param mapping matches doc §14 and `FxSlotLabels.cpp:130-135` (p2=Feedback, p3=Center).
- **[NIT] `Fv1Phaser.cpp:96` comment** says "mono-in / stereo-out" but `lout == rout` (`:98-99`) is duplicated mono (no stereo width stage). Design choice; comment overstates.

**Test suggestions:** notch-frequency pin (log-sweep a sine, locate min |H|; documents the atan warp vs Center); L==R identity pin; fc-clamp corner (Center=0,Depth=1) finite + parked-fraction; negative-feedback vs positive timbre difference; reset-then-render determinism (already partially pinned).
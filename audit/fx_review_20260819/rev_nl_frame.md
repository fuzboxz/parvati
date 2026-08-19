I have completed my review of all assigned files. Compiling the final report now.

# FX DSP Review — batch "Nonlinear 6x-OS pair + framework/chain glue"

**Scope:** FxWavefolder + FxRingModulator (`Source/dsp/fx/FxProcessors.{h,cpp}`, vendored warps cores), `Source/dsp/fx/fv1/Fv1Engine.h` + `Fv1FxProcessor.h`, `Source/dsp/fx/FxProcessor.h` + `FxChain.{h,cpp}`. Read-only; no files modified.

---

## 1. Numerical correctness — FINDINGS

- **[OK-PINNED] Wavefolder LUT clamp is correct and safe.** `FxProcessors.cpp:477-482` clamps the lookup input to ±2.29. `kScale = 2048/(2.25·1.02) = 892.3747` (`FxProcessors.cpp:463`); `stmlib::Interpolate` (dsp.h:43-48, no bounds check) computes `idx = sl·892.3747`, reads `floor(idx)` and `floor(idx)+1` from `lut_bipolar_fold+2048` (table size 4097, resources.h:74). At sl=±2.29 → idx ∈ [−2043.74, 2043.74] → max index touched `2048+2044 = 4092 ≤ 4096`; min `2048−2044 = 4 ≥ 0`. Table data read directly: ends saturate (entry 0 = −0.8678; final entries ≈ +0.8767 rising ~1e-4/4096 near the rail) — clamp is inaudible. Pinned by `parvati_clouds_fx_test.cpp:897-990`.
- **[RISK] RingModulator output is unbounded (≫ ±1) for hot input.** `FxProcessors.cpp:685-686`: `SoftLimit(gain·(Diode(x+c)+Diode(x−c)))`. `warpsDiode` (`FxProcessors.cpp:614-621`) is odd, = `0.173·(|x|−0.667)²·sign` for |x|>0.667; `c = 2·carrier`; `gain = 4+24·amount` (`:669`). `stmlib::SoftLimit(x)=x(27+x²)/(27+9x²)` → grows as **x/9** for large x (not bounded; SoftClip is the bounded one). Computed: peak input 0.5, amount 0.5 (gain 16): arg ≈ 7.38 → out ≈ **1.16**; input 0.5, amount 1: ≈ **1.64**; input 1.0: ≈ 2.97; input 4.0: ≈ **16.2** (arg 146). Upstream Warps relied on ADC-bounded ±1 input; the Parvati chain input is unclamped (the same condition that caused the Wavefolder SIGSEGV). Nothing in `FxChain` clips slot output; post-FX clipping is outside assigned scope (unverified). *Fix approach:* clamp the OS-domain signal pre-diode (e.g. `jlimit(±3)` or `SoftClip` the diode sum pre-gain), mirroring the Wavefolder fix.
- **[NIT] `f24_fromFloat` 1-LSB overshoot.** `Fv1Engine.h:63-69`: `lround(0.99999994f·2²³) = lround(8388607.5) = 8388608 = kOneQ23 > kMaxQ23` (ties-away). Harmless: all downstream ops saturate (int64 intermediates) and `f24_toFloat` gives exactly 1.0.
- **[NIT] Wavefolder Tone LP state has no denormal flush** (`FxProcessors.cpp:491-501`), unlike `Fv1Engine.h:352` which flushes explicitly. Depends on JUCE wrapper `ScopedNoDenormals`; only relevant if the processor runs outside a JUCE wrapper.

## 2. Parameter edges — CLEAN

- Wavefolder (`FxProcessors.cpp:446-503,504-510`): drive 0→1×; fold 0→gain 0.02 (upstream-faithful heavy attenuation, no div/0); bias 0.5→0; tone 1 bypasses the LP (bit-identical). `pow(100, tone)` ∈ [200 Hz, 20 kHz]; at 32 kHz host 20 kHz > Nyquist but `a = 1−e^{−3.93} = 0.980 < 1` — stable, no NaN.
- RingMod (`:656-691,692-698`): carrier 20 Hz–4 kHz log; `Render` CONSTRAINs normalized freq to ±0.25 (`quadrature_oscillator.h:39-40`); amount 0 still gain 4 — diode dead zone keeps small-signal output ≈ 0. No div/0, no illegal combos. jlimit on all params.
- FV-1 framework: `q14`/`f24_fromFloat`/`f24_sat` clamp all out-of-range and ±Inf; `f24_mulk(kMaxQ23, 8191) = 8388604` (no overflow), `f24_mulk(kMinQ23, −8192)` saturates correctly. `OnePoleLpFx` DC gain = 1 ± 1/8191 (14-bit split) — no runaway. `Allpass1Fx` clamps |c| ≤ 0.999 — stable. **[NIT]** NaN through `f24_fromFloat` falls into `lround(NaN)` (unspecified value, not a crash); unreachable with finite chain input.

## 3. Aliasing / oversampling — CLEAN

- Both nonlinear stages run inside the Warps 6× polyphase FIR (`SRC_UP/DOWN,6,48`): fold `FxProcessors.cpp:461-486`; diode product `:679-687` with the carrier band-limited to fs/2 **before** the diode (base-rate render → `srcUpCarrier_`, `:680-684`) — correct anti-alias design, faithful to upstream src_up_[0]=carrier.
- **[OK-PINNED] latency math:** `srcUp_.delay() = 48/6/2 = 4` base; `srcDown_.delay() = 48/2 = 24` high-rate = 4 base (`sample_rate_converter.h:108,157`); `latency() = 4 + 24/6 = 8` (`FxProcessors.cpp:516,704`), exact integer division. Carrier path adds the same 4-base up-delay as the signal's srcUp — aligned, no extra latency. Verified consistent with `FxChain` rings (kDelayCap 16 ≥ 8, kChainDelayCap 32 ≥ 24 for 3 OS series) and pinned by impulse tests (`parvati_clouds_fx_test.cpp:421-439`, `fx_param_coverage_test.cpp:519-539`).
- FIR gains verified by summation: SRC_UP 24-tap half sums to 3.000 → per-phase DC gain ≈ 1 (amplitude preserved); SRC_DOWN scales 1 → amplitude preserved. Mirror-index Accumulator maps i≥24 → h[47−i], never reading h[24..] of the 24-entry arrays.

## 4. State lifecycle — FINDINGS

- `FxWavefolder::prepare/reset` (`FxProcessors.cpp:423-444`) and `FxRingModulator::prepare/reset` (`:624-655`) re-`Init()` all SRCs (zeroes filter history) and scratch — clean re-prepare at a different rate. `FxChain::prepare` (`FxChain.cpp:33-107`) zeroes all delay rings, resets EQ state, recomputes fade coefficients, and **preserves** `wetFade_`/`dryWetCur_`/`masterMixCur_` (B7 documented). Ring clears on topology/order/type change (N3) documented tradeoff: an ≤ 8-sample zero blip masked by the 5 ms fade-in.
- FV-1 `RateBridge::prepare` re-designs + clears all 8 biquads, re-sizes buffers, resets phase (`Fv1Engine.h:363-396`) — no stale-rate history.
- Tail table vs DSP: **matches** for assigned effects. Wavefolder/RingMod → 0 (memoryless/one-pole) ✓. Spot-checked FV-1 mappings the table cites: Plate `0.1+p1·3.9 + p0·0.1` = `Fv1PlateReverb.cpp:36-44` ✓; Spring `0.2+p0·3.8` = `Fv1Spring.cpp:25` ✓; Room `0.1+p0·2.9` = `Fv1Room.cpp:31` ✓; Echo fb `p1·0.995` = `Fv1Echo.cpp:30` ✓; ClockedDelay fb `p1·0.95` ✓.
- **[NIT]** Zero-tail entries understate two ringing cases: Fv1Flanger (fb 0.92, loop ~5-10 ms → t60 ≈ 0.4–0.8 s > 0.2 s floor) and FxResonator (long-decay modal ring; effect file out of my scope — flagging for the table owner).

## 5. Click/pop risks — FINDINGS

- **[OK-PINNED] Type swaps mid-stream:** staged swap → `wetFade_ = 0` + 5 ms fade-in (`FxChain.cpp:249`), ring flush; pointer-move install, no AT allocation.
- **[OK-PINNED] Stepped carrier freq:** `QuadratureOscillator::Render` glides frequency via `ParameterInterpolator` across the block — no per-sub-chunk phase jump.
- **[OK-PINNED] Raw per-effect params** at the ~980 Hz cadence is a documented, deliberate chain-wide decision (`FxChain.h:170-183`); the Wavefolder fold map + tone LP (continuous state) and RingMod gain steps are no worse than that baseline. Dry/wet, masterMix and enable/bypass are per-sample one-pole smoothed.
- **[NIT]** `masterMixCur_` one-pole never reaches exactly 1.0f after any dip → the blend branch (`FxChain.cpp:649`) keeps executing forever (CPU only, inaudible: (1−g) < 1e-7). Also the `masterDryL_` ring is cold when the blend first activates (zeros for the first Lc ≤ 24 samples → ≤ 2.5% dry dip over ~0.5 ms).
- EQ coefficient changes: DF2T biquad state is continuous across coeff steps — no zip.

## 6. Rate assumptions — FINDINGS

- FV-1 `RateBridge` (`Fv1Engine.h:363-521`): persistent fractional phase (no accumulating drift; `hostWritePhase_` stays < `invRatio_` ≈ 1.5); `maxM_ = ceil(maxBlock·ratio_)+4` correctly sized for host rates both above and below 32.768 kHz (ratio 0.743@44.1k, 4.096@8k); **m==0** handled by ZOH of `prevL_/prevR_` (`:462-474`) — no dropout; upsample index math (`vj=(i−phaseStart)·ratio_`, clamp to `lastM`) verified in-bounds. BW clamp `min(15k, 0.49·hostRate)` (`:385`) prevents the sub-30 kHz instability. The linear (non-polyphase) resample AA is the documented chip-character choice.
- **[RISK] Chain master-EQ high shelf is unstable below a 10 kHz host rate.** `FxChain.cpp:500`: `w0 = 2π·5000/r`; for r < 10000, w0 > π → `sw < 0` → negative alpha → RBJ shelf coefficients leave the unit circle (poles unstable → possible runaway/NaN in `EqBiquad::process`). The 1 kHz mid band breaks only below ~2 kHz host. Exotic rates, but the same class of bug `Fv1Engine.h:385` explicitly guards. *Fix:* clamp both band centers to ≤ 0.45·rate (mirroring the RateBridge clamp).
- HostRateBridge (Clouds) is out of this batch's assigned set; not re-reviewed here.

## 7. Vendored warps adapters — CLEAN

- **[OK-PINNED] SRC_DOWN fast-path removal** (`sample_rate_converter.h:158-203`, Parvati patch): the circular path is self-consistent for any `input_size % 6 == 0` (every push writes slot + mirror, wraps within [x_, x_+N)), no stale-mirror discontinuity; Parvati always feeds `numSamples·6`. Pinned by `parvati_fx_onset_regression.cpp` (RingMod margins 0.34-0.98 documented).
- **[OK-PINNED] Oscillator phase clamp patch** (`quadrature_oscillator.h:56-61`): without it, phase exactly 0.0 → +1.0 → `Interpolate(…,1.0,1024)` reads `table[1025]` (size 1025) — OOB; clamp is correct. Wavetable indices: shape ≤ 1.9999 → integral ≤ 1 → max index `2·1+2+1 = 5` within the 6 tables.
- Carrier upsample amplitude ≈ 1 (per-phase gain verified in §3); time alignment carrier/signal verified in §3.

## 8. Existing tests — CLEAN (good coverage; additions suggested)

Covered: Wavefolder LUT-domain crash regression + continuity (`parvati_clouds_fx_test.cpp:897-990`); latency()==8 and topology accumulations (`fx_param_coverage_test.cpp:515-611`); D1/D2 blend alignment (`fx_routing_test.cpp:280-321`); OS sub-block survival 1..512 (`parvati_fx_modrate_test.cpp:118-165`); FV-1 framework round-trip, BW stability, delay/fixed-point units (`fv1_engine_test.cpp`); tail table (`render_quality_test.cpp:351-459`).

Suggested additions (do **not** write — for the test owner):
1. RingMod hot-input bound: |in| = 4, amount = 1, several blocks → assert finite **and** peak ≤ ~3 (current modrate test only bounds < 100 at ~0.5 input; a 16× overshoot would pass).
2. FxChain EQ finite-output test at an 8 kHz host rate (would catch the §6 shelf instability if hosts ever go there).
3. `f24_fromFloat(nextafterf(1.0f, 0.0f))` boundary (documents the 1-LSB NIT).
4. RateBridge long-run (≥ 10⁷ samples) phase/drift bound — current tests are short.
5. Chain masterMix first-activation transient bounded (cold masterDry ring).

---
# Parvati FX — FV-1 Hardware-Emulation Family (Design Spec)

A family of five per-part FX effects that emulate the **Spin FV-1** DSP and early
2010s lo-fi hybrid synths. They are a **separate family** from the existing
Clouds/Warps/Rings ports in `Source/dsp/fx/FxProcessors.*`: they share a common
**FV-1 emulation framework** (`Source/dsp/fx/fv1/`) and obey a strict hardware
constraint set.

## The five effects

| # | FxType (append) | Class | File |
|---|---|---|---|
| 1 | `ClockedDelay` | `Fv1ClockedDelay` | `fv1/Fv1ClockedDelay.{h,cpp}` |
| 2 | `Ensemble` | `Fv1Ensemble` | `fv1/Fv1Ensemble.{h,cpp}` |
| 3 | `PlateReverb` | `Fv1PlateReverb` | `fv1/Fv1PlateReverb.{h,cpp}` |
| 4 | `VinylCompressor` | `Fv1VinylCompressor` | `fv1/Fv1VinylCompressor.{h,cpp}` |
| 5 | `Phaser` | `Fv1Phaser` | `fv1/Fv1Phaser.{h,cpp}` |

## Architectural constraints (all five)

* **Sample rate:** internal processing fixed at **32.768 kHz**. Host audio is
  downsampled at the input and upsampled at the output via **basic linear
  interpolation** (zero-order hold optionally). **No modern anti-alias filters.**
* **Bandwidth limiting:** a **steep** input LP (before downsample) and output LP
  (after upsample) emulates the hardware ADC/DAC response. **-3 dB cutoff in
  [14.5, 15.5] kHz.** Implemented as two cascaded RBJ biquads (4th-order
  Butterworth, 24 dB/oct) at 15 kHz — a steep simple-IIR, not a polyphase FIR.
* **Memory limit:** **≤ 32,768 samples** of total delay memory per effect (1.0 s
  at 32.768 kHz). Each effect `static_assert`s its total.
* **Arithmetic:** audio path in **24-bit fixed-point (Q.23 signed)** with
  **saturation clipping** on overflow. All control parameters and filter
  coefficients quantized to **14-bit** resolution.
* **Compute:** ≤ ~128 instructions/sample, ≤ 4 read/write pointers, ≤ 4 low-order
  filters per algorithm (the per-effect notes override this where they call for
  more, e.g. the 6-stage phaser's six 1st-order allpass).

## Framework API (foundation)

`Source/dsp/fx/fv1/Fv1Engine.h` (header-only, **JUCE-free** so effects + their
unit tests compile standalone in seconds without the JUCE build):

* `namespace parvati::fv1`
* Fixed-point: `f24_fromFloat / f24_toFloat / f24_sat / f24_addSat / f24_mul /
  f24_mulk` (Q.23 data, 14-bit coeff multiply), `q14(c)` 14-bit coefficient
  quantizer, `quantBits(x,bits)` bit-truncation (grit). Constants `kOneQ23`,
  `kMaxQ23`, `kMinQ23`.
* `OnePoleLpFx` — 1-pole LP in fixed-point (coeff quantized to 14-bit).
* `Allpass1Fx` — 1st-order allpass in fixed-point (shared-coefficient, for phaser).
* `DelayLine<N>` — power-of-two fixed-point ring with integer + fractional
  (linear-interpolated) reads.
* `Lut32` sine + triangle lookup tables (33 entries, linear interp) + a phase
  accumulator helper. **No per-sample trig.**
* `RateBridge` — host↔32.768 kHz linear resample + the steep 15 kHz input/output
  BW-limiting biquads. Exposes `internalL()/internalR()` float scratch.

`Source/dsp/fx/fv1/Fv1FxProcessor.h`:
* `Fv1FxProcessor : public FxProcessor` — owns a `RateBridge`. Its `process()`
  does host→internal (BW-limited downsample) → per-internal-sample fixed-point
  `processSampleFx(lin,rin,lout,rout)` → internal→host (BW-limited upsample).
  Subclasses implement only `processSampleFx` (the fixed-point core) plus
  `prepare/reset/setParams/type`. `latency()` stays 0 (no uncompensated
  processing latency; musical delays are not reported).

## Per-effect parameter mappings (single source of truth)

Each effect uses generic params `param[0..3]` (param[4] unused; Mix is the chain
Dry/Wet). DSP workers MUST use these exact 0..1 -> physical mappings so the
`FxSlotLabels.cpp` readouts match.

### 1. ClockedDelay — labels {"Sync","Feedback","Tape Age","Grit"}
* **Sync (p0):** 8 discrete divisions, index `i=clamp(round(p*7),0,7)` over
  `{1/1,1/2,1/3,1/4,1/6,1/8,1/12,1/16}`. delaySeconds = `(4/divisor)*(60/bpm)`;
  `delaySamples = clamp(round(delaySeconds*32768),1,32767)`. Polled per block from
  `setTransport(bpm)`.
* **Feedback (p1):** `0..0.95` linear.
* **Tape Age (p2):** `0..1`. Drives a 1-pole LP cutoff `2000..200` Hz (older=darker,
  `fc = 2000*(1-p) + 200*p`) AND LFO depth `0..~6` samples on the read pointer
  (rate ~0.6 Hz from the sine LUT).
* **Grit (p3):** effective bits `24..8`, `bits = 24 - round(p*16)`; apply
  `f24_quantBits(x,bits)` to the delay-line INPUT.

### 2. Ensemble — labels {"Rate","Depth","Center","Feedback"}
* **Rate (p0):** `0.1..8.0` Hz (`rate = 0.1*pow(80,p)`). Phase-accumulator
  increment `= rate/32768` per internal sample; read the 32-value sine LUT (no
  per-sample trig). Two delay lines read the LUT at 90 deg offset.
* **Depth (p1):** `0..15.0` ms -> samples at 32768 (`depthSamp = p*15e-3*32768`).
* **Center (p2):** `2.0..25.0` ms -> samples (`centerSamp = (2 + p*23)e-3*32768`).
* **Feedback (p3):** `-0.9..0.9` (`-0.9 + p*1.8`).

### 3. PlateReverb — labels {"Predelay","Decay","Damping","Mod"}
* **Predelay (p0):** `0..100` ms linear -> predelay samples (separate buffer).
* **Decay (p1):** `0.1..4.0` s -> comb feedback `g = pow(10, -3/(decay*32768))`
  (60 dB over the decay time). Clamp g to [0, 0.999].
* **Damping (p2):** `500..12000` Hz (`fc = 500*pow(24,p)`); 1-pole LP inside each
  comb feedback loop.
* **Mod (p3):** `0..1` -> allpass delay-length LFO amplitude `0..15` samples.
* Comb delays (mutually prime, total < 32768): `{1427, 2063, 3187, 4759}` samples
  (= 11436). Predelay capacity = 100 ms = 3277 samples. Allpass base delays
  `{347, 113}` samples. Total well under the 32,768 budget. Coeffs quantized to
  14-bit via q14().

### 4. VinylCompressor — labels {"Compress","Pitch","Crackle","Age"}
* **Compress (p0):** `0..1` macro. Threshold `th = 1.0 - 0.9*p` (lowers); makeup
  gain `mg = 1.0 + 3.0*p`. Ratio fixed ~4:1; feed-forward peak.
* **Pitch (p1):** `0..1` -> dual-LFO (0.5 Hz + 4.0 Hz, sine LUT) depth `0..~3`
  samples on a 50 ms delay read pointer.
* **Crackle (p2):** `0..1` -> output level of an LCG crackle (impulse when LFG
  value > 0.98). `level = p`.
* **Age (p3):** `1000..15000` Hz (`fc = 1000*pow(15,p)`); 1-pole LP post-compressor.
* Envelope: abs() + one-pole RC, attack 2 ms (`aA = 1-exp(-1/(0.002*32768))`),
  release 150 ms. 50 ms delay = 1638 samples.

### 5. Phaser — labels {"Rate","Depth","Feedback","Center"}
* **Rate (p0):** `0.1..8.0` Hz (`rate = 0.1*pow(80,p)`). Triangle LUT LFO.
* **Depth (p1):** `0..1` LFO amplitude on the allpass coefficient.
* **Feedback (p2):** `-0.9..0.9`.
* **Center (p3):** `200..2000` Hz (`fc = 200*pow(10,p)`). The shared allpass
  coefficient is computed once per sample from `center + depth*lfo`, applied to
  all six stages. Feedback path has a hard-clip saturation block.

## Wiring (foundation touches these central files)

* `dsp/fx/FxTypes.h` — append enum values 11..15 (APPEND-ONLY; stored as choice
  index in presets).
* `ParameterLayout.cpp` `makeFxTypes()` — append 5 display strings.
* `dsp/fx/FxProcessor.h` — add `virtual void setTransport(double bpm,bool
  playing){}` (default no-op).
* `dsp/fx/FxChain.{h,cpp}` — add `setTempo(bpm,playing)` forwarding to slots.
* `SynthEngine.cpp` `processTransport` — push tempo to every part's chain.
* `dsp/fx/FxProcessors.cpp` factory — Phase-0 leaves the 5 new types returning
  `{}` (passthrough); Phase-2 wires the concrete classes.
* `ui/FxSlotLabels.cpp` — `activeParamCount` / `paramLabel` / `paramValueText`
  for the 5 types.
* Visualizer falls through to `drawNone` (cosmetic; deferred).

## Per-effect specs

(See the user spec; each worker gets a lane-specific task with the exact
parameter→param-slot mapping, the FV-1 algorithm, memory budget, and the
standalone-test contract.)

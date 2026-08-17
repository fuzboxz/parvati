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
* **Nonlinear-stage oversampling (2026-08-17 exception):** the two
  hard-nonlinear effects (Overdrive, LUT Distortion) run their wavetable
  stage inside a **6x oversampled domain** — the vendored Warps polyphase FIR
  (`warps::SampleRateConverter<SRC_UP/DOWN,6,48>`) the Wavefolder/RingMod
  slots already use — with the Q.23 saturating shaper evaluated per
  oversampled sample. The "no modern anti-alias filter" rule above governs
  the RATE BRIDGE (linear host<->internal resampling); it cannot govern the
  harmonics a hard shaper GENERATES: those fold at the 16.384 kHz internal
  Nyquist into inharmonic crackle (measured worst spur only 16 dB below the
  fundamental at a 3 kHz input — the audible note-onset crackle burst), and
  no input/output LP can undo a fold. Linear stages (Tone LP, Level, clock
  jitter) and internal-sample-defined timings (the shape-crossfade clock)
  stay at the 1x rate. Post-fix worst folded spur: -45 dB at 3 kHz
  (`parvati_fv1_alias_probe`).
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

### 4. VinylCompressor — labels {"Compress","Wow/Flut","Crackle","Age"}
Tuned after the Roland SP-303/SP-404 "Vinyl Sim" COMP (deep analog-record
squash + glue + warpy wow + subtle noise floor).
* **Compress (p0):** `0..1` macro. Threshold `th = 1.0 - 0.96*p` (down to
  0.04); ratio `4:1 .. ~16:1` (gain exponent `0.75 + 0.19*p`); makeup
  `mg = 1.0 + 5.0*p` (max 6.0, push quiet material UP); feed-forward peak.
  A fixed-point cubic soft-saturation "lathe" follows the makeup: drive
  `a = 0.06 + 0.25*p` (unity slope at 0, flat-top above the knee — analog
  glue/warmth rising with Compress).
* **Wow/Flut (p1):** `0..1` -> slow heavy WOW (0.4 Hz, depth `0..300`
  samples = ~2.3 % pitch deviation) + fast flutter (3.1 Hz, `0..24` samples
  = ~1.4 %) on a 50 ms delay read pointer — the audible old-school
  SP-303/505 "thumb on the record" warble. (Depth matters: a 24-sample wow
  is only 0.18 % deviation — below the ~0.3 % slow-FM hearing threshold,
  i.e. inaudible; measured period swing at full depth is 12 % p-p. Max
  delay value ~1963 stays inside the 2048 ring.)
* **Crackle (p2):** `0..1` -> SUBTLE vinyl noise floor: soft ticks (trigger
  when LCG value > 0.994, ~0.6 % density; decaying ~2-3 sample envelope;
  `u^2`-skewed small amplitudes; ceiling ~0.18*p) + low hiss (~-54 dB).
  Never full-scale pops.
* **Age (p3):** `700..15000` Hz (`fc = 700*pow(15000/700,p)`); 1-pole LP
  post-compressor.
* Envelope: abs() + one-pole RC, attack 0.8 ms (`aA = 1-exp(-1/(0.0008*32768))`),
  release 250 ms (the SP pump). 50 ms delay = 1638 samples; max wow/flutter
  swing ~1963 stays inside the 2048 ring.

### 5. Overdrive — labels {"Drive","Bias","Tone","Level"} (2026-08-17)
* **Drive (p0):** `1..16x` pre-gain (log) into a 1024-entry asymmetric
  12AX7-ish soft-clip LUT (the "EEPROM table" idiom). Integer 2x stages + a
  14-bit fractional remainder realize >2x gains in the fixed-point path.
* **Bias (p1):** `-0.3..+0.3` table-index offset (even harmonics / asymmetry).
* **Tone (p2):** `700..15000` Hz post-LP.
* **Level (p3):** `0..2` output trim.

### 6. LUT Distortion — labels {"Drive","Shape","Jitter","Tone"} (2026-08-17)
The "super digital" wavetable distortion: 16 stepped weird shapes.
* **Drive (p0):** `1..8x` pre-gain into the table index.
* **Shape (p1):** 16 wavetables — Clip / Soft / Tube / Wrap / OctUp / Fuzz /
  Square / Steps / SinFold / Cheby2 / Cheby3 / AsymCub / Mirror / HalfGate /
  Crush4 / Sparse (all 1024-entry, Q.14, built once at construction).
* **Jitter (p2):** shared-clock timing wobble — an LCG noise one-pole-smoothed
  into a +-12-sample read-position wobble on a 64-sample input delay; ONE
  wobble for both channels (a single crystal). 0 = fixed 1-sample read.
* **Tone (p3):** `700..15000` Hz post-LP.
* No bitcrushing (the Clocked Delay's Grit owns that).

### 7. Compressor — labels {"Amount","Attack","Release","Level"} (2026-08-17)
* **Amount (p0):** threshold `1.0 -> 0.05`, ratio `2:1 -> 10:1`, makeup `1 -> 3`.
* **Attack (p1):** `0.5..50 ms` (log). **Release (p2):** `20..500 ms` (log).
* **Level (p3):** `0..2` trim.

### 8. Gate — labels {"Thresh","Attack","Hold","Release"} (2026-08-17)
* **Thresh (p0):** `0 = DISABLED` (always open, transparent — the knob turns
  the gate off) .. `0.7` of full scale.
* **Attack (p1):** `0.05..10 ms`. **Hold (p2):** `0..150 ms`.
  **Release (p3):** `5..500 ms`. ~2.7 ms peak detector; clickless one-pole gain.

### 9. Chorus — labels {"Rate","Depth","Center","Feedback"} (2026-08-17)
* **Rate (p0):** `0.1..8 Hz` (log, both LFOs together). **Depth (p1):** `0..6 ms`.
* **Center (p2):** `5..25 ms`. **Feedback (p3):** `0..0.5`.
* Two detuned SIN-LFO voices (R trails 108 deg), panned hard L/R (AN-0001 style).

### 10. Flanger — labels {"Rate","Depth","Manual","Feedback"} (2026-08-17)
* **Rate (p0):** `0.05..3 Hz`. **Depth (p1):** `0..4.5 ms`.
* **Manual (p2):** `0.15..6 ms` base delay. **Feedback (p3):** `0..0.92`
  (8 kHz loop damper). L/R sweeps 180 deg apart.

### 11. Echo — labels {"Time","Feedback","Tone","Spread"} (2026-08-17)
* **Time (p0):** `10..470 ms` per side (log). **Feedback (p1):** `0..0.95`.
* **Tone (p2):** `700..12000` Hz loop damper. **Spread (p3):** R time `1..2x`.
* Ping-pong: L tap -> R line; damped R tap + input -> L line. The two 16384
  rings consume EXACTLY the 32768-word FV-1 RAM budget.

### 12. Room — labels {"Decay","Damp","Width","Tone"} (2026-08-17)
* Schroeder: 4 parallel lowpass combs {1687,1601,2053,2251} -> two
  decorrelated series-allpass chains (L {191,281}, R {179,271}).
* **Decay (p0):** `0.1..3 s`. **Damp (p1):** `500..12000` Hz.
  **Width (p2):** mono <-> stereo. **Tone (p3):** `700..15000` Hz output LP.

### 13. Spring — labels {"Decay","Damp","Chirp","Width"} (2026-08-17)
* Two springs: driver soft-clip -> loop [~35 ms delay -> SIX short allpasses
  (the dispersion: transients chirp/boing) -> damping LP] -> feedback ~0.97 max.
* **Decay (p0):** `0.2..4 s`. **Damp (p1):** `500..8000` Hz.
  **Chirp (p2):** AP coefficient `0.35..0.95`. **Width (p3):** mono <-> stereo.

### 14. Phaser — labels {"Rate","Depth","Feedback","Center"}
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

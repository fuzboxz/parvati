# Parvati FX — FV-1 Hardware-Emulation Family (Design Spec)

A family of five per-part FX effects emulates the **Spin FV-1** DSP and the
early-2010s lo-fi hybrid synths. This family is **separate** from the existing
Clouds/Warps/Rings ports in `Source/dsp/fx/FxProcessors.*`. The five effects
share a common **FV-1 emulation framework** (`Source/dsp/fx/fv1/`) and obey a
strict hardware constraint set.

## The five effects

| # | FxType (append) | Class | File |
|---|---|---|---|
| 1 | `ClockedDelay` | `Fv1ClockedDelay` | `fv1/Fv1ClockedDelay.{h,cpp}` |
| 2 | `Ensemble` | `Fv1Ensemble` | `fv1/Fv1Ensemble.{h,cpp}` |
| 3 | `PlateReverb` | `Fv1PlateReverb` | `fv1/Fv1PlateReverb.{h,cpp}` |
| 4 | `VinylCompressor` | `Fv1VinylCompressor` | `fv1/Fv1VinylCompressor.{h,cpp}` |
| 5 | `Phaser` | `Fv1Phaser` | `fv1/Fv1Phaser.{h,cpp}` |

## Architectural constraints (all five)

* **Sample rate:** internal processing is fixed at **32.768 kHz**. Host audio
  is downsampled at the input and upsampled at the output via **basic linear
  interpolation** (zero-order hold optionally). **No modern anti-alias
  filters.**
* **Bandwidth limiting:** a **steep** input LP (before downsample) and output
  LP (after upsample) emulate the hardware ADC/DAC response. **-3 dB cutoff
  in [14.5, 15.5] kHz.** Implementation: two cascaded RBJ biquads (4th-order
  Butterworth, 24 dB/oct) at 15 kHz — a steep simple-IIR, not a polyphase FIR.
* **Nonlinear-stage oversampling (2026-08-17 exception):** the two
  hard-nonlinear effects (Overdrive, LUT Distortion) run their wavetable
  stage inside a **6x oversampled domain**. This domain uses the vendored
  Warps polyphase FIR (`warps::SampleRateConverter<SRC_UP/DOWN,6,48>`) that
  the Wavefolder/RingMod slots already use. The Q.23 saturating shaper is
  evaluated per oversampled sample. The "no modern anti-alias filter" rule
  above governs the RATE BRIDGE (linear host<->internal resampling). It
  cannot govern the harmonics that a hard shaper GENERATES. Those harmonics
  fold at the 16.384 kHz internal Nyquist into inharmonic crackle (measured
  worst spur only 16 dB below the fundamental at a 3 kHz input — the audible
  note-onset crackle burst). No input/output LP can undo a fold. Linear
  stages (Tone LP, Level, clock jitter) and internal-sample-defined timings
  (the shape-crossfade clock) stay at the 1x rate. Post-fix worst folded
  spur: -45 dB at 3 kHz (`parvati_fv1_alias_probe`).
* **Memory limit:** **≤ 32,768 samples** of total delay memory per effect
  (1.0 s at 32.768 kHz). Each effect `static_assert`s its total.
* **Arithmetic:** audio path in **24-bit fixed-point (Q.23 signed)** with
  **saturation clipping** on overflow. All control parameters and filter
  coefficients are quantized to **14-bit** resolution.
* **Compute:** ≤ ~128 instructions/sample, ≤ 4 read/write pointers, ≤ 4
  low-order filters per algorithm (the per-effect notes override this where
  they need more, e.g. the 6-stage phaser's six 1st-order allpass).

## Framework API (foundation)

`Source/dsp/fx/fv1/Fv1Engine.h` (header-only, **JUCE-free**, so effects +
their unit tests compile standalone in seconds without the JUCE build):

* `namespace parvati::fv1`
* Fixed-point: `f24_fromFloat / f24_toFloat / f24_sat / f24_addSat / f24_mul /
  f24_mulk` (Q.23 data, 14-bit coeff multiply), `q14(c)` 14-bit coefficient
  quantizer, `quantBits(x,bits)` bit-truncation (grit). Constants `kOneQ23`,
  `kMaxQ23`, `kMinQ23`.
* `OnePoleLpFx` — 1-pole LP in fixed-point (coeff quantized to 14-bit).
* `Allpass1Fx` — 1st-order allpass in fixed-point (shared-coefficient, for
  phaser).
* `DelayLine<N>` — power-of-two fixed-point ring with integer + fractional
  (linear-interpolated) reads.
* `Lut32` sine + triangle lookup tables (33 entries, linear interp) + a phase
  accumulator helper. **No per-sample trig.**
* `RateBridge` — host↔32.768 kHz linear resample + the steep 15 kHz
  input/output BW-limiting biquads. Exposes `internalL()/internalR()` float
  scratch.

`Source/dsp/fx/fv1/Fv1FxProcessor.h`:
* `Fv1FxProcessor : public FxProcessor` — owns a `RateBridge`. Its `process()`
  does host→internal (BW-limited downsample) → per-internal-sample
  fixed-point `processSampleFx(lin,rin,lout,rout)` → internal→host
  (BW-limited upsample). Subclasses implement only `processSampleFx` (the
  fixed-point core) plus `prepare/reset/setParams/type`. `latency()` stays 0
  (musical delays are not reported). The ONE exception: the 6x-oversampled
  distortion pair (Overdrive, LUT Distortion) reports its SRC group delay
  (`8 internal samples, converted to host samples in prepareInternal`). The
  chain can then time-align dry/wet.

## Per-effect parameter mappings (single source of truth)

Each effect uses generic params `param[0..3]` (param[4] unused; Mix is the
chain Dry/Wet). DSP workers MUST use these exact 0..1 -> physical mappings so
that the `FxSlotLabels.cpp` readouts match.

### 1. ClockedDelay — labels {"Sync","Feedback","Tape Age","Grit"}
* **Sync (p0):** 8 discrete divisions, index `i=clamp(round(p*7),0,7)` over
  `{1/1,1/2,1/3,1/4,1/6,1/8,1/12,1/16}`. delaySeconds = `(4/divisor)*(60/bpm)`;
  `delaySamples = clamp(round(delaySeconds*32768),1,32767)`. Polled per block
  from `setTransport(bpm)`.
* **Feedback (p1):** `0..0.95` linear.
* **Tape Age (p2):** `0..1`. Drives a 1-pole LP cutoff `2000..200` Hz
  (older=darker, `fc = 2000*(1-p) + 200*p`) AND LFO depth `0..~6` samples on
  the read pointer (rate ~0.6 Hz from the sine LUT).
* **Grit (p3):** effective bits `24..8`, `bits = 24 - round(p*16)`; apply
  `f24_quantBits(x,bits)` to the delay-line INPUT.

### 2. Ensemble — labels {"Rate","Depth","Center","Feedback"}
* **Rate (p0):** `0.1..8.0` Hz (`rate = 0.1*pow(80,p)`). Phase-accumulator
  increment `= rate/32768` per internal sample; read the 32-value sine LUT
  (no per-sample trig). Two delay lines read the LUT at 90 deg offset.
* **Depth (p1):** `0..15.0` ms -> samples at 32768 (`depthSamp = p*15e-3*32768`,
  capped at `center-1` — the old Depth=1/Center=0 corner pinned ~46% of every
  sweep at the 1-sample read floor; 2026-08-19).
* **Center (p2):** `2.0..25.0` ms -> samples (`centerSamp = (2 + p*23)e-3*32768`).
* **Feedback (p3):** `-0.9..0.9` (`-0.9 + p*1.8`).

### 3. PlateReverb — labels {"Predelay","Decay","Damping","Mod"}
* **Predelay (p0):** `0..100` ms linear -> predelay samples (separate buffer).
* **Decay (p1):** `0.1..4.0` s. Per-comb feedback
  `g_i = pow(10, -3*D_i/(decay*32768))` where `D_i` is the comb's own delay
  — the per-PASS RT60 law. The knob therefore delivers t60 == Decay by
  construction. (The pre-2026-08-19 code used the per-sample law
  `10^(-3/(decay*fs))` applied per pass. That made the knob inert above ~3%
  travel — real t60 5-17 min.) The clamp [0, 0.999] is now a guard that
  never engages. Measured: decay 4.0 -> 3.58 s, 2.05 -> 1.84 s (~10%
  undershoot from the in-loop damping LP). At the 0.1 s end, the Schroeder
  allpass bank sets a ~0.2 s diffusion floor that is independent of the
  feedback law.
* **Damping (p2):** `500..12000` Hz (`fc = 500*pow(24,p)`); 1-pole LP inside
  each comb feedback loop.
* **Mod (p3):** `0..1` -> allpass delay-length LFO amplitude `0..15` samples.
* Comb delays (mutually prime, total < 32768): `{1427, 2063, 3187, 4759}`
  samples (= 11436). Predelay capacity = 100 ms = 3277 samples. Allpass base
  delays `{347, 113}` samples. Total is well under the 32,768 budget. Coeffs
  quantized to 14-bit via q14().

### 4. VinylCompressor — labels {"Compress","Wow/Flut","Crackle","Age"}
Tuned after the Roland SP-303/SP-404 "Vinyl Sim" COMP (deep analog-record
squash + glue + warpy wow + subtle noise floor).
* **Compress (p0):** `0..1` macro. Threshold `th = 1.0 - 0.96*p` (down to
  0.04); ratio `4:1 .. ~16:1` (gain exponent `0.75 + 0.19*p`); makeup
  `mg = 1.0 + 5.0*p` (max 6.0, pushes quiet material UP); feed-forward peak.
  A fixed-point cubic soft-saturation "lathe" follows the makeup: drive
  `a = 0.06 + 0.25*p` (unity slope at 0, flat-top above the knee — analog
  glue/warmth rises with Compress).
* **Wow/Flut (p1):** `0..1` -> slow heavy WOW (0.4 Hz, depth `0..300` samples
  = ~2.3 % pitch deviation) + fast flutter (3.1 Hz, `0..24` samples = ~1.4 %)
  on a 50 ms delay read pointer — the audible old-school SP-303/505 "thumb
  on the record" warble. (Depth matters: a 24-sample wow is only 0.18 %
  deviation — below the ~0.3 % slow-FM hearing threshold, that is, inaudible;
  measured period swing at full depth is 12 % p-p. Max delay value ~1963
  stays inside the 2048 ring.)
* **Crackle (p2):** `0..1` -> SUBTLE vinyl noise floor: soft ticks (trigger
  when LCG value > 0.994, ~0.6 % density; decaying ~2-3 sample envelope;
  `u^2`-skewed small amplitudes; ceiling ~0.18*p) + low hiss (~-54 dB). Never
  full-scale pops.
* **Age (p3):** `700..15000` Hz (`fc = 700*pow(15000/700,p)`); 1-pole LP
  post-compressor.
* Envelope: abs() + one-pole RC, attack 0.8 ms (`aA =
  1-exp(-1/(0.0008*32768))`), release 250 ms (the SP pump). 50 ms delay =
  1638 samples; max wow/flutter swing ~1963 stays inside the 2048 ring.

### 5. Overdrive — labels {"Drive","Bias","Tone","Level"} (2026-08-17)
* **Drive (p0):** `1..16x` pre-gain (log) into a 1024-entry asymmetric
  12AX7-ish soft-clip LUT (the "EEPROM table" idiom). Integer 2x stages + a
  14-bit fractional remainder realize >2x gains in the fixed-point path.
  (2026-08-19: the table index now reads the true domain `xT = D*x`. The old
  `>>13` shift read it at 8x, so the knob was effectively 8..128x. Small-
  signal gain at Drive 1 is now ~1.04; it was 7.78.) Curve truth: positive
  half peaks 0.77 then droops to ~0.41 at the rail; negative half clamps -1
  beyond ~-0.76 — asymmetric by design, NOT "saturates ~1.6".
* **Bias (p1):** `-0.3..+0.3` table-domain offset (even harmonics /
  asymmetry). The ±38-index realization now matches the domain value exactly
  (was ±0.60). A ~10 Hz one-pole HP on the wet output removes the bias-
  induced DC.
* **Tone (p2):** `700..15000` Hz post-LP.
* **Level (p3):** `0..2` output trim (integer/fractional shift+gain split —
  the upper half of the knob works; pre-2026-08-19 it clamped flat at 1.0).
* `latency()` reports the 6x-OS SRC group delay (8 internal samples -> 12
  host samples @48k) so that chain dry/wet blending stays comb-free.

### 6. LUT Distortion — labels {"Drive","Shape","Jitter","Tone"} (2026-08-17)
The "super digital" wavetable distortion: 16 stepped weird shapes.
* **Drive (p0):** `1..8x` pre-gain into the table index — now the true domain
  `xT = D*x` (2026-08-19: the old `>>13` shift read 8x hot; Drive 1 small-
  signal gain ~1.03, was 9.0). Stereo math: each channel's curve output is
  HALVED before the saturating add. Mono L==R therefore reaches the true
  ~0.78 ceiling instead of the old 0.5 sum-cap. A ~10 Hz one-pole HP on the
  wet output removes shape DC (Cheby2/OctUp/Asym). `latency()` reports the
  6x-OS group delay like Overdrive.
* **Shape (p1):** 16 wavetables — Clip / Soft / Tube / Wrap / OctUp / Fuzz /
  Square / Steps / SinFold / Cheby2 / Cheby3 / AsymCub / Mirror / HalfGate /
  Crush4 / Sparse (all 1024-entry, Q.14, built once at construction).
* **Jitter (p2):** shared-clock timing wobble — an LCG noise, one-pole-
  smoothed, into a +-12-sample read-position wobble on a 64-sample input
  delay; ONE wobble for both channels (a single crystal). 0 = fixed 1-sample
  read.
* **Tone (p3):** `700..15000` Hz post-LP.
* No bitcrushing (the Clocked Delay's Grit does that).

### 7. Compressor — labels {"Amount","Attack","Release","Level"} (2026-08-17)
* **Amount (p0):** threshold `1.0 -> 0.05`, ratio `2:1 -> 10:1`, makeup
  `1 -> 3`.
* **Attack (p1):** `0.5..50 ms` (log). **Release (p2):** `20..500 ms` (log).
* **Level (p3):** `0..2` trim (integer/fractional split — the upper half
  works, 2026-08-19; was clamped flat at 1.0).

### 8. Gate — labels {"Thresh","Attack","Hold","Release"} (2026-08-17)
* **Thresh (p0):** `0 = DISABLED` (always open, transparent — the knob turns
  the gate off) .. `0.7` of full scale.
* **Attack (p1):** `0.05..10 ms`. **Hold (p2):** `0..150 ms`.
  **Release (p3):** `5..500 ms`. ~2.7 ms peak detector; clickless one-pole
  gain.

### 9. Chorus — labels {"Rate","Depth","Center","Feedback"} (2026-08-17)
* **Rate (p0):** `0.1..8 Hz` (log, both LFOs together). **Depth (p1):**
  `0..6 ms` (capped at `center-1` samples — the Depth=1/Center=0 corner can
  no longer pin the sweep at the 1-sample read floor, 2026-08-19).
  **Center (p2):** `5..25 ms`. **Feedback (p3):** `0..0.5`.
* Two equal-rate SIN-LFO voices (R trails a fixed 108 deg; NOT detuned —
  2026-08-19 doc fix), panned hard L/R (AN-0001 style).

### 10. Flanger — labels {"Rate","Depth","Manual","Feedback"} (2026-08-17)
* **Rate (p0):** `0.05..3 Hz`. **Depth (p1):** `0..4.5 ms` (capped at
  `base-1` samples — the old Manual=0/Depth=1 corner pinned ~49% of every
  sweep at the 1-sample floor and collapsed the jet; 2026-08-19).
* **Manual (p2):** `0.15..6 ms` base delay. **Feedback (p3):** `0..0.92`
  (8 kHz loop damper). L/R sweeps 180 deg apart.

### 11. Echo — labels {"Time","Feedback","Tone","Spread"} (2026-08-17)
* **Time (p0):** `10..470 ms` per side (log). **Feedback (p1):** `0..0.95`.
* **Tone (p2):** `700..12000` Hz loop damper. **Spread (p3):** R time `1..2x`.
* Both read taps (Time AND Spread) GLIDE on retarget — Q.16 slew at ≤0.25
  sample/internal-sample with a ≤1/16-sample snap, mirroring the Clocked
  Delay (2026-08-19; stepped taps jumped the read pointer and clicked). A
  circulating echo attenuates while its tap slews — authentic tape retarget.
* Ping-pong: L tap -> R line; damped R tap + input -> L line. The two 16384
  rings consume EXACTLY the 32768-word FV-1 RAM budget.

### 12. Room — labels {"Decay","Damp","Width","Tone"} (2026-08-17)
* Schroeder: 4 parallel lowpass combs {1687,1601,2053,2251} -> two
  decorrelated series-allpass chains (L {191,281}, R {179,271}).
* **Decay (p0):** `0.1..3 s`, per-comb `g_i = pow(10, -3*D_i/(decay*32768))`
  (the same per-pass law as Plate — measured 3.0 -> 2.69 s, 1.55 -> 1.39 s;
  ~0.2 s allpass diffusion floor at the 0.1 s end). **Damp (p1):**
  `500..12000` Hz. **Width (p2):** mono <-> stereo. **Tone (p3):**
  `700..15000` Hz output LP.

### 13. Spring — labels {"Decay","Damp","Chirp","Width"} (2026-08-17)
* Two springs: driver soft-clip -> loop [~35 ms delay -> SIX short allpasses
  (the dispersion: transients chirp/boing) -> damping LP] -> per-spring
  feedback `g_s = pow(10, -3*D_s/(decay*32768)) * (1-0.25*chirp)` with `D_s`
  the loop length (A: 1350, B: 1333). t60 == Decay at Chirp 0. Chirp
  back-off SHORTENS the tail (by design: dispersion trades ring for boing).
  The cap 0.97 never engages.
* **Decay (p0):** `0.2..4 s` (the knob works across its range —
  pre-2026-08-19 the cap clamped it inert at every setting). **Damp (p1):**
  `500..8000` Hz. **Chirp (p2):** AP coefficient `0.35..0.95`. **Width
  (p3):** TRUE mono at 0 (R is a bit-exact copy of spring A; 1 = the two
  decorrelated springs).

### 14. Phaser — labels {"Rate","Depth","Feedback","Center"}
* **Rate (p0):** `0.1..8.0` Hz (`rate = 0.1*pow(80,p)`). Triangle LUT LFO.
* **Depth (p1):** `0..1` LFO amplitude on the allpass coefficient.
* **Feedback (p2):** `-0.9..0.9`.
* **Center (p3):** `200..2000` Hz (`fc = 200*pow(10,p)`). The shared allpass
  coefficient is computed once per sample from `center + depth*lfo` and
  applied to all six stages. The feedback path has a hard-clip saturation
  block.

## Wiring (foundation touches these central files)

* `dsp/fx/FxTypes.h` — append enum values 11..15 (APPEND-ONLY; stored as
  choice index in presets).
* `ParameterLayout.cpp` `makeFxTypes()` — append 5 display strings.
* `dsp/fx/FxProcessor.h` — add `virtual void setTransport(double bpm,bool
  playing){}` (default no-op).
* `dsp/fx/FxChain.{h,cpp}` — add `setTempo(bpm,playing)` forwarding to slots.
* `SynthEngine.cpp` `processTransport` — push tempo to every part's chain.
* `dsp/fx/FxProcessors.cpp` factory — Phase-0 leaves the 5 new types
  returning `{}` (passthrough); Phase-2 wires the concrete classes.
* `ui/FxSlotLabels.cpp` — `activeParamCount` / `paramLabel` /
  `paramValueText` for the 5 types.
* Visualizer falls through to `drawNone` (cosmetic; deferred).

## Per-effect specs

(See the user spec; each worker gets a lane-specific task with the exact
parameter→param-slot mapping, the FV-1 algorithm, memory budget, and the
standalone-test contract.)

### Out-of-domain table reads (LutDistortion, fixed 2026-08-21)

The wavetable spans x in [-4,4); driven peaks past the rails must never read
ZERO. Two stacked bugs made loud passages gate to silence (the "distortion
dropouts" report — measured windowed RMS fell to 0.2–1.4% of the running
median mid-note):

1. The blanket index CLAMP read the wrap-family shapes' edge entries, which
   are zero (Wrap #3 / SFold #8 evaluate sin(±π)=0 at ±4). Fix: those shapes
   are periodic (period 2 in x = 256 entries), so the index WRAPS modulo
   1024 — the exact continuation of the curve (loud peaks fold over, the
   intended character). Every other shape saturates at its edge entry as
   before.
2. The drive gain ladder saturated in Q.23 (f24_addSat(v,v) at the rail) and
   pre-collapsed the domain to x in [-1,1), where SFold's sine is 0 at the
   rail. Fix: the ladder is unsaturated (|v| <= 8·rail = 2^26, int32-safe);
   out-of-domain peaks reach the wrap/clamp policy intact.

Pinned by tests/parvati_fx_lut_dropout_test.cpp (red on the pre-fix tree:
rmsMin 0.002–0.014; green post-fix: 0.28–0.77, margins > 2x).

### FX knob-drag dezipper (2026-08-21 — "whichever knob I drag crackles")

Subagent investigation (diff-census method: max impulse of render(dragged) −
render(static) with identical input — the raw census false-positives on
folded/driven content):

* **Base-param dezipper 3 ms → 15 ms** (SynthEngine.h
  kBaseDeClickTauSec): a fast drag fires ~8 param ticks per block. The 3 ms
  one-pole tracked them almost instantly, and the ~980 Hz sub-chunk cadence
  stepped FX coefficients directly. Flanger Manual measured worst
  0.1613 → 0.0493. The FX MOD MATRIX still passes RAW (audio-rate parity
  preserved) and the fix is unconditional (no Settings toggle dependency).
  Red-validated at 3 ms.
* **Fv1Flanger base-delay glide** (the Echo/ClockedDelay Q16 idiom, defense
  in depth): Manual steps the read position ~2.5 smp/tick on fast drags. The
  one-pole glide (k=1/256, 0.25 smp/sample cap) turns every move into a
  tape-speed pitch bend.
* **FxChain::setTopology/setOrder value-guarded**: the fxDirty_ service
  re-pushed both on EVERY param write (including Dry/Wet). Each call cleared
  the dry/wet latency-compensation rings. The dry component of any
  latency>0 slot read zeros, then jumped: a click per write (the dry/wet
  crackle). Now only a real routing change clears them.
* **Ruled out (measured)**: the common fxDirty_ service (None-slot writes
  diff 0.0000); real-time contention under concurrent 120 Hz writes (worst
  0.93 ms vs an 11.6 ms deadline); the dry/wet per-sample smoother itself
  (0.0060).
* **Character, documented**: the Wavefolder fold/bias drag buzz is
  RATE-INDEPENDENT (0.0992 slow vs 0.0975 fast) and output-continuous. It
  is the slide of the fold curve's C0 knees, inherent to modulation of a
  folding curve. No dezipper applies. Pinned at a 0.12 bound with a
  fast==slow equality check in tests/parvati_fx_drag_test.cpp.

### Subagent audit wave (2026-08-21): the phaser class, family-wide

Three parallel code audits (feedback loops / shapers / rate-bridging) ran
over the FV-1 tree. REAL findings, all fixed + pinned:

* **Overdrive drive ladder**: an earlier unsaturated-ladder fix was LOST to
  a `git checkout` during red-validation (the audit caught it) — re-applied.
* **q14 drive remainder pinned to unity (BOTH distortions)**: the fractional
  drive stage was encoded in [1,2), but q14 clamps c>=1.0 to 1.0. The Drive
  knob was therefore a powers-of-two staircase (1/2/4/8[/16]x; every
  intermediate position dead). The remainder is now in [0.5,1). Pinned by
  fx-invariants [I4] (red: rms identical across a 1.5x drive span).
* **Ensemble**: fb ±0.9 raw addSat loop — the phaser pre-fix signature twice
  over (no HF damp: near-Nyquist resonance at the depth-clamp dl=1.0 dwell;
  no DC killer: 10x DC loop gain). Full phaser treatment (per-line 4-pole
  damp + knee + killer).
* **ClockedDelay**: no in-loop HF damping (tapeLp_ ages the INPUT branch
  only); integer tempo reads + fb 0.95 => even-parity Nyquist pole, 20x
  bound. A 2-pole 5 kHz damp was added in the loop.
* **Spring A**: delay 1146 (EVEN) + six odd chirp APs => total loop phase 0
  mod 2pi at Nyquist with 0.60 gain on one damp pole — the measured-
  insufficient regime. The delay is now 1145 (odd => negative fb). Measured
  by fx-invariants [I2b]: 1.65x -> 0.71x at 15.9 kHz.
* **Flanger**: LoopDcKiller added to the return branch (12.5x DC loop gain;
  placed in the FEEDBACK component — a kill of the whole write added a phase
  lead that flipped a near-tie lag measurement in drive_calib [7]).
* **Wavefolder bias DC**: constant bias into the fold held the wet path at
  lut(x2*gain) ~= ±0.62 DC. The fix subtracts the SAME fold evaluated at the
  reference point — exact, stateless (no transient: keeps the RAW-param
  contract that HARDEN-2b pins).

New invariant layers in tests/parvati_fx_invariants_test.cpp: [I2] extended
to Chorus/Ensemble; **[I2b]** near-Nyquist loop gain (a quiet 15.9 kHz probe
straight into processSampleFx — bypassing the bridge's 15 kHz output LP —
must stay <= 3x); **[I4]** Drive-knob resolution (distinct drive settings
must produce distinct output; red-validated 0.2298/0.2298 identical
pre-fix).

Documented residuals (deliberate): Echo's worst-corner Nyquist loop gain
0.447 (below the 0.5 positive-fb threshold, ~1.8x peaking — thin but safe);
the master EQ +12 dB high shelf multiplies the bridge floor band ~4x
post-effect (character, stacks statically); LUT Jitter's readFrac
floor-pinning at Jitter=1 (character-level, no loop).

### Phaser regen HF damp (2026-08-21 — the "100% params crackle")

User repro: all phaser params at 100% — crackle with NO modulation (dragging
Center was incidental). Mechanism: six cascaded 1st-order allpasses give ~pi
phase each at HF (6*pi == 0 mod 2*pi) = POSITIVE feedback near Nyquist. At
fb 0.9, the RateBridge's linear-resampler artifact floor (~0.025 worst
impulse, present in the no-FX engine too) amplified ~4x into 0.096 ticks
that pulse with the 8 Hz sweep. Fix: FOUR cascaded one-pole LPs at 5 kHz in
the regen return (measured ladder: 1 pole 0.063, 2 poles 0.034-0.046 — the
artifacts are broadband; 4 poles = -24 dB/oct pin every above-damp resonance
at net-negative gain). The musical notch band (<= 3.5 kHz at max
Center+Depth) is untouched — analog regen stages are band-limited too.
Pinned by parvati_phaser_crackle_test (red 0.096 pre-damp; passes at the
0.025 baseline floor = parity with the engine's own lo-fi character).

### Math-invariant tests (2026-08-21): catching the memory-safety-blind class

`tests/parvati_fx_invariants_test.cpp` encodes the LAWS that each effect
class must obey, at parameter EXTREMES (every bug below lived there — the
existing suites probe typical settings). It caught two real bugs on its
first run:

* **[I1] Curve audit**: wavetable edge entries ~0 ⟺ the shape is periodic
  (the LUT gating invariant); the healed Fuzz knee must taper to ~0; Sparse
  must pass exactly through 0 (odd symmetry).
* **[I2] Loop DC-freeness**: every feedback effect at MAX regen, fed a
  SATURATING-HOT ASYMMETRIC (DC-free) input, must emit |mean| <= 0.1*rms. A
  pure sine clips symmetrically and passes even pre-fix. The rectification
  DC source is asymmetric program material through the loop rail (the
  user's chord wash). First run caught: ClockedDelay's Grit TRUNCATION
  quantization (systematic DC; now blocked at the wet output — the AND-MASK
  character stays) and Phaser's feedback rectification at max fb
  (|mean|/rms 0.136 -> soft-knee + LoopDcKiller in the return path ->
  0.048).
* **[I3] Param fuzz**: random extreme param vectors -> finite, bounded,
  non-degenerate output (the attack silence of delays/reverbs is exempt;
  the analysis uses the final 30%).

Red-validated: the USER REPRO rows in parvati_fx_lut_dropout_test fail
0.002-0.03 on the pre-killer tree; I2 phaser fails 0.136 pre-fb-killer.

### Native-shape quality pass (2026-08-21, second wave)

* **Overdrive**: the drive gain ladder is UNSATURATED (64-bit intermediate,
  |v| <= 2^27), exactly like the LUT fix above. The old Q.23-saturating
  doublings pinned the table index at the rail for Drive >= ~8x. They
  squared the signal instead of folding it through the tube curve's droop
  tail. Max-drive now reads the real curve; pinned by the Overdrive row in
  parvati_fx_lut_dropout_test.
* **LUT shapes**: Fuzz (#5) and Sparse (#15) had hard C0 discontinuities in
  their transfer curves (a knee step at x=0.12; a zero-crossing flip with a
  ~400x slope) — step artifacts that no oversampling can repair. Fuzz now
  cosine-tapers below the knee; Sparse is the continuous
  x/(|x|+0.05)^0.7 expander (same intent, no discontinuity).
* **Flanger**: the regen loop write is SOFT-SATURATED (float-domain knee,
  transparent to +/-0.6, tanh to the rail). At fb 0.92 the loop resonates
  ~12.5x, and the old f24_addSat hard-clipped every recirculation —
  measured -58 dB inharmonic foldback on a pure sine
  (tests/parvati_fx_foldback_probe); -64 dB with the knee. The jet
  self-oscillation character is preserved.
* **Harness note**: test binaries that construct juce objects without
  ScopedJuceInitialiser_GUI bind the MessageManager to a background thread.
  Every APVTS write then takes the DEFERRED param path and, with no message
  loop, silently never reaches the engine (the FX appear "not to engage").
  Probes must initialise the GUI and (for extra safety) call
  syncAllParamsToEngine().

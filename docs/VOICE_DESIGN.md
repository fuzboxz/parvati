# The Design of the Ambika Digital Voice

This article is a detailed technical description of the **digital voice** at the
center of the [Mutable Instruments **Ambika**](https://github.com/pichenettes/ambika)
hybrid polysynth. It also describes how [**Parvati**](../README.md) implements
this voice again in software. The article starts with some history and shows a
block diagram early. It then builds up the design one component at a time. Each
choice comes with an explanation of the *why*.

If you have read about the Juno's digitally-controlled oscillators, you already
know one part of this design: a microcontroller gives an unstable analog
oscillator a very stable pitch. Ambika takes this idea further and asks a
sharper question — *how much of a synthesizer voice can you put inside a small
microcontroller, and how much do you deliberately keep in the analog domain?*
The answer is the "hybrid voice". This is the central idea of this article.

> The DSP described here lives, byte-for-byte, under [`Source/dsp/`](../Source/dsp).
> Parvati keeps it **integer and bit-exact** with the original Ambika firmware
> (in [`ambika_reference/voicecard/`](../ambika_reference), not part of the
> tracked source). Real constant and function names are given inline, so you
> can follow them in the code.

---

## A little history

Ambika was released around 2012 by **Emilie Gillet** (Mutable Instruments). It
is the direct descendant of the **Shruthi-1**. The Shruthi-1 is a single-voice
hybrid monosynth. It uses the same digital voice, which runs on a small 8-bit
AVR microcontroller with one analog filter board. Ambika is, in essence, *six
Shruthi voices in one box*. A motherboard "controller" handles MIDI, patch
storage, the arpeggiator/sequencer and voice allocation. Up to six
**voicecards** complete the unit. Each voicecard is a Shruthi-class hybrid
voice (a microcontroller + an analog filter + an analog VCA).

This heritage explains the architecture. A pure-analog polysynth of the early
1980s — for example a Jupiter-8 — has many voltage-controlled oscillators.
Each one is a hand-tuned analog circuit, and all of them share one control
bus. It sounds excellent, but it is very difficult to keep in tune and to make
polyphonic. A modern **virtual-analog** (VA) synth removes the analog parts
completely. It computes everything — oscillators, filters, VCAs — as
floating-point DSP. It is perfectly stable and perfectly flexible. Many people
also feel that it can sound *too* clean, because the filter is a mathematical
ideal and not a real circuit.

Ambika is the **hybrid** compromise, and it is a careful one:

* Everything that needs to be **precise and flexible** — the oscillators,
  their waveforms, the modulation matrix, the envelopes, the LFOs, the
  sequencer — is computed **digitally** in the voicecard's microcontroller.
* The one part that gives a synth its *character* — the **resonant filter** —
  stays **real analog hardware**, one per voicecard. The final
  **voltage-controlled amplifier (VCA)** also stays analog.

The Juno used a microcontroller to *control* an analog oscillator. Ambika uses
a microcontroller to *replace* the analog oscillator completely and to *drive*
an analog filter. The microcontroller is the voice. The analog filter and VCA
give the character.

> **Why Parvati implements this again.** The Ambika firmware is GPL-3.0, and
> Parvati is a faithful port. This article exists for the same reason that the
> integer DSP stays bit-exact instead of a float "cleanup": every quirk (the
> 8-bit centring, the ÷256 mixers, the 24-bit phase, the control-rate block
> size) is part of the *sound*. A change in the math changes the synth.

---

## An overview of Ambika's sound generation

Here is the overview before the details. One Ambika voice produces sound in
this order:

```
                     ┌────────────────────  DIGITAL  (the microcontroller)  ────────────────────┐
                     │                                                                          │
  note +             │  ┌──────────┐   ┌──────┐                       ┌──────────┐              │
  modulation  ──────▶│  │ 2× OSC   │──▶│ MIXER│── (ring/sync/sub/noise)│ 8-bit    │   cutoff ──┐  │
  (pitch, env,       │  │ (phase + │   └──────┘                       │ audio    │   reso  ──┤  │
   lfo, matrix)      │  │ wavetables)                                │ out      │   mode  ──┤  │
                     │  └──────────┘                                 │ (≈128)   │            │  │
                     │                  modulation matrix ──┐        └────┬─────┘            │  │
                     │                  (3 ENV/LFO, voice   │             │                  │  │
                     │                   LFO, 14 routings)  │             │                  │  │
                     └──────────────────────────────────────┼─────────────┼──────────────────┘  │
                                                               │             │                     │
                                                  8-bit sample │             │   8-bit CVs         │
                                                               ▼             ▼                     │
                     ┌────────────────────  ANALOG  (the voicecard hardware)  ────────────────┐ │
                     │                                                                         │ │
                     │      ┌─────────────┐   mode    ┌─────────┐              ┌─────┐         │ │
                     │      │  DAC + S/H  │─────────▶ │ ANALOG  │ ──audio────▶ │ VCA │ ──out──▶│ │
                     │      │  (8-bit)    │           │ FILTER  │              │     │         │ │
                     │      └─────────────┘           │ (LP/BP/ │   ◀── VCA CV │     │         │ │
                     │                                │  HP/Not)│   (mult. by │     │         │ │
                     │                                └─────────┘    env/wheel)└─────┘         │ │
                     └─────────────────────────────────────────────────────────────────────────┘ │
                                                                                                   │
                                                                                          voicecard output
```

Two points are important, because they drive every decision that follows:

1. **The digital part ends at an 8-bit number centred on 128.** Every
   waveform, every mix and every modulation is an 8-bit unsigned value. In
   this format, `128` means silence (the virtual ground). This value goes to
   the analog side.
2. **The filter and the VCA are analog and sit *after* the DAC.** The
   microcontroller never filters anything. It only *computes* the filter's
   cutoff, resonance and mode as small integers and sends them to a DAC,
   exactly as it sends the audio. The VCA gain is likewise an analog
   multiplier. The microcontroller computes the control value that drives it.

Compare this with the two pure approaches:

```
  PURE ANALOG (VCO)              AMBIKA (hybrid)                 PURE VA (virtual analog)
  ─────────────────              ──────────────                  ────────────────────────
  VCO ─▶ analog filter ─▶ VCA    OSC(digital) ─▶ analog filter   OSC(float) ─▶ filter(float)
        everything analog        ─▶ analog VCA                    ─▶ VCA(float): everything DSP
        (warm, drifts,           (stable, flexible osc +          (stable, flexible, "clean";
         huge, expensive)         characterful analog filter)      filter is a math model)
```

The hybrid concept gives you the **flexibility of a digital oscillator** (15+
waveform algorithms, wavetables, FM, CZ-style phase distortion, a full
modulation matrix) *and* the **character of a real analog filter**
(self-oscillation, drive, per-voicecard variation) in one low-cost, stable
voice. The rest of this article describes the inside of that "digital" box.

---

## The internal sample rate, and why it is 39216 Hz

Everything digital in an Ambika voice runs at one fixed sample rate. Parvati
names it `kInternalSampleRate`, and the value is deliberate:

```cpp
// Source/dsp/constants.h
static constexpr double kInternalSampleRate = 39216.0;
```

Where does 39216 Hz come from? The voicecard does **not** have a dedicated
audio DAC. It makes its analog output with the microcontroller's own hardware:
an 8-bit, phase-correct **PWM** (pulse-width modulation) peripheral, clocked
directly from the CPU. For an ATmega that runs at 20 MHz in phase-correct PWM
mode with an 8-bit counter (TOP = 255), the PWM carrier frequency is

```
   F_CPU / (2 × TOP) = 20 000 000 / (2 × 255) = 39 215.7 Hz
```

That carrier *is* the audio sample rate. The analog filter/VCA are low-pass,
so they remove the PWM carrier and leave the audio. The pitch increment table
— `lut_res_oscillator_increments` — was generated for exactly this rate. A
check of a known note in the table (MIDI 116 ≈ 6645 Hz) gives ~39218 Hz and
confirms it. (Later, Parvati resamples this fixed-rate signal up or down to
the sample rate that the host DAW uses. See the end of this article.)

### Why integer, fixed-point math?

The voicecard MCU is an 8-bit AVR with **no floating-point unit**. The
complete voice is therefore computed in **integer fixed-point arithmetic**.
The audio path is **8-bit, centred on 128**:

* a sample of `128` is silence (the virtual ground / PWM 50% point);
* `0` and `255` are full negative / full positive swing.

Parvati does **not** "modernize" this into floats. It keeps the integer math
identical, in [`Source/dsp/fixed_math.h`](../Source/dsp/fixed_math.h), because
the quirks *are* the sound. These helpers matter throughout:

| Helper | What it does | Why it matters |
|--------|--------------|----------------|
| `U8Mix(a,b,balance)` | `(a·(255−balance) + b·balance) >> 8` | The `>>8` is a **÷256**, not ÷255. So `U8Mix(255,x,0) == 254`, never 255. This off-by-one occurs everywhere. |
| `InterpolateSample(table, phase)` | linear interp over a 257-entry table | `phase` is 16-bit; top byte = index, low byte = fractional blend. |
| `U8U8MulShift8(a,b)` | `(a·b) >> 8` | An 8-bit × 8-bit **gain/attenuation**: `U8U8MulShift8(x,255) ≈ x`, `...,128) ≈ x/2`. The VCA is built from these. |
| `S8U8MulShift8(a,b)` / `S8S8Mul(a,b)` | signed × unsigned multiplies | For bipolar (AC) modulation amounts. |
| `U24AddC(phase, inc)` | 24-bit add that reports a **carry** bit | The code detects oscillator **sync** through this carry (see below). |

### The control rate: why CVs update only once per 40 samples

A final fundamental constant defines the *feel* of the synth:

```cpp
// Source/dsp/constants.h
static constexpr uint8_t kControlRate     = 40;
static constexpr uint8_t kAudioBlockSize  = kControlRate;   // == 40
```

Ambika does **not** recompute every modulation source and every filter cutoff
on every single sample. Instead, the voice renders a block of **40 samples**
at a time. The modulation matrix — envelopes, LFOs, the 14 routings, the
filter cutoff/resonance — is evaluated **once per block**. So it runs once
every 40 samples (about 980 times per second). The 40-sample audio block is
then filled from those frozen control values. The firmware comment states
this plainly: *"One control signal sample is generated for each 40 audio
samples."*

This is the digital copy of the analog world that the Juno article describes,
where a control voltage charges a capacitor. Here the "control voltage" is an
8-bit value that stays constant for ~1 ms. It also causes Ambika's slightly
*stepped* modulation character. LFOs and envelopes quantize to ~1 ms; this is
audible and intentional. Parvati's `ProcessBlock()` honours this exactly:
`LoadSources → ProcessModulationMatrix → UpdateDestinations` run once, then
`RenderOscillators` fills the 40-sample block.

> The whole `Patch` struct — every oscillator, every modulation routing, every
> envelope — is exactly **112 bytes** (enforced by `static_assert(sizeof(Patch)
> == 112)` in `constants.h`). This is the complete sonic identity of one
> voice, and it is very small. The `.PRO` patch files that you load and save
> are exactly those 112 bytes (plus an 84-byte Part block).

---

## The oscillator engine

The internal sample rate and the fixed-point math are the foundation. The
oscillator is the central part. This is Ambika's analogue of the Juno's "ramp
generator": the part that turns *pitch* into a repeating waveform.

### The 24-bit phase accumulator

Every oscillator in Ambika is, at its core, a **phase accumulator**. The phase
is a 24-bit fixed-point number — 16 integer bits and 8 fractional bits. It is
stored in two halves:

```cpp
// Source/dsp/oscillator.h
uint24_t phase_ {};          // { integral: 16 bits, fractional: 8 bits }
uint24_t phase_increment_ {};
```

At each audio sample, the phase advances by `phase_increment_`. The top bits
then look up a waveform. The fractional 8 bits give sub-sample pitch
resolution, so notes track in tune. The 16 integer bits index a 257-entry
wavetable (the extra sample gives clean wrap-around interpolation). The
fundamental render loop, copied faithfully from the firmware, is:

```cpp
// oscillator.cpp, the UPDATE_PHASE macro (verbatim from the firmware)
if (*sync_input_++) { phase.integral = 0; phase.fractional = 0; }  // hard sync
phase = U24AddC(phase, phase_increment_int);
*sync_output_++ = phase.carry;   // a 24-bit overflow => "I wrapped"
```

That `carry` bit is small but important: it is the mechanism of **oscillator
sync**. One oscillator can reset the phase of another to zero every time it
wraps. This is the classic sync timbre. The `U24AddC` adder returns a carry
flag exactly so that the two oscillators can pass it sample-by-sample.

### Pitch → increment: the pitch table and octave shifting

So how does a *note* become a `phase_increment_`? This is the digital
equivalent of the Juno clock-frequency choice. It uses a precomputed **pitch
table** plus **octave shifting** — an elegant method that keeps the table
small.

Pitches are stored as **14-bit** values: the top 7 bits are the MIDI note, the
low 7 bits are fine tuning in 1/128-semitone steps (`kLowestNote` …
`kHighestNote = 120·128`). The pitch table `lut_res_oscillator_increments`
only covers the *top* octave-plus of the range:

```cpp
// constants.h
static constexpr int16_t kPitchTableStart = 116 * 128;   // table covers MIDI 116..127
static constexpr int16_t kOctave          = 12 * 128;
```

To play a lower note, read the table at the equivalent pitch within the
table's range. Then **halve the increment once per octave down**:

```cpp
// voice.cpp, RenderOscillators() — the pitch→increment conversion
int16_t ref_pitch = pitch - kPitchTableStart;
uint8_t num_shifts = 0;
while (ref_pitch < 0) { ref_pitch += kOctave; ++num_shifts; }   // fold up into range
uint24_t increment;
increment.integral = Lookup(lut_res_oscillator_increments, ref_pitch >> 1);
while (num_shifts--) { increment = U24ShiftRight(increment); }  // ÷2 per octave down
```

`U24ShiftRight` is a single-bit right shift of the 24-bit phase. A halved
frequency is exactly one octave lower. This is why the table needs to cover
only one octave: octave-shifting derives every other note. It is the same
idea as the Juno method of one high-frequency clock with division. Ambika
applies it in the phase-increment domain instead of the time domain.

The oscillator's note value (`note_`) also feeds a second, subtler mechanism.
It is closely related to the Juno's **amplitude compensation** — see the next
section.

### The waveshapers: bandlimited multi-zone wavetables (Ambika's amplitude compensation)

Recall from the Juno article that a naive DCO gets *quieter at high
frequencies*, because the integrator has less time to charge each cycle. The
fix is **amplitude compensation** — the integrator gets a bigger voltage for
higher notes. Ambika has the opposite problem. It uses a technique called
**multi-zone bandlimited wavetables**.

The problem: at a 39216 Hz sample rate, Nyquist is ~19.6 kHz. A naive
sawtooth contains harmonics well above that frequency. The top octaves would
therefore **alias** badly (scratchy, inharmonic content). The fix keeps
*several copies* of each waveform. Each copy has **fewer harmonics** than the
last. The oscillator picks the correct copy for the note's pitch:

```cpp
// constants.h
static constexpr uint8_t kNumZonesFullSampleRate = 6;   // 6 bandlimited zones
static constexpr uint8_t kNumZonesHalfSampleRate = 5;
```

`RenderSimpleWavetable` (the algorithm behind the saw / square / triangle
waveshapes) does exactly this. It derives a `balance_index` from the note. The
low nibble selects which two adjacent zones to read. The high nibble sets the
**crossfade gain** between them:

```cpp
// oscillator.cpp, RenderSimpleWavetable()
uint8_t balance_index = U8Swap4(note_);           // split note into zone + blend
uint8_t wave_1 = base_resource_id + (balance_index & 0xf);
uint8_t wave_2 = base_resource_id + U8AddClip(balance_index & 0xf, 1, kNumZonesFullSampleRate);
... // then each sample: InterpolateTwoTables(wave_1, wave_2, phase, gain_1, gain_2)
```

A high note therefore crossfades into a lower-harmonic zone (less aliasing).
The pitch-driven crossfade is smooth, not stepped. This is Ambika's answer to
"keep the waveform clean across the whole keyboard". It has the same goal as
the Juno's DAC amplitude tracking, solved in the wavetable domain.

### The fifteen waveform algorithms

The oscillator dispatches on the `shape` byte through a function table
(`fn_table_[]` in `oscillator.cpp`). The algorithms form a few families. They
show the range of what "a digital oscillator" can be without the limits of
analog circuitry:

**The analog-emulating wavetables** — `WAVEFORM_SAW`, `WAVEFORM_SQUARE`,
`WAVEFORM_TRIANGLE`, `WAVEFORM_SINE`. These are the multi-zone bandlimited
tables above. `SQUARE` is special: at parameter 0 it is a plain bandlimited
square. With a non-zero parameter it becomes `RenderBandlimitedPwm` — a true
**pulse-width modulation**. It subtracts a phase-shifted copy of the waveform
from itself. This is the method that turns a square into a pulse.

**The Casio CZ family** — `WAVEFORM_CZ_SAW`, `_CZ_SAW_LP/_PK/_BP/_HP`,
`_CZ_PLS_LP/_PK/_BP/_HP`, `_CZ_TRI_LP`. These are **phase-distortion**
synthesis, the technique from the Casio CZ-101: the code does not read a
fixed wavetable with a linear phase. Instead, it *warps the phase itself*
before it looks up a sine. `RenderCzSaw` is the simplest — it bends the
phase, so the sine readout becomes a saw. The resonant variants
(`RenderCzResoSaw/Pulse/Tri`) add a second, faster-running "resonator" phase
that resets each cycle. This produces the resonant tones for which the CZ is
famous. Each variant comes in LP/PK/BP/HP window flavours; these select how
the code windows the resonator. You cannot get this sound from an analog
oscillator without a second oscillator and a ring modulator. Here it is
*free*, because the phase is only a number.

**FM** — `WAVEFORM_FM`. A two-operator FM: a modulator sine at a frequency
ratio (from `lut_res_fm_frequency_ratios`) phase-modulates a carrier sine.
The `parameter` controls the modulation depth. `range` serves as the FM ratio
parameter (note in `RenderOscillators`: FM skips the usual range-as-octave
offset). Again, this is possible only in the digital domain.

**The 16 wavetables + wavequence** — `WAVEFORM_WAVETABLE_1..16` and
`WAVEFORM_WAVEQUENCE`. `RenderInterpolatedWavetable` reads a small
per-wavetable *definition* (a list of single-cycle waves). It **interpolates
between adjacent waves based on the parameter**. A parameter sweep therefore
scans through the wavetable. `RenderWavequence` plays one selected wave.
Wavetable scanning is the backbone of modern "wavetable synths". Ambika had
it on an 8-bit MCU.

**Noise and the rest** — `WAVEFORM_FILTERED_NOISE` (a one-pole low-pass then
high-pass over its own Galois LFSR, `RenderFilteredNoise`), `WAVEFORM_VOWEL`
(a formant/vocal-tract synthesizer that runs a Cantarino-style three-formant
model, `RenderVowel`), `WAVEFORM_QUAD_SAW_PAD` (four detuned saws summed for
thick pads, `RenderQuadSawPad`), `WAVEFORM_8BITLAND` (deliberately distorted
bit arithmetic for chiptune effects, `Render8BitLand`), and
`WAVEFORM_DIRTY_PWM` (a raw comparator PWM with audible aliasing,
`RenderDirtyPwm`).

The purpose of this list is not memorization. It shows the central idea:
*because the oscillator is a program, not a circuit, Ambika can offer a
waveform for every purpose, from analog-accurate saws to formant speech. All
of them come from the same small phase accumulator.* This breadth is the
benefit of the digital approach.

---

## The mixer

The two oscillators do not go straight to the output. They meet in a **mixer**
that is itself a sound-design tool. The mixer's parameters are the `mix_*`
fields of the `Patch` (`patch.h`):

* `mix_balance` — the crossfade between oscillator 1 and oscillator 2. In
  `ProcessModulationMatrix` this becomes two complementary gains:
  `osc_2_gain = U14ShiftRight6(dst_[MIX_BALANCE])` and
  `osc_1_gain = ~osc_2_gain`. Note the bitwise complement — the gains always
  sum to 255, an equal-power-ish crossfade.
* `mix_op` — the **operator** that combines them (`enum Operator`): `OP_SUM`
  (plain mix), `OP_SYNC` (osc 2 hard-synced to osc 1 — using those sync carry
  bits), `OP_RING_MOD` (analog-style ring modulation: the two 8-bit signals
  multiplied sample-by-sample), `OP_XOR`, `OP_FOLD` (wavefolding distortion)
  and `OP_BITS` (bit-crushing).
* `mix_sub_osc` + `mix_sub_osc_shape` — a **sub-oscillator**
  (`sub_oscillator.h`) that runs one octave below oscillator 1, selectable as
  square / triangle / pulse (with a second set one octave lower still). It is
  mixed in by amount, exactly like a third oscillator that adds bass weight.
* `mix_noise`, `mix_fuzz`, `mix_crush` — white-noise level, a fuzz-style
  waveshaper, and a bit-crusher. For `mix_crush`, `UpdateDestinations`
  computes `(dst_[MIX_CRUSH] >> 8) + 1`. The actual crushing happens in the
  per-sample output, by masking low bits.

A **transient generator** (`transient_generator.h`) can also start on
note-on. It layers a short percussive click/blow/metallic hit — the `Click`,
`Glitch`, `Blow`, `Metallic`, `Pop` sub-oscillator shapes. It decays over 255
samples and then stops. The sustained oscillators continue.

---

## The modulation architecture

This is where Ambika shows its depth. The voice has a remarkably complete
modulation system for an 8-bit chip. It is useful to understand it as a
whole.

### The three envelope/LFO units (a duality)

There are **three** identical slots, `env_lfo[3]` (`kNumEnvelopes == 3`). The
clever part: **each slot is either an envelope *or* an LFO**, selected by its
`shape` byte. This duality saves silicon, because the same timing hardware
serves both purposes.

* As an **envelope**, it is an ADSR-with-a-DEAD-stage generator
  (`Source/dsp/envelope.h`). Internally, it is a 16-bit accumulator `value_`
  with a range of 0…65025. An exponential curve from a portamento lookup
  table advances it. `Render()` returns `value_ >> 8`, i.e. 0…254 (note the
  ÷256 — the peak is 254, not 255; another faithful quirk). It walks ATTACK
  → DECAY → SUSTAIN → RELEASE → DEAD and snaps to each stage target on
  16-bit phase wraparound. DEAD forces the accumulator to zero. This is how a
  voice goes fully silent.
* As an **LFO**, the same slot renders a low-frequency modulation oscillator
  (`Source/dsp/lfo.h`) — triangle, square, sample-and-hold, or ramp — with a
  16-bit phase accumulator and optional **tempo sync** (rates below
  `kNumSyncedLfoRates = 15` lock to the host BPM via the
  `midi_clock_tick_per_step` table).

There is also a dedicated **voice LFO** (`MOD_SRC_LFO_4`) that is always an
LFO, never an envelope. So in total a voice has up to three envelopes *or*
LFOs plus one extra LFO.

### The modulation matrix

These sources are connected to the voice's parameters by a **14-routing
modulation matrix** (`kNumModulations == 14`, the `modulation[14]` array).
Each routing is `{ source, destination, amount }`:

```cpp
// patch.h
struct Modulation { uint8_t source; uint8_t destination; int8_t amount; };
```

`source` can be any of 31 things (`MOD_SRC_*`: the three env/LFOs, the four
LFOs, four operator outputs, two sequencer tracks, velocity, aftertouch,
pitch bend, the two mod wheels, expression, note, gate, noise, random, and a
set of constants). `destination` is one of 19 (`MOD_DST_*`: the oscillator
parameters, their coarse/fine pitch, the mixer balance/noise/sub/fuzz/crush,
the filter cutoff and resonance, the envelope attack/decay/release, the
voice-LFO rate, and — crucially — the VCA).

The matrix is processed once per block in `ProcessModulationMatrix`. It
respects a subtle but musically vital distinction: **AC vs DC coupling**.

* **DC-coupled** sources (envelopes, velocity, the wheels) are unipolar 0…255
  and are scaled by a signed amount with `S8U8Mul`.
* **AC-coupled** sources (the LFOs, pitch bend, note) are bipolar, centred on
  128, and are scaled with `S8S8Mul` after subtracting the 128 bias
  (`source_value + 128`). This is the difference between "an LFO wobble above
  and below the current value" and "an envelope that moves the value in one
  direction only".

> **Why is this distinction fixed in the code?** It models the analog
  patchbay. An envelope is a positive-going voltage that you route to open a
  filter. An LFO is a bipolar voltage that you route to wobble a pitch. The
  hardcoded coupling per source class has two benefits. The user does not
  have to think about it. The code also avoids per-routing bias bookkeeping.

### The VCA is a modulation destination

Here is the detail that ties the whole digital/analog split together, and the
most important one: **the VCA is just another destination in the matrix
(`MOD_DST_VCA`), but the code treats it specially — multiplicatively.**

Most destinations are *additive*: the routed modulation is added to a base
value (the cutoff, the pitch, etc.). The VCA is *multiplicative*: each
routing that targets it **multiplies** the current VCA level. This is exactly
what an analog VCA (an OTA — operational transconductance amplifier) does. It
is a voltage-controlled *multiplier*, not an adder. The init patch shows the
idiom with `{ MOD_SRC_ENV_2, MOD_DST_VCA, 32 }`: envelope 2 scales the
volume, and the part volume sets the baseline:

```cpp
// voice.cpp — the VCA branch of the modulation matrix
// (modulation_destinations_[MOD_DST_VCA] starts at part_.volume << 1)
if (amount < 0) { amount = -amount; source_value = 255 - source_value; }  // invert
if (amount != 63) source_value = U8Mix(255, source_value, amount << 2);   // scale by amount
modulation_destinations_[MOD_DST_VCA] = U8U8MulShift8(vca_level, source_value); // multiply
```

So the "amplitude envelope" is not a special-case stage at the end of the
signal chain. It is a routing into the VCA, computed in the same matrix as
everything else. This exactly mirrors the analog VCA, which sits at the end
and is driven by a control current. The final `vca()` value (0…~254) goes to
the analog VCA.

### Modifiers, the sequencer, and the skip-to-silence

Two more modulation sources complete the system:

* **Four modifiers** (`modifier[4]`, `kNumModifiers == 4`). These are tiny
  **operators** (`enum ModifierOp`): SUM, PRODUCT, ATTENUATE, MAX, MIN, XOR,
  GE, LE, QUANTIZE, and a one-pole LAG_PROCESSOR. Each takes two modulation
  sources as operands and produces a derived source (`MOD_SRC_OP_1..4`) that
  you can then route anywhere. They are the "logic/math" layer that you can
  use to build complex modulations (e.g. "the LFO, but only while the
  envelope is above halfway").
* **The step sequencer** (`MOD_SRC_SEQ_1/2`) — two modulation sequences plus
  a note sequence, advanced by the transport, usable as modulation sources.
  (The arpeggiator and sequencer are controller-side features in Ambika;
  Parvati ports them too, but they feed the same matrix.)

Finally, a small but important optimization: if the computed `vca()` value
falls below 2 (essentially silent), `ProcessBlock` takes a shortcut. It
writes a block of silence (`128`) and does not render the oscillators at all
(`voice.cpp`, the `if (vca() < 2)` early-out). A silent voice costs almost no
CPU. This is how 16 voices can run on modest CPU. (Parvati also frees the
voice once all three envelopes reach DEAD — `envelopesDead()`. A patch with
no ENV→VCA routing therefore still releases cleanly.)

---

## From digital to analog: the filter and the VCA

We have now followed the signal all the way to the 8-bit output centred on
128, plus a small set of computed control values (cutoff, resonance, mode,
VCA gain). Here the design crosses from the digital microcontroller into the
**analog voicecard hardware**. This is the one place where Parvati *cannot*
be a bit-exact port, because the firmware contains **no filter code at
all**.

> *There is NO filter code in the Ambika firmware: the filter is ANALOG hardware
> (one per voicecard). The voice DSP computes cutoff / resonance (8-bit) and a
> 2-bit filter mode and sends them to a DAC + parallel port.*
> — [`Source/dsp/analog_filter.h`](../Source/dsp/analog_filter.h)

So the firmware's job ends at computing those integers. Parvati is a plugin
with no analog hardware, so it must **emulate** the filter and VCA in
software. It does so with new code (not a port) in `analog_filter.{h,cpp}`.
The code models the three real voicecard filter boards that Ambika shipped:

| Voicecard board | Filter | Parvati model |
|-----------------|--------|---------------|
| SMR4 (LM13700 OTA) | 4-pole low-pass OTA cascade | Parvati models it with the Ladder card: `juce::dsp::LadderFilter`, 24 dB/oct, tanh saturation, self-oscillating (an approximation — the OTA cascade is not natively modeled) — plus a dedicated SMR4 card: custom OTA-cascade model (this repo), per-stage `gm*tanh(error/2Vt)` |
| 4-pole SSM2164 | 4-pole cascade | a custom 4-stage one-pole cascade with feedback — "politer" than the ladder |
| 2-pole SVF (SSM2164) | state-variable filter | `juce::dsp::StateVariableTPTFilter` — the only one that honours LP **/BP/HP/Notch** |

Some engineering details matter for the *character*:

* The voice's 8-bit cutoff is converted to **Hz** via
  `AnalogFilter::cutoffByteToHz` (an exponential mapping, because the
  hardware OTA is exponential V/Hz). Resonance is scaled to 0…1 and **capped
  below self-oscillation** (`kMaxResonance = 0.95f`) for stability. The
  bounds are `kMinHz = 20` / `kMaxHz = 16000`.
* The 4-pole boards are **low-pass only** (hardware-accurate); only the SVF
  honours the BP/HP/Notch modes. Parvati enforces this.
* The **VCA** is applied after the filter, just like the hardware. It has two
  response curves (`AmbikaVoice.cpp`), which mirror the firmware's log/lin
  jumper: a *linearized* mode (`gain = vca/255`) and an *exponential* ~60 dB
  OTA taper (`gain = 10^((vca/255 − 1)·3)`). The exponential mode gives the
  classic analog VCA "snap".

The signal handoff looks like this in `AmbikaVoice::fillInternalBlock`:

```cpp
// AmbikaVoice.cpp — 8-bit (centred 128) → float → analog filter → VCA → FIFO
float s = (static_cast<int>(out[i]) - 128) / 128.0f;   // 8-bit → bipolar float
s = filter_.processSample(s);                           // modeled analog filter
s *= vcaGain;                                           // modeled analog VCA
fifo_.push_back(s);
```

So the digital 8-bit signal becomes audio, passes through the *modeled*
analog filter and VCA, and then goes into a FIFO. The FIFO is the last stop
before rate conversion.

---

## How Parvati implements it

Two points tie this back to Parvati specifically:

**1. The digital voice is a bit-exact port.** Everything in
[`Source/dsp/`](../Source/dsp) — the phase accumulators, the ÷256 mixers, the
24-bit carries, the envelope's `>>8` peak of 254, the control-rate block of
40 — matches the firmware integer-for-integer. The main structural change:
the firmware's globals (one voicecard = one voice, everything `static`)
become **per-instance members**, so Parvati can run 16 polyphonic voices. The
DSP math is otherwise untouched. The `Patch` struct is kept at exactly 112
bytes. The original `.PRO`/`.MUL` patch files therefore load and save
byte-for-byte.

**2. The analog part is a faithful *model*, resampled to the host.** The
engine is locked to 39216 Hz, but a DAW usually runs at 44.1/48/96 kHz. Each
voice therefore owns a **Lagrange resampler** (`AmbikaVoice`) that converts
the 39216 Hz internal signal to the host rate with sample accuracy.
Optionally, only the *filter model* can be **oversampled** 2× or 4× to reduce
its digital aliasing for higher fidelity (the oscillators stay at the
authentic 39216 Hz). This quality toggle leaves the oscillator character
intact. A master DC blocker protects the output against sub-audio drift.

The net result is a plugin that is, sample-for-sample, the Ambika digital
voice. Its analog filter and VCA are carefully reconstructed. Every quirk
stays as a feature rather than something that the code smooths away.

---

## Further reading

* **Ambika firmware** (the source of truth) — <https://github.com/pichenettes/ambika>,
  especially `voicecard/voice.cc`, `voicecard/oscillator.cc`, and
  `common/patch.h`. (GPL-3.0; Emilie Gillet.)
* **Shruthi-1** — Ambika's single-voice predecessor, the clearest explanation
  of the hybrid-voice idea: <https://pichenettes.github.io/mutable-instruments-elements/shruthi-1/>.
* **Parvati sources** —
  [`Source/dsp/oscillator.{h,cpp}`](../Source/dsp/oscillator.h) (the oscillator),
  [`Source/dsp/voice.{h,cpp}`](../Source/dsp/voice.h) (the per-block render +
  modulation matrix), [`Source/dsp/patch.h`](../Source/dsp/patch.h) (the
  `Patch`/`Part` layout + all enums), [`Source/dsp/envelope.h`](../Source/dsp/envelope.h)
  (the ADSR+DEAD envelope), [`Source/dsp/analog_filter.{h,cpp}`](../Source/dsp/analog_filter.h)
  (the modeled filter), and [`Source/AmbikaVoice.cpp`](../Source/AmbikaVoice.cpp)
  (the 8-bit → filter → VCA → resample path).
* **Parvati port spec** — [`docs/DSP_PORT_SPEC.md`](DSP_PORT_SPEC.md) for the
  full firmware→JUCE mapping and the filter-card topology notes.

---

*Parvati is a derivative work of the GPL-3.0 Ambika firmware and is licensed
under the GNU GPL v3.0. See [`LICENSE`](../LICENSE) and
[`NOTICES.md`](../NOTICES.md). The digital voice architecture described here
is the design of Emilie Gillet / Mutable Instruments. This article explains
that design as realized in Parvati's code.*

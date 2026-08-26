# DSP-core test-coverage gap audit (Source/dsp, excl. fx/ and clouds/)

Naming/build pattern to follow: `tests/<name>_test.cpp` → `add_executable(parvati_<name>_test ...)` linking `Parvati` (see CMakeLists.txt:673-690, lfo_sync_test).

**Headline:** NO test includes `dsp/oscillator.h`, `dsp/envelope.h`, `dsp/sub_oscillator.h`, `dsp/transient_generator.h`, `dsp/random.h`, or `dsp/fixed_math.h` directly. The whole DSP core is only exercised through engine/Voice-level tests.

## Prioritized missing tests (max 6)

1. **fixed_math.h pure functions (+ Random LFSR)** — Effort **S**
   - Pin: `U8Mix(255,x,0)==254` (documented ÷256 truncation, fixed_math.h:78-81); `S16ClipU14(16384)==16383`, `(-1)==0`, `(16383)==16383` (51-58); `U8AddClip` wrap→max; `U24AddC` carry=1 at 0xFFFFFF+1 and full 24-bit sum (156-175); `U24Sub` wraps at 24 bits, never negative (179-197); `U24ShiftLeft` drops bit 23; `InterpolateSample(table,0xFFFF)`==table[256] endpoint (210-213). Random: seed 0x21 → first `Update()` → 0xB410, `GetByte()`==0xB4 advances state, `state_msb()` doesn't (random.h:24-44).
   - Suggested: `tests/fixed_math_test.cpp` → `parvati_fixed_math_test`.
2. **TransientGenerator** — Effort **M** — zero coverage (grep "TransientGenerator" in tests/: no hits; lifecycle "transient" is UI lifetime).
   - Pin: `Trigger()` arms counter_=255 so Render mixes exactly 255 samples then stops (transient_generator.h:38,41-55); `shape < WAVEFORM_SUB_OSC_CLICK` is a no-op; `shape > POP` clamps to POP; CLICK: output 255 only while counter_<32 (last 31 samples), amplitude envelope = `U8U8MulShift8(counter_, amount)`; GLITCH first sample = 0*73+254 = 254 (deterministic LCG, rng_state_=0); POP returns value 0 with gain 255→0.
   - Suggested: `tests/transient_generator_test.cpp` → `parvati_transient_generator_test`.
3. **Envelope unit semantics** — Effort **S** (only env2 amplitude via Voice in hellcat_tests.cpp:293-312).
   - Pin: `Update(0,...)` gives lut[0]=65535 → first `Render()` completes ATTACK, peaks at **254 not 255** (÷256, envelope.h:101-118); sustain target = `sustain<<1` (127→254, :92-94); `Trigger(DEAD)` forces value_=0 → Render()==0 (:73-84); stage chain ATTACK→DECAY→SUSTAIN(holds, increment 0)→RELEASE→DEAD; Init() renders 0 and is trigger-ready.
   - Suggested: `tests/envelope_test.cpp` → `parvati_envelope_test`.
4. **SubOscillator** — Effort **M** — no direct tests (only engine sub-level 0 vs 63 in synth_param_coverage_test.cpp:450).
   - Pin: amount=0 still attenuates input by 1/256 (128→127) via `U8Mix(b, v, 255, 0)` (sub_oscillator.h:61-65); shapes 3..5 halve the increment (one octave down, :38-41); triangle folding second half (`phase_.integral & 0x8000`); square duty from pulse_width 0x80 vs pulse 0x40 for shape 2.
   - Suggested: `tests/sub_oscillator_test.cpp` → `parvati_sub_oscillator_test`.
5. **Oscillator hard-sync path (OP_SYNC + sync arrays)** — Effort **M** — zero coverage: no test mentions `OP_SYNC`, `sync_input`, or `sync_output` (only SUM vs RING_MOD, synth_param_coverage_test.cpp:434-448).
   - Pin: `UPDATE_PHASE` resets phase to 0 when sync_input byte non-zero and records carry on wrap (oscillator.cpp:44-56); at Voice level `mix_op==OP_SYNC` routes osc_1's `sync_state_` into osc_2 (voice.cpp:496) → osc_2 pitch locks to osc_1's period; square `parameter_==0` dispatches RenderSimpleWavetable not PWM (oscillator.h:63-70).
   - Suggested: `tests/osc_sync_test.cpp` → `parvati_osc_sync_test`.
6. **Voice pitch-path extremes** — Effort **M** — pitch tested only at notes 57/69/81 (hellcat_tests.cpp:242).
   - Pin: `pitch >= kHighestNote` clamp (voice.cpp:459-461) — notes 120*128 and 127*128 produce identical increments; `ref_pitch < 0` octave-shift loop for low notes+negative range (:463-476); `midi_note < 0` clamp to 0 (:478-482); `set_pitch_bend_offset(+128)` shifts detected pitch exactly +1 semitone (bypasses mod matrix, :444).
   - Suggested: `tests/voice_pitch_test.cpp` → `parvati_voice_pitch_test`.

## Already well covered (don't re-audit)

- All 15 osc shapes render finite under sanitizers, incl. wavetable/wavequence index paths (concurrency_test [1b], multi_load_test [7] hostile shapes) — correctness NOT asserted though.
- Saw/square audibility + pitch tracking 220/440/880 Hz (synth_param_coverage, parvati_tests [2]).
- Envelope attack loudness + release decay via engine (synth_param_coverage:640, parvati_tests [3]).
- LFO: tempo sync vs free-run (lfo_sync_test), slave retrigger (lfo_retrigger_test), range sanity (parvati_tests [4]).
- Voice Kill/idle silence/DC blocker (parvati_tests [3], idle_silence_test); legato (legato_test); polyphony + envelopesDead free (polyphony_test); slots (voice_slots_test).
- Mod-matrix source/destination routing to DSP (controller_mod_test, mod_audio_test, synth/fx param coverage).
- Patch layout: static_asserts 112/7 bytes (constants.h:73-83); serialization/roundtrip (patch_test, patch_load_test, roundtrip_*).
- AnalogFilter ladder tap-vs-JUCE bit-identity + SVF/SSM sanity (analog_filter_batch_test).
- Hostile patch-byte clamps at DSP edge (multi_load_test [7]); vendored resource tables verbatim (tuning_test §1).
- resources_manager.h/constants.h: pure pass-throughs + static_asserts, no behavioral surface left.

## Residual risks

- No golden/reference pinning of oscillator waveform samples (all algos only "finite"); fixed_math regression would silently corrupt every waveform.

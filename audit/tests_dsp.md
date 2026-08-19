# DSP-core test wave — 6 new unit tests (audit/gap_dsp.md items 1-6)

All six build green (`cmake --build build --target parvati_<name>_test`), pass (rc=0), and are UBSan-clean (`-fsanitize=undefined -fno-sanitize-recover=all`). JUCE-free direct-compile pattern (fv1-test style): only the referenced vendored .cpps are linked (`random.cpp`, `oscillator.cpp`, `voice.cpp`, `resources_data.cpp`); `sub_oscillator`/`transient_generator` are header-only targets. CMake blocks appended after `lfo_sync_test` (shared file — sibling lanes' additions intact, verified post-merge).

## Per-test pins

**fixed_math_test** — U8Mix ÷256 truncation family (255→254); S16ClipU14/U8/S8 corners; U8AddClip; bit fiddling; multiplies (incl. arithmetic-shift floor on negatives); U24AddC carry-at-0xFFFFFF, U24Sub 24-bit wrap, U24ShiftLeft bit-23 drop; InterpolateSample 257th-entry endpoint; Random LFSR (seed 0x21→0xB410→0x5A08, GetByte advances vs state_msb doesn't, global singleton).

**transient_generator_test** — shape<CLICK no-op; un-Trigger()ed no-op; exactly-255-sample decay across 7 calls (40×6+15, 8th call inert); hostile shape>POP clamps; CLICK value-255 burst over the LAST 32 samples with amplitude ramp ((255·ampl)>>8 exact sequence); amount scaling; GLITCH LCG samples (0·73+254=254, 254·73+253≡107); METALLIC sample 0.

**envelope_test** — table assumptions pinned first (env LUT[0]=65535, expo head 0,4 tail 255,255); full ATTACK(253,253)→DECAY(253)→SUSTAIN(hold, increment 0, never advances) chain; sustain target = sustain<<1 (64→127 vs 127→253); RELEASE→DEAD; Trigger(DEAD) forces 0; never-Updated envelope stuck at 0 (documents why Init() primes); slow-attack low start.

**sub_oscillator_test** — increment 0x80000 chosen so the 24-bit accumulator wraps every 32 samples inside one block: SQUARE_1 crossing at 15 AND wrap at 31; PULSE_1 duty (crossing 7) incl. the wrap-edge re-rise at 39; TRIANGLE fold (bit-15 branch, 256→0 truncation, wrap to ~0=255); shapes 3..5 octave halving (crossing 31 vs 15); amount 0 still attenuates 128→127; partial mix 95/159; phase persistence across blocks.

**osc_sync_test** — DirtyPWM threshold; sync_input resets phase BEFORE the increment (crossing shifts 15→23); sync_output carry == k&1 at inc 0x800000; SQUARE parameter 0 (wavetable, unpaired) vs ≠0 (PWM, equal pairs) dispatch; **voice-level OP_SYNC end-to-end**: the real Voice's block matches a byte-exact reference of two hand-driven Oscillators wired osc1.sync_out→osc2.sync_in through the full mix/sub/noise/fuzz chain (120 samples × 3 blocks), + OP_SYNC vs OP_SUM counterfactual.

**voice_pitch_test** — kHighestNote clamp (notes 120 & 127 byte-identical, non-silent); non-vacuity (15359 vs 15360 differ); set_pitch_bend_offset(+128) ≡ note+1 byte-identical; octave loop exactness via stride (note 69's odd samples == note 81's stream; LUT precondition 2·(lut[64]>>4)==lut[64]>>3 asserted); low-note region (0/640/1535) non-silent + pitches distinguished.

## Audit-expected-value corrections

1. **U8AddClip "wrap→max" is wrong**: the uint8 add wraps BEFORE the compare (faithful avrlib op.h:46-52) — U8AddClip(250,10,255)==4, not 255. Pinned true semantics.
2. **Envelope "peaks at 254"**: the 65025 snap is overwritten same-render (next stage inc always ≥1); the observable outputs are 253 (attack render 1 = 255·254>>8; sustain hold 254·255>>8).
3. **CLICK "last 31 samples"**: it is 32 samples (223..254).
4. **Discovered engine behavior**: `mix_balance ≥ 128` makes dst[MIX_BALANCE]=balance<<8 read as negative under S16ClipU14 → clips to 0 (osc1-only mix). The OP_SYNC test documents this and uses balance 127 (→16383→gain 255/0). The audit's 252/3 mix model was wrong.

## Residual risks

- The `midi_note < 0` clamp is pinned as a property (low pitches render valid non-silent blocks; pitches distinguished), not exact equality — impossible at Voice level: only bandlimited-table shapes observe `note`, and any pitch change also changes the increment.
- Stride/octave pins depend on the current LUT values (preconditions asserted in-test, so a table regen fails loudly rather than vacuously passing).
- Sibling lanes are concurrently editing shared files (CMakeLists.txt, other tests); my blocks re-verified green after the merge.

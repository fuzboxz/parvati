# Ambika → JUCE — DSP Port Specification

**Status:** Spec for implementation subagents (Phases 2–4).
**Reference source (read-only):** `ambika_reference/` (the original Ambika firmware; not tracked — see `.gitignore`)
**Target project:** the Parvati project root (JUCE project "Parvati", Jozsef Ottucsak), JUCE at `~/JUCE`.

All `file:line` citations are relative to `ambika_reference/`.

---

## LOCKED DESIGN DECISIONS (do not deviate)

1. **Faithful integer port of the digital voice DSP.** Preserve 8-bit audio (0–255, centered 128), all PROGMEM lookup/wavetables, the fixed-point math (`avrlib/op.h`), and the **40-sample block / once-per-block control-rate** structure. Convert to `float` **only** at the audio output boundary. **Do NOT** re-implement oscillators/LFOs/envelopes with `juce::dsp` generators — port them so that they bit-match the firmware.
2. **`juce::dsp` is used for:** (a) the **analog filter emulation** (no firmware exists — written fresh), and (b) the **framework**: `juce::Synthesiser`, `juce::AudioProcessorValueTreeState` (APVTS).
3. **Scope** = patch/voice engine **+ arpeggiator**. DEFER multi-timbral parts + step sequencer + the LCD/encoder UI.
4. **Polyphony** = **16** voices per Part (fixed 96-voice pool, 6 Parts x 16; the authentic hardware default is 6 — one per voicecard).

---

## A. TARGET FILE / CLASS LAYOUT

All DSP under `Source/dsp/`. Framework under `Source/`.

```
Source/
  PluginProcessor.{h,cpp}          juce::AudioProcessor + APVTS owner
  PluginEditor.{h,cpp}             GUI (Phase 4)
  SynthEngine.{h,cpp}              owns juce::Synthesiser, voice management, arp
  AmbikaSound.{h,cpp}              juce::SynthesiserSound (trivial, accepts all notes)
  AmbikaVoice.{h,cpp}              juce::SynthesiserVoice; wraps one dsp::Voice + dsp::AnalogFilter

  dsp/
    fixed_types.h                  uint24_t / uint24c_t (from avrlib/base.h:55-65)
    fixed_math.h                   ALL op.h helpers (Section D)
    random.h/.cpp                  avrlib::Random (LFSR) (avrlib/random.h)
    patch.h                        enums + Patch/Part structs (from common/patch.h)
    resources/
      resources.h                  table extern decls + ResourcesManager (index helpers)
      resources_data.cpp           ALL constexpr/static tables (Section C)
      resources_manager.h          Lookup/Load helpers (avrlib/resources_manager.h)
    oscillator.h/.cpp              dsp::Oscillator (voicecard/oscillator.{h,cc})
    sub_oscillator.h               dsp::SubOscillator (voicecard/sub_oscillator.h)
    transient_generator.h          dsp::TransientGenerator (voicecard/transient_generator.h)
    envelope.h                     dsp::Envelope (voicecard/envelope.h)
    lfo.h                          dsp::Lfo (common/lfo.h)
    voice.h/.cpp                   dsp::Voice (mod matrix + render) (voicecard/voice.{h,cc})
    analog_filter.h/.cpp           dsp::AnalogFilter: 3 topologies via juce::dsp (Section E)
    constants.h                    kControlRate=40, kAudioBlockSize=40, pitch consts, sample-rate const
```

**Namespace:** wrap all ported DSP in `namespace ambika::dsp { }`. Framework classes at `Source/` top level.

**Class→singleton conversion (IMPORTANT):** In the firmware, `Voice`, `SubOscillator`, `TransientGenerator` are **static singletons** (one per voicecard). For 16-voice polyphony, every `static` member in the firmware must become an **instance member** of the C++ class. In detail:
- `Voice` (voice.h:97-126): all `static` fields → instance fields; `osc_1`, `osc_2`, `sub_osc`, `transient_generator` globals (voice.cc:20-23) → instance members.
- `SubOscillator` (sub_oscillator.h:64-66): `static phase_/phase_increment_` → instance.
- `TransientGenerator` (transient_generator.h:76-87): `static counter_/rng_state_/decimate_/gain_` → instance.
- `Envelope`, `Lfo`, `Oscillator`: already instance-based in firmware — keep as-is.
- `Random` (random.h) is a global LFSR. Ambika uses it as a source of noise/random across the whole synth. **Decision:** keep ONE global `ambika::dsp::Random` shared across voices. This matches the hardware (one RNG per voicecard, but the noise is uncorrelated). A single shared RNG is acceptable and simpler. Each voice also needs its own noise generator for the mixer noise (`voice.cc` mixer uses an inline `noise = noise*73+1` LCG per voice — keep that per-voice).

---

## B. PER-MODULE PORTING NOTES

### B.1 `dsp::Oscillator` — `voicecard/oscillator.{h,cc}`

**State (oscillator.h:158-170):** `uint24_t phase_`, `uint24_t phase_increment_`, `uint8_t shape_`, `uint8_t parameter_`, `uint8_t fm_parameter_`, `uint8_t note_`, `OscillatorState data_` (union: VowelSynthesizerState / FilteredNoiseState / QuadSawPadState / `uint16_t secondary_phase`), `sync_input_/sync_output_` pointers.

**Render dispatch (oscillator.h:131-153):**
- `shape==WAVEFORM_SQUARE`: if `parameter_==0` → `RenderSimpleWavetable`; else `RenderBandlimitedPwm`.
- else: index = `shape >= WAVEFORM_WAVETABLE_1 ? WAVEFORM_WAVETABLE_1 : shape`; load fn from `fn_table_[index]`; special-case `WAVEFORM_WAVEQUENCE` → `RenderWavequence`.
- `fn_table_` (oscillator.cc:481-504): the dispatch table mapping shape→RenderFn. Port this exact table.

**Functions to port (with `file:line`):**
| Function | Lines | Notes |
|---|---|---|
| RenderSilence | 58-63 | writes 128 to buffer. |
| RenderBandlimitedPwm | 65-111 | SAW table pair, `U8Swap4(note_)` for zone; doubles phase increment; **writes 2 samples/iter and `size--`** (so 40 iters→40 samples but writes pairs); special `note_>52` scale; PWM via phase offset `shift=(parameter_+128)<<8`; `a = a - b + 128`. |
| RenderSimpleWavetable | 113-159 | zone via `U8Swap4(note_)`; base resource by shape (SAW/SQUARE/TRIANGLE); sine uses both waves = SINE; **triangle uses different waveshaper** (`sample=parameter_` vs `sample+=parameter_>>1`). |
| RenderCzSaw | 161-170 | `clipped_phi = phi<0x20 ? phi<<3 : 0xff`; `ReadSample(wav_res_sine, U8MixU16(phi, clipped_phi, parameter_<<1))`. |
| RenderCzResoSaw | 172-194 | `increment = phase_increment_.integral + (phase_increment_.integral*parameter_)>>2`; **`type = shape - WAVEFORM_CZ_SAW_LP`** (0..4); `phase_2` reset from `lut_res_cz_phase_reset[type&3]` on `phase.carry`; window `~(phase.integral>>8)`; `type&2` → signed path. |
| RenderCzResoPulse | 195-226 | window is a pulse/trapezoid (`<0x4000`→255, `<0x8000` ramp); `type==5` halts carrier. |
| RenderCzResoTri | 227-252 | triangle window. |
| RenderFm | 253-281 | `offset` from `fm_parameter_` (range clamp 24..48→0..24); `multiplier=lut_res_fm_frequency_ratios[offset]`; `increment=(phase_increment_.integral*multiplier)>>8`; modulator→sine phase modulation. |
| Render8BitLand | 282-288 | `(((phase.integral>>8)^(x<<1)) & (~x)) + (x>>1)`. |
| RenderVowel | 290-361 | **Cantarino formant synth.** `data_.vw` updated every 4 calls; reads `wav_res_vowel_data` (7 bytes/vowel × interpolated); 3 formant oscillators (2 sine + 1 square) via `wav_res_formant_sine/square` indexed by `phaselet | amplitude`; `phase_noise = S8S8Mul(state_msb, noise_modulation)`; glottal reset when `(phase.integral+phase_noise) < phase_increment_int.integral`; writes pairs (`size--`). |
| RenderDirtyPwm | 362-368 | `(phase.integral>>8) < 127+parameter_ ? 0 : 255`. |
| RenderQuadSawPad | 370-394 | `phase_spread = (phase_increment_.integral*parameter_)>>13 +1`; 3 detuned saws via `data_.qs.phase[3]`; sum `>>10`. |
| RenderFilteredNoise | 395-425 | own LFSR `rng_state` (Galois, 0xb400); on sync resets to `rng_reset_value`; one-pole LP via `U8Mix(lp, noise, filter_coefficient)`; `parameter_>=64` → HP (noise - lp - 128) else LP. |
| RenderInterpolatedWavetable | 426-462 | `wavetable_definition = wav_res_wavetables + U8U8Mul(shape-WAVEFORM_WAVETABLE_1, 18)`; `num_steps` at byte 0; `pointer = U8U8Mul(parameter_<<1, num_steps)`; wave indices at bytes 1,2; `wave_N = wav_res_waves + U8U8Mul(idx,129)`; read with `phase.integral>>1`; balance `~gain, gain`. |
| RenderWavequence | 463-471 | single wave `wav_res_waves + U8U8Mul(parameter_,129)`; `InterpolateSample(wave, phase.integral>>1)`. |

**GOTCHAS (oscillator):**
- **Phase is 24-bit `{integral:16, fractional:8}`** (fixed_types.h). A full cycle = `2^24` phase units; `phase.carry` (from `U24AddC`) = cycle wrap = **sync pulse**. Do NOT simplify to a single int without preserving `carry`.
- `BEGIN_SAMPLE_LOOP/END_SAMPLE_LOOP/UPDATE_PHASE*` macros (oscillator.cc:18-44): port as explicit loops. `UPDATE_PHASE` checks `*sync_input_++` (resets phase to 0) then `U24AddC` then emits `phase.carry` to `*sync_output_++`. **`sync_input_`/`sync_output_` are per-sample arrays of size `kAudioBlockSize`** (set up in Voice::ProcessBlock via `sync_state_`/`no_sync_`/`dummy_sync_state_`).
- `no_sync_` is a zeroed buffer (never forces reset); `sync_state_` records osc1 wraps; `dummy_sync_state_` discards osc2 wraps.
- **BandlimitedPWM & Vowel write 2 samples per iteration and `size--`** — they fill the 40-sample block in 20 iters. Preserve exactly (the doubled-write + decrement).
- **Multi-zone sample-rate selection:** `U8Swap4(note_)` splits the note into nibbles: high nibble = interpolation gain between adjacent bandlimited tables, low nibble = table index (0..5). `U8AddClip(idx,1,5)` clamps for half-rate waves (SAW/SQUARE: `kNumZonesFullSampleRate=6`, clamp 5; for the half-rate `kNumZonesHalfSampleRate=5`). Tables 6/placeholder (waveform_table indices 9,16,23 → `wav_res_sine`) are unreachable because of the clamping, but keep them.
- **CZ `type`**: `shape - WAVEFORM_CZ_SAW_LP` (not `WAVEFORM_CZ_SAW`). Values 0..4 select LP/PK/BP/HP variants + signed/unsigned (`type&2`).
- `RenderFm`: `fm_parameter_` is set in voice.cc to `patch.osc[i].range + 36` (NOT a raw patch value). `offset` is derived 0..24 via the 24/48 clamp.
- **Vowel/VowelSynthesizerState**: `formant_amplitude[3]` aliases `noise_modulation` (comment oscillator.cc:312). Keep the 4-element array.
- `ReadSample(table, phase)` (oscillator.h:28-34) = `table[phase>>8]` (no interpolation); `InterpolateSample(table, phase)` = linear interp (needs 257-entry tables).

### B.2 `dsp::SubOscillator` — `voicecard/sub_oscillator.h`
- `Render(shape, buffer, amount)` (lines 23-54). `increment = phase_increment_`; if `shape>=3`, `increment=U24ShiftRight(increment)` (−1 octave) and `shape-=3`. `pulse_width = shape==0 ? 0x80 : 0x40`. For `shape!=1`: square/pulse (`phase.integral>>8 < pulse_width ? 0 : 255`). For `shape==1`: triangle (`phase.integral>>7`, folded). `*buffer = U8Mix(*buffer, v, ~amount, amount)`.
- Shapes: 0/3=square, 1/4=triangle, 2/5=pulse (−1 octave for 3/4/5). `set_increment` receives `U24ShiftRight(osc1_increment)` from voice.cc (one octave below osc1).
- Phase accumulator: `phase_ = U24Add(phase_, increment)` per sample.

### B.3 `dsp::TransientGenerator` — `voicecard/transient_generator.h`
- Only active for `shape >= WAVEFORM_SUB_OSC_CLICK` (shapes CLICK/GLITCH/BLOW/METALLIC/POP). `counter_=255` on `Trigger()`.
- `Render(shape, buffer, amount)` (lines 18-32): while `counter_ && size--`, call `fn_table_[shape-WAVEFORM_SUB_OSC_CLICK]()`, `amplitude = U8U8MulShift8(gain_, amount)`, mix into buffer.
- 5 generators (lines 40-72): Click (255 when `counter<32`), Glitch (LCG `state*73+counter`), Blow (decimated LCG with `decimate_` counter, gain from `counter&0x80`), Metallic (`counter*57`, gain `counter>=64?255:counter<<2`), Pop (returns 0, gain gate).
- **`gain_` is set INSIDE each generator** (side effect), then used for the mix amplitude. Preserve ordering.

### B.4 `dsp::Envelope` — `voicecard/envelope.h`
- ADSR + DEAD stages (EnvelopeStage enum). Stage `phase_increment_` from `lut_res_env_portamento_increments[attack/decay/release]`; `stage_target_[DECAY]=stage_target_[SUSTAIN]=sustain<<1`.
- `Render()` (lines 75-92): `phase_ += phase_increment_`; on wrap (`phase_ < phase_increment_`) → set value to `b_` and Trigger next stage. value `= U8MixU16(a_, b_, InterpolateSample(wav_res_env_expo, phase_))`; returns `value_>>8`.
- `Trigger(stage)` (lines 56-64): `a_=value_>>8; b_=stage_target_[stage]; phase_=0`.
- **3 envelopes** per voice (kNumEnvelopes=3), each with its own `env_lfo` settings (attack/decay/sustain/release/shape/rate/retrigger_mode). In firmware env1/2/3 are driven by the SAME `env_lfo[i]` settings (the patch has `env_lfo[3]`, each = an EnvelopeLfoSettings). Env2 is the filter envelope (filter_env modulation), env3 also available.

### B.5 `dsp::Lfo` — `common/lfo.h`
- `Render(shape)` (lines 28-66): 16-bit phase accumulator; `looped_ = phase_ < phase_increment_` (wrap detection). Shapes: RAMP (`phase>>8`), S&H (latch `value_=Random::GetByte()` on loop), TRIANGLE (`phase&0x8000 ? phase>>7 : ~(phase>>7)`), SQUARE (`phase&0x8000?255:0`), default = wavetable LFO from `wav_res_lfo_waveforms` (16 single-... actually each LFO waveform is `wav_res_lfo_waveforms + offset` where `offset = (shape-LFO_WAVEFORM_WAVE_1)*257` — note `wav_res_lfo_waveforms_SIZE=2` is just the base; the real LFO waveforms live in `wav_res_waves`/`wav_res_lfo_waveforms`). ⚠️ **VERIFY the LFO wavetable addressing** (lfo.h:55-62 uses `wav_res_lfo_waveforms + offset`, offset = `(shape-LFO_WAVEFORM_WAVE_1)<<8 + (shape-LFO_WAVEFORM_WAVE_1)` = `n*257`). The 16 LFO waves occupy `16*257=4112` bytes — cross-check that this overlaps the `wav_res_waves` region. **Flag for verification.**
- `set_phase_increment` is fed from `lut_res_lfo_increments` (indexed by rate). For synced rates (rate >= kNumSyncedLfoRates=15) the rate is tempo-derived — handled controller-side (out of scope for the first pass; implement free-running rates first, synced LFO as a follow-up).
- `looped()` used for LFO retrigger (sync mode).

### B.6 `dsp::Voice` — `voicecard/voice.{h,cc}` (THE CORE)

**Public API (voice.h:42-94):** `Init`, `Trigger(note,velocity,legato)`, `Release`, `Kill`, `ProcessBlock`, `cutoff()/vca()/crush()/resonation()` (read mod destinations), `set_patch_data/set_part_data`, `mutable_patch_data`, `TriggerEnvelope(index,stage)`, `ResetAllControllers`.

**Pitch representation:** 14-bit fixed (7 bits MIDI note : 7 bits fine, units of 1/128 semitone). `kLowestNote=0`, `kHighestNote=120*128=15360`, `kOctave=12*128=1536`, `kPitchTableStart=116*128=14848` (voice.h:21-25).

**ProcessBlock (voice.cc:432-540) pipeline:**
1. `LoadSources()` (220-218): render env1/2/3, voice_lfo (MOD_SRC_LFO_4), noise, note (`U14ShiftRight6(pitch_value_)`), gate; run **4 modifiers** (lines 233-264). 
2. `ProcessModulationMatrix()` (221-265): 14 routings.
3. `UpdateDestinations()` (267-330): filter cutoff/res, crush, osc params, env updates, voice-lfo rate.
4. `vca() < 2` early-out (437-442): write 128 (silence) and return — **skip oscillator render**.
5. `RenderOscillators()` (332-420): portamento, pitch→increment, render osc1+sub+osc2.
6. Mixer operators (444-489): SUM/SYNC(via sync arrays)/RING_MOD/XOR/FOLD/BITS.
7. Sub-osc / transient mix (491-499).
8. Noise + fuzz/distortion (501-535): per-sample LCG `noise=noise*73+1`; distortion via `wav_res_distortion` waveshaper; write via `audio_buffer.Overwrite2`.

**GOTCHAS (voice — the highest-risk module):**

- **AC-coupled sources (voice.cc:240-249):** sources in `[MOD_SRC_LFO_1..MOD_SRC_LFO_4]`, `MOD_SRC_PITCH_BEND`, `MOD_SRC_NOTE` are bipolar centered at 128. Their modulation = `S8S8Mul(amount, source_value + 128)` (note `+128`, then signed×signed). All other sources use `S8U8Mul(amount, source_value)`.
- **`amount` is int8** (-63..63 from patch; stored as `int8_t amount = patch.modulation[i].amount`).
- **VCA is multiplicative, not additive (voice.cc:251-264):** for `destination==MOD_DST_VCA`: if `amount<0`, negate amount and invert source (`source=255-source`); if `amount!=63`, `source = U8Mix(255, source, amount<<2)`; then `modulation_destinations_[MOD_DST_VCA] = U8U8MulShift8(current_vca, source)`. Note base VCA = `part_.volume<<1` (line 234).
- **Wheel scaling of LAST modulation (voice.cc:225-227):** `if (i==kNumModulations-1) amount = S8U8MulShift8(amount, modulation_sources_[MOD_SRC_WHEEL])`.
- **Modifiers (voice.cc:233-264):** `MODIFIER_LAG_PROCESSOR` is special — it has state (`modulation_sources_[MOD_SRC_OP_1+i]` retained, 1-pole). `MODIFIER_QUANTIZE` uses a derived mask. The arithmetic modifiers compute `ops[1..8]` (sum/product/attenuate/max/min/xor) then select `ops[op]`. Preserve the `ops[]` indexing exactly (op values: SUM=1,PRODUCT=2,ATTENUATE=3,MAX=4,MIN=5,XOR=6,GE=7,LE=8 per the if/else + ops[] layout — **verify op→ops[] mapping against lines 241-258**).
- **Filter cutoff (voice.cc:253-255, 268-273):** `dst_[MOD_DST_FILTER_CUTOFF] = S16ClipU14(U8U8Mul(patch.filter[0].cutoff,128) + pitch_value_ - 8192)` — **cutoff tracks pitch** (keytracking built-in). Then env2 and lfo2 added (lines 269-271): `cutoff += S8U8Mul(filter_env, env2)`, `cutoff += S8S8Mul(filter_lfo, lfo2+128)`. Output `modulation_destinations_[MOD_DST_FILTER_CUTOFF] = U14ShiftRight6(cutoff)` → 8-bit (0..127). `resonance = U14ShiftRight6(dst_[...]<<8)`.
- **filter[1] is UNUSED** by the voicecard firmware (only `filter[0]` is live, voice.cc:253/255). filter[1] params exist in the patch/parameter table but are not sent to the analog filter. **Decision:** keep filter[1] in the patch for compat, but drive the single emulated filter from filter[0]. (The hardware is one filter per voicecard.)
- **`set_fm_parameter` = `patch.osc[i].range + 36`** (voice.cc:300,308) — for FM shape only; range is also added to pitch for non-FM (line 343).
- **Pitch→increment (voice.cc:347-371):** `ref_pitch = pitch - kPitchTableStart`; while `ref_pitch<0`: `ref_pitch += kOctave; ++num_shifts`; `increment.integral = lut_res_oscillator_increments[ref_pitch>>1]` (uint24, frac=0); then `while(num_shifts--) increment = U24ShiftRight(increment)`. sub_osc uses `U24ShiftRight(increment)` (one octave down).
- **`midi_note` for osc** = `U15ShiftRight7(pitch)-12`, clamped ≥0 (voice.cc:374-378). Used for zone selection.
- **Distortion/noise mixer (voice.cc:501-535):** processes 2 samples per loop iter (`i+=2`), per-voice LCG `noise=noise*73+1`, `signal_noise = U8Mix(buffer, noise, ~noise_gain, noise_gain)`, `out = U8Mix(signal_noise, wav_res_distortion[signal_noise], ~fuzz_gain, fuzz_gain)`. `fuzz` = mix_fuzz, `noise` = mix_noise.
- **`audio_buffer.Overwrite2(a,b)`** → in JUCE, append two samples to a per-voice float buffer (centered 128 → float). See Section G.
- **kAudioBlockSize=40**: ProcessBlock renders exactly 40 samples. The `buffer_[40]`, `osc2_buffer_[40]`, `sync_state_[40]`, `no_sync_[40]`, `dummy_sync_state_[40]` (voice.h:118-122).

---

## C. RESOURCES CONVERSION PLAN (`dsp/resources/`)

Convert `voicecard/resources.cc` PROGMEM tables → plain C++ arrays. **Mechanical transcription; preserve every value and size exactly.** Source `resources.h` SIZE macros give element counts.

### C.1 Lookup tables (`uint16_t`, indexed by element — `pgm_read_word(addr+i)` → `table[i]`)
| Table | ID macro | Size (elts) | Used by |
|---|---|---|---|
| lut_res_lfo_increments | LUT_RES_LFO_INCREMENTS | 128 | LFO rate→phase increment |
| lut_res_env_portamento_increments | LUT_RES_ENV_PORTAMENTO_INCREMENTS | 128 | env attack/decay/release + portamento |
| lut_res_oscillator_increments | LUT_RES_OSCILLATOR_INCREMENTS | 768 | **pitch→phase increment** (THE pitch table) |
| lut_res_fm_frequency_ratios | LUT_RES_FM_FREQUENCY_RATIOS | 25 | FM ratio |
| lut_res_vca_linearization | LUT_RES_VCA_LINEARIZATION | 256 | (controller VCA curve — not in voicecard DSP; port anyway for completeness, optional) |
| lut_res_cz_phase_reset | LUT_RES_CZ_PHASE_RESET | 4 | CZ phase2 reset values |

`lookup_table_table[]` (resources.cc:214-221): array of 6 pointers → port as `const uint16_t* const lookup_table_table[6] = {lfo_inc, env_inc, osc_inc, fm_ratios, vca_lin, cz_reset};`. **Used only by the `Lookup(ResourceId,i)` overload — but all DSP call sites pass the table pointer directly, so the code rarely uses this indirection. Keep it for parity.**

### C.2 Wavetables (`uint8_t`, indexed by byte — `pgm_read_byte(p+i)` → `table[i]`)
| Table | Size (bytes) | Notes |
|---|---|---|
| wav_res_formant_sine | 256 | vowel (256 entries, 16 amplitudes × 16 phases) |
| wav_res_formant_square | 256 | vowel |
| wav_res_sine | 257 | +1 for interp |
| wav_res_bandlimited_square_0..5 | 257 each | zone 0..5 |
| wav_res_bandlimited_saw_0..5 | 257 each | |
| wav_res_bandlimited_triangle_0 | 257 | (only 0,3,4,5 defined) |
| wav_res_bandlimited_triangle_3 | 257 | |
| wav_res_bandlimited_triangle_4 | 257 | |
| wav_res_bandlimited_triangle_5 | 257 | |
| wav_res_vowel_data | 63 | 9 vowels × 7 bytes |
| wav_res_distortion | 256 | fuzz waveshaper |
| wav_res_lfo_waveforms | 2 | base ptr (real waves offset into this — see B.5 caveat) |
| wav_res_env_expo | 257 | envelope exponential curve |
| **wav_res_waves** | **10320** | the 80 single-cycle waves (80×129=10320, 129=128+1 interp) |
| **wav_res_wavetables** | **288** | 16 wavetable definitions × 18 bytes |

`waveform_table[]` (resources.cc:2301-2331): **30 pointers** indexed by WAV_RES_* ID. Note indices 9,16,23 alias `wav_res_sine` (placeholders). Port this array verbatim — oscillators index it as `waveform_table[WAV_RES_BANDLIMITED_SAW_1 + wave_index]`.

### C.3 ResourcesManager helpers (`dsp/resources_manager.h`)
Port `avrlib/resources_manager.h` semantics as free functions on plain arrays:
- `LookupU8(const uint8_t* p, index)` → `p[index]`
- `LookupU16(const uint16_t* p, index)` → `p[index]`
- `InterpolateSample` stays in `fixed_math.h`.
- All `PROGMEM`/`pgm_read_*` disappear (RAM arrays).

**Conversion method (for the resources_data.cpp author):** write a small parser, or transcribe by hand from the existing `resources.cc`. Just strip `PROGMEM`, change `prog_uint8_t`→`const uint8_t`, and keep the brace-init lists. Make sure that the total byte counts match the SIZE macros. A checksum/count assertion in code is recommended.

---

## D. FIXED-POINT MATH HELPERS (`dsp/fixed_math.h`)

Port the **non-optimized branch** of `avrlib/op.h` (lines ~430-540, the portable C fallbacks) — these are mathematically identical to the ASM. **Keep everything integer** (faithfulness). Reference C++:

```cpp
#include <cstdint>

namespace ambika::dsp {

using uint24_t  = struct { uint16_t integral; uint8_t fractional; };  // or class w/ accessors
using uint24c_t = struct { uint8_t carry; uint16_t integral; uint8_t fractional; };

inline int16_t Clip(int16_t v, int16_t lo, int16_t hi){ return v<lo?lo:(v>hi?hi:v); }

// 8-bit unsigned mix: result = (a*(255-balance) + b*balance) >> 8
inline uint8_t U8Mix(uint8_t a, uint8_t b, uint8_t balance){
  return static_cast<uint8_t>((a*(255-balance) + b*balance) >> 8);
}
// 4-arg: result = (a*gain_a + b*gain_b) >> 8
inline uint8_t U8Mix(uint8_t a, uint8_t b, uint8_t ga, uint8_t gb){
  return static_cast<uint8_t>((a*ga + b*gb) >> 8);
}
// returns full 16-bit sum (no >>8)
inline uint16_t U8MixU16(uint8_t a, uint8_t b, uint8_t balance){ return a*(255-balance) + b*balance; }

// signed mix
inline int8_t S8Mix(int8_t a, int8_t b, uint8_t ga, uint8_t gb){ return (a*ga + b*gb) >> 8; }

inline uint8_t  U8U8MulShift8(uint8_t a, uint8_t b){ return (uint8_t)((a*b)>>8); }
inline int8_t   S8U8MulShift8(int8_t a, uint8_t b){ return (int8_t)((a*b)>>8); }   // a is signed
inline int16_t  S8U8Mul(int8_t a, uint8_t b){ return (int16_t)(a*b); }
inline int16_t  S8S8Mul(int8_t a, int8_t b){ return (int16_t)(a*b); }
inline uint16_t U8U8Mul(uint8_t a, uint8_t b){ return (uint16_t)(a*b); }
inline int8_t   S8S8MulShift8(int8_t a, int8_t b){ return (int8_t)((a*b)>>8); }
inline uint16_t U8U8Mul_u16(uint8_t a, uint8_t b){ return (uint16_t)(a*b); } // U8U8Mul already returns u16

inline uint16_t Mul16Scale8(uint16_t a, uint16_t b){ return (uint32_t(a)*b)>>8; }

inline uint8_t U14ShiftRight6(uint16_t v){ return (uint8_t)(v>>6); }   // 14-bit→8-bit
inline uint8_t U15ShiftRight7(uint16_t v){ return (uint8_t)(v>>7); }   // 15-bit→8-bit
inline uint16_t U16ShiftRight4(uint16_t a){ return a>>4; }

inline uint8_t U8ShiftRight4(uint8_t a){ return a>>4; }
inline uint8_t U8ShiftLeft4(uint8_t a){ return a<<4; }
inline uint8_t U8Swap4(uint8_t a){ return (a<<4)|(a>>4); }
inline uint8_t U8AddClip(uint8_t v, uint8_t inc, uint8_t mx){ v+=inc; return v>mx?mx:v; }

inline uint8_t S16ClipU8(int16_t v){ return v<0?0:(v>255?255:(uint8_t)v); }
inline int8_t  S16ClipS8(int16_t v){ return v<-128?-128:(v>127?127:(int8_t)v); }
inline int16_t S16ClipU14(int16_t v){           // clamp to [0,16383]
  uint16_t u=(uint16_t)v;
  if ((u>>8)&0x80) return 0;
  if ((u>>8)&0x40) return 16383;
  return v;
}

// 24-bit ops (use 32-bit intermediate)
inline uint24c_t U24AddC(uint24_t a, uint24_t b){
  uint32_t av=(uint32_t)a.integral<<8 | a.fractional;
  uint32_t bv=(uint32_t)b.integral<<8 | b.fractional;
  uint32_t s=av+bv;
  uint24c_t r; r.carry=(uint8_t)(s>>24); r.integral=(uint16_t)((s>>8)&0xFFFF); r.fractional=(uint8_t)(s&0xFF); return r;
}
inline uint24_t U24Add(uint24_t a, uint24_t b){
  uint32_t s=((uint32_t)a.integral<<8|a.fractional)+((uint32_t)b.integral<<8|b.fractional);
  uint24_t r; r.integral=(uint16_t)((s>>8)&0xFFFF); r.fractional=(uint8_t)(s&0xFF); return r;
}
inline uint24_t U24ShiftRight(uint24_t a){
  uint32_t v=((uint32_t)a.integral<<8|a.fractional)>>1;
  uint24_t r; r.integral=(uint16_t)(v>>8); r.fractional=(uint8_t)(v&0xFF); return r;
}
inline uint24_t U24ShiftLeft(uint24_t a){
  uint32_t v=((uint32_t)a.integral<<8|a.fractional)<<1;
  uint24_t r; r.integral=(uint16_t)(v>>8); r.fractional=(uint8_t)(v&0xFF); return r;
}

// linear interp into a 257-entry (or 256+1) table
inline uint8_t InterpolateSample(const uint8_t* table, uint16_t phase){
  return U8Mix(table[phase>>8], table[(phase>>8)+1], (uint8_t)(phase&0xFF));
}

} // namespace
```

**IMPORTANT semantics to preserve (op.h):**
- `U8Mix(a,b,balance)`: `balance` is the weight of `b`; weight of `a` is `255-balance`. The 4-arg form uses independent `gain_a/gain_b`.
- `S8U8MulShift8/S8U8Mul`: `a` is **signed** int8 (e.g. `amount`, `patch.filter_env`), `b` unsigned. Result signed.
- `S8S8Mul`: both signed → signed 16.
- `U14ShiftRight6`: drops 6 bits (14→8). `U15ShiftRight7`: 15→8. These are NOT plain `>>6`/`>>7` in the ASM (they shift a value whose top bits matter) but the C fallback is exactly `>>6`/`>>7` — use the fallback.
- **`Random`** (avrlib/random.h): Galois LFSR `state=(state>>1)^(-(state&1)&0xB400)`, period 65535. `GetByte()` updates then returns `state>>8`; `state_msb()` returns `state>>8` WITHOUT updating. Port as a class with a seedable `uint16_t state_`. **Critical:** `GetByte` updates state; `state_msb` does not — voice.cc uses `state_msb()` for `MOD_SRC_RANDOM` (voice.cc:189) and vowel noise (oscillator.cc), and `GetByte()` for `MOD_SRC_NOISE` (voice.cc:188) — these are DIFFERENT RNG draws. Keep a single global Random; seed once.

**Where float is acceptable:** NOWHERE in the digital voice path. The only float conversion is `(sample-128)/128.0f` at the JUCE output boundary (Section G). The filter (Section E) operates in float but consumes the already-converted signal.

---

## E. ANALOG FILTER EMULATION DESIGN (`dsp/analog_filter.h/.cpp`)

There is **no firmware filter**. The voice computes `cutoff` (8-bit, 0..127 via `U14ShiftRight6`, voice.cc:274) and `resonance` (8-bit, voice.cc:276-278) once per block, plus `mode` (`patch.filter[0].mode`, written to hardware in voicecard.cc:143). Parvati emulates 3 selectable topologies:

### E.1 Topology → JUCE class
| Voicecard | Topology | JUCE class | Modes |
|---|---|---|---|
| SMR4 (LM13700) / 4-pole SSM2164 | 4-pole cascaded LP | `juce::dsp::LadderFilter` (mode = LP12/LP24) | LP (24dB/oct). NOTCH/BP/HP from `mode` apply only conceptually; real 4-pole card is LP. **Use LP24 for both 4-pole types.** (The UI card is named Ladder: the JUCE ladder stands in for the OTA cascade.) | 
| SVF (SSM2164) | 2-pole state variable | `juce::dsp::StateVariableTPTFilter` | LP/BP/HP (mode 0/2/3). NOTCH (mode 3→ use HP or a notch via `band - low` — approximate). |

**Decision:** expose a single `AnalogFilter` class with an enum `Topology { FOUR_POLE_LADDER, FOUR_POLE_SSM2164, TWO_POLE_SVF }` (selectable per-patch or globally). For v1, all three use LP; they differ in slope/pole-count via the chosen JUCE class. (LM13700 vs SSM2164 differ subtly in resonance drive/saturation. V1 treats both as the JUCE ladder — a follow-up enhancement can add OTA saturation.)

### E.2 Cutoff mapping (0..127 → Hz)
The 8-bit cutoff from the voice already includes keytracking (`pitch_value_`) + env2 + lfo2. Map the 7-bit value to Hz with an **exponential** curve that spans about the analog filter range. The hardware DAC + OTA gives roughly exponential V/Hz. Use:
```
freqHz = minHz * pow(maxHz/minHz, cutoff/127.0f)
```
with `minHz≈20`, `maxHz≈16000` (tune in Phase 3 against the frequency response of a reference). Clamp to [20, 0.49×sampleRate].
- **`juce::dsp::LadderFilter`**: `setCutoffFrequencyHz(freqHz)`.
- **`juce::dsp::StateVariableTPTFilter`**: `setCutoffFrequencyHz(freqHz)`.

### E.3 Resonance mapping (0..127 → 0..1)
- `LadderFilter.setResonance(resonance/127.0f * 0.95f)` (avoid self-osc instability; the JUCE ladder self-oscillates near 1.0).
- `StateVariableTPTFilter` has no Q param directly — use the TPT `setResonance` if available, else emulate resonance by blending or by using `juce::dsp::StateVariableFilter` (deprecated but has resonance) OR implement a small SVF. **Flag:** verify the exact JUCE 8 API for SVF resonance (the TPT filter exposes `setResonance(0..1)` in recent JUCE). If unavailable, fall back to a hand-written TPT SVF (4 lines) — acceptable, because it is filter emulation and not voice DSP.

### E.4 Mode → output selection
- SVF: `mode==0`→Low, `2`→High, `3`→Band (and Notch≈Band−Low). Per voice.cc/voicecard.cc the 2-bit mode is `filter_mode_bytes[]={0,1,2,3}` but only LP/BP/HP/NOTCH enum exists. Map FILTER_MODE_LP→low, _HP→high, _BP→band, _NOTCH→notch.
- 4-pole: always lowpass output.

### E.5 Update cadence
The analog filter cutoff/res/mode update **once per 40-sample block** (control rate) in hardware. Faithfully: update the JUCE filter params once per block in `Voice::ProcessBlock` after `UpdateDestinations()`, then `process()` each of the 40 samples. (JUCE dsp filters accept per-sample `process`.) This matches the hardware CV-update cadence.

### E.6 VCA
`vca()` (0..255, multiplicative mod) gates amplitude. Apply as a per-sample gain `vca()/255.0f` AFTER the filter (the analog VCA is post-filter). Also note `lut_res_vca_linearization` (256) gives the VCA log curve — apply it for authenticity: `gain = lut_res_vca_linearization[vca()]/32768.0f` (verify scaling). This is the one controller-side table that IS relevant to voice output.

---

## F. APVTS PARAMETER LAYOUT

Source of truth: `controller/parameter.cc` (`parameters[]` table) + `common/patch.h`. **Patch bytes ↔ APVTS** via the patch struct byte layout (patch.h:248-269). APVTS holds the user-facing params; the engine reads a `Patch` struct built from APVTS (or APVTS writes directly into the `Patch` bytes via `mutable_patch_data()`).

### F.1 Patch (voice) parameters → APVTS IDs
Use `juce::ParameterID` strings. Ranges from `parameter.cc`:

| # | ParamID | Patch field | Type | Range |
|---|---|---|---|---|
| osc1_shape | "osc1Shape" | osc[0].shape | Choice (28 entries) | 0..27 |
| osc1_param | "osc1Param" | osc[0].parameter | Int | 0..127 |
| osc1_range | "osc1Range" | osc[0].range | Int(signed) | -24..24 |
| osc1_detune | "osc1Detune" | osc[0].detune | Int(signed) | -64..64 |
| osc2_shape | "osc2Shape" | osc[1].shape | Choice | 0..27 |
| osc2_param | "osc2Param" | osc[1].parameter | Int | 0..127 |
| osc2_range | "osc2Range" | osc[1].range | Int(signed) | -24..24 |
| osc2_detune | "osc2Detune" | osc[1].detune | Int(signed) | -64..64 |
| mix_balance | "mixBalance" | mix_balance | Int | 0..63 |
| mix_op | "mixOperator" | mix_op | Choice (OP) | 0..5 |
| mix_param | "mixParameter" | mix_parameter | Int | 0..63 |
| mix_sub_shape | "mixSubShape" | mix_sub_osc_shape | Choice (sub-osc) | 0..11 |
| mix_sub | "mixSubLevel" | mix_sub_osc | Int | 0..63 |
| mix_noise | "mixNoise" | mix_noise | Int | 0..63 |
| mix_fuzz | "mixFuzz" | mix_fuzz | Int | 0..63 |
| mix_crush | "mixCrush" | mix_crush | Int | 0..31 |
| filt_cutoff | "filtCutoff" | filter[0].cutoff | Int | 0..127 |
| filt_reso | "filtResonance" | filter[0].resonance | Int | 0..63 |
| filt_mode | "filtMode" | filter[0].mode | Choice (LP/BP/HP/NOTCH) | 0..3 |
| filt_env | "filtEnv" | filter_env | Int(signed) | 0..63 (patch is int8; UI 0..63) |
| filt_lfo | "filtLfo" | filter_lfo | Int | 0..63 |

**Env/LFO block (3 instances, env_lfo[i])** — these are `PRM_PATCH_ENV_ATTACK..LFO_SYNC` with stride 3 (parameter.cc:25-31), `indexed_by=PRM_UI_ACTIVE_ENV_LFO`:
Per instance i (0..2): attack(0..127), decay(0..127), sustain(0..127), release(0..127), shape(LFO 0..19 / for env unused), rate(0..142 synced+free), retrigger/sync(0..2). The patch `env_lfo[i]` = EnvelopeLfoSettings{attack,decay,sustain,release,shape,rate,padding,retrigger_mode} (patch.h:41-50). Envelope uses attack/decay/sustain/release; LFO uses shape/rate. **Note:** in firmware env1/2/3 each share their `env_lfo[i]` for both the env AND the LFO of that index. So 3 "env+lfo" units. Expose per unit: envA/D/S/R + lfoShape + lfoRate + lfoSync.

- voice_lfo_shape (0..3), voice_lfo_rate (0..127).

**Modulation matrix (14 routings)** — `patch.modulation[14]` (patch.h:261): each {source, destination, amount}. Expose 14× {modN_source (Choice 0..32), modN_dest (Choice 0..18), modN_amount (-63..63)}. UI selector `PRM_UI_ACTIVE_MODULATION` (0..13) is GUI-only, not stored in patch.

**Modifiers (4)** — `patch.modifier[4]` (patch.h:264): each {operands[2], op}. Expose 4× {modifN_in1 (Choice 0..MOD_SRC_LAST-1), modifN_in2, modifN_op (Choice 0..10)}.

### F.2 Part parameters (in scope: volume, legato, portamento)
- part_volume (0..127), part_legato (0/1), part_portamento (0..63). (Octave/tuning/spread/raga are part-level extras — include octave (-2..2) and tuning for usability; defer raga/scale quantization.)

### F.3 Arpeggiator parameters (in scope)
From `parameter.cc` 49-53: arp_mode, arp_direction, arp_octave(1..4), arp_pattern, arp_resolution(0..14). These live in the Part struct (controller-side). Implement an arpeggiator in `SynthEngine` that consumes these; DEFER the step-sequencer (arp_mode distinguishes ARP vs SEQ — only implement ARP modes; SEQ modes can no-op for v1).

### F.4 Patch byte ↔ APVTS bridge
- `mutable_patch_data()` returns `uint8_t*` over the `Patch` struct (voice.h:80). APVTS parameter changes write the corresponding byte; the engine reads the struct. Use a `ValueTree::Listener` or `AudioProcessorParameter::Listener` → `voice.set_patch_data(byteAddr, value)`. Map each APVTS param to its struct byte offset (from patch.h field order). Signed fields (range/detune/amount/filter_env/filter_lfo) cast int8.
- For MIDI automation parity: `parameter.cc` `midi_cc_map[128]` (CC→param index) and `midi_nrpn_map[256]` (NRPN address→param index). Port these maps to support the documented CC/NRPN assignments for hardware-like control.

---

## G. CONTROL-RATE vs SAMPLE-RATE in JUCE

### G.1 The 40-sample block structure
`kControlRate = kAudioBlockSize = 40` (voicecard.h:20). The firmware renders **40 audio samples per `ProcessBlock`**, and the modulation matrix runs **once** (before the 40-sample render). `Voice::ProcessBlock` must be called to produce exactly 40 samples each time.

### G.2 Recommended architecture: fixed-rate engine + resampler (MOST FAITHFUL)
Run the integer engine at a **fixed internal rate** (the original voicecard rate) and resample to the host rate at the output. This preserves:
- exact 8-bit quantization & all table character (engine untouched),
- the control-rate cadence (mod matrix updates at `internalRate/40` Hz — matches hardware),
- correct pitch (increments read straight from tables, no scaling math).

```
JUCE processBlock (hostRate, N samples)
  → for each needed host sample, pull from a Resampler fed by the Engine
  → Engine produces internalRate-rate float samples in 40-sample blocks:
        AmbikaVoice::renderNextBlock() {
            while (need ≥ 40 samples) { voice.ProcessBlock();   // 40 u8 samples
                                        float x = (s-128)/128.0f; push 40 to resampler input; }
            resampler.process → hostRate output buffer
            → apply VCA gain, filter (float, post-resample or pre? see below)
        }
```
**Filter placement:** apply `AnalogFilter` on the **internal-rate** signal (40 samples/block) BEFORE resampling. The filter's per-block CV update then matches the engine cadence. The 8-bit→float conversion feeds the float filter; then resample the filtered float. Order: engine(8bit)→float→filter(float)→resample→host buffer. VCA gain is applied with the filter.

### G.3 Sample rate value
The original voicecard rate must be determined (Phase 3 verification). **Candidate: 62500 Hz** (Shruthi-1 / Ambika documented rate; the README calls this "a refined version of the Shruthi-1 engine"). Store as `constants.h::kInternalSampleRate`. **VERIFY by back-solving the pitch table:** for MIDI note A4=69, expected 440 Hz; `increment = lut_res_oscillator_increments[ref_pitch>>1]` after octave-shifting; `freq = increment_as_24bit / 2^24 * kInternalSampleRate` must equal 440 Hz. Solve for `kInternalSampleRate`. Put the exact derived constant in code + an assertion/test.
- **8-bit → float:** `float sample = (int32_t(u8) - 128) / 128.0f;` (range ≈ [-1, +0.992]). Preserve the asymmetry (128 vs 127) exactly — do NOT normalize to ±1 symmetric.

### G.4 Polyphonic dispatch
`juce::Synthesiser` calls each `AmbikaVoice::renderNextBlock` per block. Each voice owns its `dsp::Voice`, `dsp::AnalogFilter`, resampler state, and a 40-sample staging buffer. ProcessBlock emits exactly 40 samples. Therefore align engine block production with resampler demand across arbitrary host block sizes (variable host N). Use a per-voice ring/jitter buffer between the 40-sample producer and the resampler.

### G.5 ALTERNATIVE (acceptable, simpler): host-rate engine with scaled increments
If the resampler is too heavy, run the engine at host rate and scale ALL table-derived increments by `kInternalSampleRate/hostRate` (oscillator, LFO, env, portamento — all are increment-based, so they scale uniformly). Keep the 40-sample block + once-per-block control update (the control cadence becomes `hostRate/40` Hz — slightly different from hardware, but usually inaudible). Pitch correctness comes from the scale factor. **Default to G.2; fall back to G.5 only if needed.**

---

## H. PHASED IMPLEMENTATION ORDER (DEPENDENCY DAG)

The layers are ordered so that independent modules build in parallel. Each layer must compile against the previous one.

```
Layer 0 (no deps — parallel):
  H0a  dsp/fixed_types.h + dsp/fixed_math.h + dsp/random.h        (Section D)
  H0b  dsp/resources/* (resources.h, resources_data.cpp, manager) (Section C)   ← mechanical transcription of resources.cc
  H0c  dsp/patch.h (enums + Patch/Part structs) + dsp/constants.h (patch.h port)
  H0d  dsp/analog_filter.h/.cpp (juce::dsp filters)               (Section E)

Layer 1 (depends on L0 — parallel):
  H1a  dsp/envelope.h            (needs H0a,H0b)
  H1b  dsp/lfo.h                 (needs H0a,H0b)
  H1c  dsp/sub_oscillator.h      (needs H0a)
  H1d  dsp/transient_generator.h (needs H0a,H0b,random)
  H1e  dsp/oscillator.h/.cpp     (needs H0a,H0b)   ← biggest module, can run alone

Layer 2 (depends on L1):
  H2   dsp/voice.h/.cpp          (needs H1a–H1e, H0b)  ← the mod matrix + ProcessBlock

Layer 3 (depends on H2, H0d):
  H3   AmbikaVoice/AmbikaSound/SynthEngine + PluginProcessor (APVTS) + resampler plumbing (Section F,G)

Layer 4 (depends on H3):
  H4   Arpeggiator (SynthEngine) + GUI (Phase 4)
```

**Parallelization guidance:**
- L0a/L0b/L0c/L0d are fully independent → 4 parallel workers.
- L1a–L1e depend only on L0 → 5 parallel workers after L0 is complete (the oscillator is the largest task; start it early).
- H2 is serial after L1 (one worker; it is the integration point and carries the highest risk — it needs careful review).
- H3 is serial after H2+H0d.
- L0b (resources) is large but mechanical; it can start immediately, with no dependencies.

**Verification gates (Phase 3) per layer:** L0b resources must match byte counts/SIZE macros (assert). H1/H2 must render known waveforms that match the firmware reference (desktop-compiled original) — see Phase 3 plan. The **mod matrix (H2)** is the #1 faithfulness risk; plan a dedicated review pass for it.

---

## CROSS-CUTTING RISKS / VERIFICATION FLAGS

1. **kInternalSampleRate** — MUST be derived & asserted (Section G.3). Wrong value = wrong pitch/timing everywhere.
2. **LFO wavetable addressing** (B.5) — `wav_res_lfo_waveforms` SIZE=2 but LFO code offsets by `n*257`; check that the 16 LFO waves actually live where indexed (likely overlapping `wav_res_waves`). **Resolve before H1b.**
3. **Mod-matrix sign conventions** (AC-coupling +128, VCA multiplicative, wheel-scaled last slot) — highest bug risk in H2.
4. **Modifier op→ops[] mapping** (B.6) — re-verify the exact index mapping in voice.cc:241-258.
5. **uint24 carry / sync** — must preserve `U24AddC` carry semantics, or oscillator sync breaks.
6. **Vowel & BandlimitedPWM write pairs** (`size--`) — easy to miscount; produces a wrong block length.
7. **JUCE SVF resonance API** (E.3) — confirm that JUCE 8 `StateVariableTPTFilter` exposes resonance; else hand-roll a TPT SVF.
8. **filter[1] is vestigial** in voicecard firmware — do not double-filter; use a single filter from filter[0].
9. **filter_env/filter_lfo** are `int8` in the Patch struct (can be negative) but the UI parameter range is 0..63 — confirm signed handling at the struct boundary (voice.cc reads `patch_.filter_env` as int8).

```

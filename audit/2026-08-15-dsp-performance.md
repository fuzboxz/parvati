# DSP Correctness + Audio-Thread Performance Audit — Parvati @ HEAD (2026-08-15)

Lane: Source/dsp/**, Source/AmbikaVoice.cpp render path, SynthEngine::processBlock/renderPartFx.
Method: static line-by-line comparison against ambika_reference/ (read-only; no files modified,
no builds run). Every claim below cites file:line evidence read directly. Hypotheses are marked.
How verified: (V)=verified by direct code reading/diff vs the named reference file;
(H)=hypothesis requiring runtime confirmation (test/command listed).

---

## 1. Fidelity vs ambika_reference (spot-check results)

### 1.1 fixed_math.h vs avrlib/op.h — VERIFIED FAITHFUL (V)
Read Source/dsp/fixed_math.h in full and diffed op-by-op against ambika_reference/avrlib/op.h
(the ASM branch and the `#else` C-fallback branch were both read):
- U8Mix ÷256 truncation semantics preserved (fixed_math.h:56-66 vs op.h:216-219/340-343):
  U8Mix(255,x,0)==254, matching `sum.bytes[1]` of the ASM (r1 = high byte), not ÷255.
- S8*/mulsu sign-extension semantics identical: S8U8MulShift8 floor-shifts the 16-bit
  product (fixed_math.h:126) exactly like the ASM's r1 capture — negative products match.
- S16ClipS8: the port's direct clamp (fixed_math.h:37-40) is equivalent to the ASM's
  `S16ClipU8(value+128)+128` truncation (verified by hand for ±128 endpoints).
- U24Add/U24Sub/U24Shift*: pack/unpack via 32-bit intermediate; the op.h C-fallback's
  U24Sub typo (`sum` undefined, op.h:461) is correctly implemented as wrapping a-b
  (fixed_math.h:148-159, documented).
- U24AddC carry = bit 24 (fixed_math.h:140), identical to ASM `adc %2, r1`.
- NOTE (documented-in-code divergence class): the port follows the op.h **C-fallback**
  branch. For U8U4MixU8 the ASM branch (op.h:225-247) truncates the high nibble of a
  >12-bit sum while the C fallback (and the port, fixed_math.h:90-93) truncates the full
  >>4 — these differ only when a*(15-balance)+b*balance > 4095, i.e. a=b=255 territory
  the vowel synth never reaches (amplitudes are LUT bytes, balance ≤ 15; max reachable
  ≈ 15*255*2 only if both operands saturate — not the case in RenderVowel's use).
  Severity: note. No action needed.

### 1.2 oscillator.{h,cpp} vs voicecard/oscillator.{h,cc} — VERIFIED FAITHFUL (V)
All 15 render functions compared line-by-line:
- UPDATE_PHASE / BEGIN_SAMPLE_LOOP macros copied verbatim (oscillator.cpp:46-83 vs
  oscillator.cc:21-46); the added `(void)` casts are inert.
- RenderBandlimitedPwm: identical incl. `shift` 16-bit wrap, double U8Mix scale shaping
  for note_>52, phase doubling via U24ShiftLeft, dual-sample sync consumption
  (oscillator.cpp:92-137 vs oscillator.cc:86-134).
- RenderSimpleWavetable: identical triangle waveshaper branch (sample=parameter_ vs
  sample+=parameter_>>1) (oscillator.cpp:139-185 vs oscillator.cc:137-185).
- CZ family: the AVR 16-bit-promotion-sensitive sites are correctly replicated:
  - CZ pulse window `~(integral-0x4000)>>6` reproduced as 32-bit unsigned then
    truncated — bit-identical because the low 8 bits of >>6 come from bits 6..13 of
    the same 16-bit value (oscillator.cpp:239 with the explanatory comment; verified
    against oscillator.cc:198-200).
  - CZ saw window `~(integral>>8)` truncation equivalence (oscillator.cpp:203).
  - `lut_res_cz_phase_reset[type & 0x03]` type masking preserved (oscillator.cpp:186-194).
- FM: `parameter_ <<= 1` persists into the member exactly as firmware (oscillator.cpp:272),
  offset clamp 24/24/48 identical, modulation = modulator*parameter_ in 16-bit range.
- Vowel: formant/amplitude interpolation, the formant_amplitude[3]/noise_modulation alias
  resolved without OOB (oscillator.cpp:318-335), and the glottal-reset comparison
  reproduced with explicit uint16 truncation to match AVR unsigned promotion
  (oscillator.cpp:371-377 vs oscillator.cc:329-331). result += uint8 semantics preserved
  (full-unsigned add then wrap to int8).
- 8-bit land / dirty PWM / quad saw / filtered noise / interpolated wavetable: identical
  integer math (oscillator.cpp:279-471 vs oscillator.cc:238-452).
- DELIBERATE, DOCUMENTED DIVERGENCE — RenderWavequence clamps parameter_ to the 80-wave
  table (oscillator.cpp:479-489) where the firmware reads PROGMEM past wave 79
  (oscillator.cc:460-466). Firmware reads undefined adjacent resource bytes; the clamp is
  the sane choice. Severity: note (behavior differs only for parameter_ > 79; unreachable
  from the UI's 0..127 knob only above 79 — reachable, but reads garbage in hardware).
- Dispatch table (oscillator.cpp:505-537) matches oscillator.cc:481-504 including the
  WAVEFORM_SQUARE pw==0 hack (oscillator.h:100-106 vs oscillator.h:118-135) and the
  WAVETABLE_1 index collapsing. WAVEFORM_* enum order in Source/dsp/patch.h:60-86 is
  identical to ambika_reference/common/patch.h (verified entry-by-entry).

### 1.3 envelope.h vs voicecard/envelope.h — VERIFIED FAITHFUL (V)
Render() loop identical incl. the wrap-snap `U8MixU16(a_,b_,255)` == b*255 and the
`Trigger(++stage_)` chaining (envelope.h:117-131 vs envelope.h:96-107). Update()
identical (sustain<<1 target). The port's Init() additionally zeroes stage_/value_/phase_
(envelope.h:59-69) — output-equivalent to the firmware's BSS-zero state at power-up
(firmware Init leaves stage_=ATTACK=0 with zero increments → Render() returns 0);
documented in-code. Severity: none.

### 1.4 lfo.h vs common/lfo.h — VERIFIED FAITHFUL to the VOICECARD build (V)
The wavetable-LFO default branch is compiled out under PARVATI_DISABLE_WAVETABLE_LFOS
(lfo.h:89-96), matching the voicecard makefile's -DDISABLE_WAVETABLE_LFOS. The
unreachable-shape default returns the held S&H value instead of the firmware's
use-of-uninitialized `value` (lfo.h:99-104) — strictly safer, no reachable divergence.
set_phase/looped_ semantics identical.

### 1.5 sub_oscillator.h / transient_generator.h — VERIFIED FAITHFUL (V)
Line-identical logic (instance-ized). TransientGenerator's set-gain-then-read ordering
preserved (transient_generator.h:56-64 vs transient_generator.h:52-59); the private
8-bit LCG (rng*73+counter) not the global Random — matches.

### 1.6 voice.cpp vs voicecard/voice.cc — VERIFIED FAITHFUL (V)
LoadSources / ProcessModulationMatrix / UpdateDestinations / RenderOscillators /
ProcessBlock compared line-by-line (voice.cpp:215-528 vs voice.cc:167-417): init patch
bytes identical, AC/DC coupling set (LFO_1..4/PITCH_BEND/NOTE) identical, VCA
multiplicative special case identical, filter_env/filter_lfo hardwired mods identical,
portamento snap logic identical, pitch-table octave-shift loop identical, mix ops
(SUM/RING/XOR/FOLD/BITS) identical, per-pair noise LCG `noise*73+1` identical, distortion
LUT mix identical. Deliberate plugin extensions, all documented at voice.cpp:5-29 and
voice.h:20-31: (1) LFO_1/2/3 rendered in-engine (firmware: controller over SPI);
(2) pitch_bend_offset_ added directly to osc pitch (voice.cpp:443-448) — a Parvati MPE
feature, NOT in firmware; (3) RetriggerLfos (mirrors controller Part::RetriggerLfos);
(4) envelope priming in Init (output-neutral). These change behavior vs firmware by
design; flagging only so nobody mistakes them for drift.

### 1.7 Known, accepted fidelity gaps (documented in-repo)
- AnalogFilter/VCA are fresh float juce::dsp emulations — there is no filter code in the
  firmware (analog hardware; analog_filter.h:4-20 documents this). The cutoff byte→Hz
  mapping is an explicit v1 guess "tunable in Phase 3" (analog_filter.h:66-68,
  cutoffByteToHz at analog_filter.cpp:230-238: exponential 20 Hz..16 kHz).
  Severity: improvement — recommend calibrating the Hz curve (and the VCA taper /
  kExpVcaMakeup=3.5 at AmbikaVoice.cpp:15-21) against hardware measurements or the
  SMR4/LM13700 schematics before calling it an "emulation".
- LFO 1/2/3 wavetable shapes 4..19 + controller-side tempo-sync rates are stubs
  (voice.cpp:6-29). UI clamps shapes to 0..3 (ParameterLayout.cpp:306,313), but
  MidiParameterMap.cpp:160 accepts NRPN lfo_shape 0..19 and .MUL loads carry raw bytes —
  such patches render an ~constant LFO. Severity: note — recommend remapping shapes>3 to
  triangle on load, or surfacing a warning.
- Bit-exactness is not regression-tested: the simavr/avr-gcc gold-standard harness skips
  when tools are absent (tests/hellcat_tests.cpp:5-10). All §1 conclusions above are
  code-reading only. Severity: important (residual risk) — recommend a CI job (or a
  one-time committed golden-fixture set: N patches × M blocks of 8-bit output captured
  from the AVR build) diffed against ambika::dsp::Voice.

---

## 2. Audio-thread discipline

### 2.1 Verified good (V)
- No locks anywhere in Source/dsp or SynthEngine (grep mutex/lock_guard/CriticalSection:
  zero hits).
- Steady-state process paths are allocation-free: FxChain scratch reserved in prepare
  (FxChain.cpp:24-56), HostRateBridge scratch reserved (HostRateBridge.h:37-75),
  FV-1 engine fixed-size state (fv1/Fv1Engine.h:366-394), voice FIFO pre-reserved with
  a worst-case-derived capacity that provably covers the fill loop
  (AmbikaVoice.cpp:66-75 + renderNextBlock fill/erase arithmetic — max size =
  inputNeeded+39 < reserve).
- Message-thread → audio-thread hand-offs are staged atomic frames with release/acquire
  publish: patch/part bytes (SynthEngine.cpp:184-193 frameDirty_), options
  (SynthEngine.cpp:1194-1210 optionsDirty_), arp/seq config seqlock, FX state fxDirty_
  (SynthEngine.cpp:1491-1527). The per-voice osFactor_/topology_ staging uses
  exchange() check-and-clear (AmbikaVoice.cpp:249, 263).
- Denormals: juce::ScopedNoDenormals wraps the whole block including voices + FX
  (PluginProcessor.cpp:161).
- Idle voices self-gate before running DSP (AmbikaVoice.cpp:522-533); Voice::ProcessBlock
  early-outs at vca()<2 (voice.cpp:489-495), matching firmware.
- renderVoices clears only its own sub-block range (MIDI-split tiling) —
  SynthEngine.cpp:1436-1443.

### 2.2 Findings
- F3 (important) — audio-thread allocations on parameter changes, deliberate but risky:
  (a) `fillInternalBlock` services osFactorDirty_/topologyDirty_ by calling
  `recreateOversampling()` → `std::make_unique<juce::dsp::Oversampling<float>>` +
  `filter_.prepare()` (AmbikaVoice.cpp:249-268, 113-140) — heap alloc/free plus JUCE
  filter-prepare allocations (LadderFilter::prepare resizes `state`,
  juce_LadderFilter.h:106 setNumChannels) on the AT, mid-render, while other voices run.
  (b) `FxChain::setSlotType` on the AT (fxDirty_ service) does
  `slots_[slot].reset(); slots_[slot] = createFxProcessor(t);` (FxChain.cpp:97-100) —
  FxLoopingDelay/FxWSOLAStretch embed ~512 KB of int16 buffers as members
  (FxProcessors.h:158-160, 210-212) → a large AT allocation with first-touch page faults
  on iOS (AUv3).
  Recommendation: pre-construct the new processor on the message thread into a staging
  slot and swap via atomic pointer exchange on the AT (the codebase already has this
  pattern elsewhere); for oversampling, pre-build the 2x and 4x Oversampling objects in
  prepare() and select between them by flag. At minimum, pre-touch/mlock the big buffers.
- F6 (note) — block-size robustness: all FX/engine scratch is sized from prepareToPlay's
  samplesPerBlock (SynthEngine.cpp:80-95; FxChain.cpp:20-56; HostRateBridge.h:37-41) and
  processBlock has no numSamples > prepared guard (PluginProcessor.cpp:69-72,152+). JUCE
  does not universally guarantee processBlock ≤ samplesPerBlock. An oversized block is an
  OOB heap write (wetL_[i], dryL_[i] at FxChain.cpp:317-319 etc.). Recommendation: guard
  `numSamples > preparedMax` → re-prepare (or jassert + clamp) at the top of processBlock.
- (note) `AmbikaVoice::setVcaExponential/setSmoothingEnabled/setFilterDrive` are plain
  writes, but the ENGINE routes them through optionsDirty_ staging
  (SynthEngine.cpp:228-232, 1198-1209) — the raw methods are only reached AT-side. OK.

---

## 3. Clouds FX section — block-size assumptions, DC/glitch risks

### 3.1 Prior doc claims re-verified at HEAD (V)
- FX_CRACKLE_INVESTIGATION.md: the HostRateBridge m<=0 hold-last-sample fix IS present
  (HostRateBridge.h:178-190). The drift-free fractional phase (hostWritePhase_ carry,
  HostRateBridge.h:153) and the phaseStart_ head-overlap (HostRateBridge.h:161-176,
  194-205) are as documented. Regression tests exist
  (tests/hellcat_fx_bridge_tinychunk.cpp, tests/hellcat_fx_engine_continuity.cpp).
- FX_AUDIO_REVIEW.md findings vs current code:
  - B1/B3 fixed: wetFade_ is a per-sample one-pole both directions, always on
    (FxChain.h:169-186; blendSlotWetFade FxChain.cpp:171-224; renderParallel persists
    fades FxChain.cpp:512-526).
  - B2/B6 fixed for gain-style params: dryWet_ and masterMix_ per-sample one-pole
    (smoothCoef_, FxChain.cpp:112-118, 455-479). Effect params deliberately RAW at the
    980 Hz cadence (FxChain.cpp:305-308 comment) — a documented design choice.
  - B5 fixed: the FX mod matrix evaluates per internal-block sub-chunk with drift-free
    fractional boundaries (SynthEngine.cpp:1608-1634, 1671-1677).
  - B7 fixed: prepare() preserves wetFade_/dryWetCur_/masterMixCur_ across re-prepare
    (FxChain.cpp:57-96).
  - B8 fixed: reverb/diffuser amount pinned full-wet (FxProcessors.cpp:65-69, 122).
  - B4 partially: type change still destroys the old processor's tail (accepted
    exception, documented FxChain.cpp:81-86); new module fades in from 0.
- Sub-chunk/m==0 safety of the vendored engines (V): FxEngine loops are per-sample with
  LFO updates keyed to write_ptr_&(31)==0 (fx_engine.h:259-273) — independent of call
  granularity; PitchShifter/Reverb/Diffuser Process tolerate size 0 (loops skip;
  ParameterInterpolator computes an unused ±inf increment but its destructor stores the
  unchanged old value — stmlib/dsp/parameter_interpolator.h:33-49 — no NaN escapes).
  LoopingDelay/WSOLA/Spectral are chunked at ≤32 internal samples by the wrappers
  (FxProcessors.cpp:248-262, 288-296, 385-394).

### 3.2 Findings
- F4 (note) — clouds::Reverb reads uninitialized lp_decay_1_/lp_decay_2_:
  Init() (clouds/dsp/fx/reverb.h:44-69) sets every field except these two; Process reads
  them (reverb.h:93-94). Same as upstream Clouds, but in-plugin this is an
  indeterminate-float read (MSAN-detectable; benign post-first-block). The vendored file
  already carries local modifications (the four ParameterInterpolators, reverb.h:75-79),
  so adding two zero-initializers there is consistent with local practice — or zero via
  FxReverb::prepare/reset if the vendored file is to be kept pristine.
- F10 (note) — PitchShifter window-glide rate varies with sub-chunk size:
  size_coeff = 0.05f / chunkSize (clouds/dsp/fx/pitch_shifter.h:86-88), and the bridge's
  m jitters ±1 around ~30 (at 48 kHz) → the one-pole glide speed wobbles ~±3%.
  Inaudible by design intent (documented in the file); listed for completeness.
- F8 (improvement, perf) — WSOLA inline correlator: LoadCorrelator +
  EvaluateSomeCandidates run per 32-sample chunk (FxProcessors.cpp:307-308), i.e. ~1000x/s
  per enabled WSOLA slot, where the firmware amortizes the search across its main loop.
  With several parts running WSOLA this is plausibly the heaviest FX cost (H — confirm
  with a profile; tools/ has profiling helpers and PluginProcessor has an audio-load
  probe). Recommendation: spread the candidate evaluation across chunks to firmware-
  equivalent duty.
- (improvement, perf) — per-sub-chunk transcendentals: FxPitchShifter::process computes
  std::pow per sub-chunk (~1 kHz) even when ratioParam_ is unchanged (FxProcessors.cpp:72);
  same for FxReverb low-cut fc (FxProcessors.cpp:156) and FxRingModulator carrier
  (FxProcessors.cpp:648). Cache on param change.
- (note, perf) — HostRateBridge runs 8 stmlib::Svf processes per host sample per FX when
  host > 32 kHz (2 cascaded AA + 2 cascaded recon, per channel; HostRateBridge.h:106-131,
  216-230). Correct design; just the dominant bridge cost.

---

## 4. SynthEngine process path — routing bug + render-path review

- F1 (important — routing bug, verified V): per-part FX input sums the wrong buffers.
  Evidence chain:
  - `voiceIndices` are indices into the Synthesiser voice list (SynthEngine.h:271),
    filled with sequential pool indices 0..95 (rebuildVoiceAllocation,
    SynthEngine.cpp:651-658; kNumVoices = 96, SynthEngine.h:42).
  - Voices render into `voiceCardBuffers_[av->getVoiceCard()]` — card 0..5
    (SynthEngine.cpp:1457-1458; voiceCardBuffers_ has kNumParts entries, SynthEngine.h:588).
  - The FX mono sum does `for (int vi : part.voiceIndices) if (vi >= 0 && vi < kNumParts)
    add(mono, voiceCardBuffers_[vi], ...)` (SynthEngine.cpp:1534-1536) — a voice pool
    index used as a CARD index.
  Correct only when each Part's first pool indices numerically equal its card numbers
  (true for the default `1<<partIndex` bitmask + AUTO slots, and for Part 0 owning all 6
  cards). Broken for: voiceSlots > card count (a Part with 1 card and 16 slots sums ALL
  six card buffers — other Parts' audio bleeds into its FX and is then double-summed on
  the main bus, which sums all six fxOutputBuffers — PluginProcessor.cpp:239-243), and
  for non-prefix card bitmasks (a Part owning cards {2,4} sums buffers 0,1).
  Recommendation: have rebuildVoiceAllocation store each Part's card mask (it already
  computes partCards[]), keep it AT-readable, and sum voiceCardBuffers_ over the set
  bits. Add a multitimbral FX-routing test (two parts, disjoint non-prefix card sets,
  distinct tones, assert each part's fxOutput contains only its own tone).
- renderVoices/renderPartFx structure otherwise sound: per-voice fixed voicecard routing,
  representative-voice mod tracking with crossfade (SynthEngine.cpp:1553-1601),
  drift-free 980 Hz sub-chunking (SynthEngine.cpp:1608-1634), bounded modDst validation
  (SynthEngine.cpp:1664-1665).

---

## 5. Performance opportunities (hot loops, SIMD, LUTs)

- F2 (important — perf): AnalogFilter's ladder path builds a 1-sample juce::dsp
  AudioBlock + ProcessContextReplacing and calls ladder_.process() PER SAMPLE
  (analog_filter.cpp:215-222) because LadderFilter::processSample is protected
  (verified juce_LadderFilter.h:125-127). The core is ~30 flops + LUT saturation
  (juce_LadderFilter.cpp:130-153); the wrapper adds context construction, channel-pointer
  fetch, and 2 smoother reads per call. At the 96-voice ceiling × 39216 Hz that is ~3.8M
  wrapper invocations/s (H for the exact cost split — measure with the existing audio-load
  probe / Instruments). Fixes: (a) batch the 40-sample internal block into ONE
  process() call per block (JUCE's internal per-sample smoothers then operate correctly
  across the block), then apply VCA gain per sample; or (b) a small subclass
  `struct LadderTap : juce::dsp::LadderFilter<float> { using
  LadderFilter<float>::processSample; }` to expose the protected method.
  Related: in the optional smoothing path, commit() runs per sample →
  setCutoffFrequencyHz → setTargetValue(std::exp(...)) per sample
  (AmbikaVoice.cpp:362-366; juce_LadderFilter.h:107) — 2 std::exp/sample/voice while
  smoothing is ON (default off). Since the JUCE ladder self-smooths per sample
  (updateSmoothers, juce_LadderFilter.cpp:158-162), Parvati's extra 20 ms smoother is
  double work; consider dropping the per-sample commit and letting JUCE's smoothers ramp.
- LUT use in the integer engine is already the firmware's own (257-entry tables, const
  arrays with external linkage — resources.h:118-155); InterpolateSample is 2 loads +
  mul-shift (fixed_math.h:196-200). No action.
- The integer voice loops (8-bit, table-driven, per-sample branches) do not vectorize and
  should not be forced to (bit-exactness); LTO is already enabled via
  juce_recommended_lto_flags (CMakeLists.txt:567) which inlines the fixed-point ops.
  A micro-optimization (note, optional): U24AddC/U24Add repack struct fields each call
  (fixed_math.h:132-159); representing phase as a single uint32_t in the oscillator hot
  loops (keeping the same wrap semantics) would shave packing/unpacking — only worth it
  if profiling shows the voice engine as the bottleneck.
- Per-voice resampler: juce::LagrangeInterpolator + vector FIFO with front-erase per
  chunk (AmbikaVoice.cpp:545-592). The erase is O(lookahead) — negligible. Reserve math
  verified correct (§2.1).
- FX blend loops (per-sample one-poles) are scalar by necessity; the chain's
  FloatVectorOperations copies/clears already vectorize where possible.

---

## 6. Findings index (severity order)

| ID | Severity | Area | Summary | Where |
|----|----------|------|---------|-------|
| F1 | important | SynthEngine renderPartFx | FX input sums voice-pool indices as card indices; wrong part mixes for non-default allocations | SynthEngine.cpp:1534-1536 vs :1457-1458, SynthEngine.h:271,588 |
| F2 | important | perf / filter | 1-sample LadderFilter::process() wrapper per sample per voice | analog_filter.cpp:215-222 |
| F3 | important | AT discipline | heap alloc/free on audio thread for OS-factor/topology/FX-type changes (make_unique ~512KB classes) | AmbikaVoice.cpp:249-268; FxChain.cpp:97-100; FxProcessors.h:158-160 |
| F-bitexact | important (residual risk) | testing | no automated bit-exact regression vs firmware DSP (simavr harness skips without avr-gcc) | tests/hellcat_tests.cpp:5-10 |
| F4 | note | clouds fx | Reverb lp_decay_1_/2_ uninitialized (indeterminate read) | clouds/dsp/fx/reverb.h:44-69 vs :93-94 |
| F5 | note | fidelity | LFO wavetable shapes 4..19 + tempo-sync stubs reachable via NRPN/.MUL | voice.cpp:6-29; lfo.h:99-104; MidiParameterMap.cpp:160 |
| F6 | note | robustness | no processBlock-size guard vs prepared scratch | PluginProcessor.cpp:69-72; FxChain.cpp:20-56 |
| F8 | improvement | perf | WSOLA inline correlator per 32-sample chunk; cacheable per-sub-chunk pows | FxProcessors.cpp:307-308, :72, :156, :648 |
| F9 | improvement | fidelity | filter/VCA float emulation curves uncalibrated (v1 guesses) | analog_filter.h:66-68; AmbikaVoice.cpp:15-21 |
| F10 | note | clouds fx | PitchShifter glide rate varies ±~3% with sub-chunk m jitter | clouds/dsp/fx/pitch_shifter.h:86-88 |

No critical findings. The one behavioral bug (F1) is contained to Parvati-exclusive FX
routing in non-default voice-allocation configs; the default single-part and
default-multitimbral configs are unaffected.

Commands the supervisor may run to confirm (not run here — read-only lane):
- `ctest --test-dir build -R parvati_fx_engine_continuity` (F-verify bridge/sub-chunk)
- a new multitimbral FX routing test per F1 (does not exist yet)
- Audio-load probe (PluginProcessor::getAudioLoad*) before/after an F2 fix, 96 voices

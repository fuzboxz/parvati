# Bug Hunt 2026-08-18 — Engine/DSP correctness lane (read-only review)

Scope reviewed: Source/SynthEngine.{h,cpp}, Source/AmbikaVoice.{h,cpp}, Source/Arpeggiator.{h,cpp},
Source/Sequencer.{h,cpp}, Source/NoteStack.h, Source/TransportClock.h, Source/dsp/{voice,analog_filter,
oscillator,envelope,lfo,fixed_math,patch,constants}.{h,cpp}, Source/dsp/fx/** (FxChain, FxProcessors,
HostRateBridge, Fv1Engine, fv1 family), plus load-path files (PluginProcessor.cpp, PatchFile.cpp,
HellcatPreset.cpp) where the engine's inputs are validated (or not).

Tests cross-checked before reporting (so pinned behavior is not re-reported):
tests/arp_test.cpp, tests/arp_seq_timing (hellcat_arp_seq_timing_test.cpp), tests/sequencer_test.cpp,
tests/polyphony_test.cpp, tests/multitimbral_test.cpp, tests/host_state_test.cpp, tests/load_invariants_test.cpp,
tests/loader_fuzz_test.cpp, tests/concurrency_test.cpp, tests/realtime_test.cpp, tests/part_fx_routing_test.cpp,
tests/fx_param_coverage_test.cpp, tests/synth_param_coverage_test.cpp, tests/voice_slots_test.cpp,
tests/tuning_test.cpp, tests/firmware_parity_known_divergences.txt.
Notable coverage gaps found while cross-checking: the loader fuzzer does NOT mutate .MUL Patch bodies
(only .PRO bodies + .MUL MultiData/PartData bytes), and its P2 property only asserts finite output —
it cannot catch the OOB write in F-eng-1; no test exercises arp_direction=Chord release behavior (F-eng-2).

## F-eng-1: Raw .MUL / host-state Patch bytes bypass all range clamping → OOB read AND WRITE in the per-voice mod matrix (audio thread)
- file:line —
  - Entry (unclamped writes into engine patch storage): Source/PluginProcessor.cpp:1046
    (`if (multi.parts[i].hasPatch) part.patchBytes = multi.parts[i].patch;` — raw 112 bytes),
    Source/PluginProcessor.cpp:1047 (raw PartData), Source/SynthEngine.cpp:686
    (`part.patchBytes.loadFrom (s.patch.data());` — host-state blob v7, values never validated),
    Source/PatchFile.cpp:250 (parser memcpys file bytes verbatim).
  - Sinks (all on the audio thread, reached via pushPartBytesToVoices → setPatchByte → ProcessBlock):
    - **OOB WRITE**: Source/dsp/voice.cpp:316 and :325 — `int16_t modulation = dst_[destination];`
      / `dst_[destination] = S16ClipU14 (modulation);` where `destination = patch_.modulation[i].destination`
      (voice.cpp:313) is a raw byte 0..255, but `dst_` is `int16_t dst_[kNumModulationDestinations]`
      = 19 entries (Source/dsp/voice.h:182). dst_[255] writes ~510 bytes past the array — beyond the
      ~241 bytes of trailing Voice members (voice.h:184-193) and past the end of `voice_` inside
      AmbikaVoice (which is heap-allocated via `new AmbikaVoice()`, SynthEngine.cpp:8), corrupting
      members such as `fifo_` (std::vector) → wild writes / crash on the next push_back.
    - OOB read: Source/dsp/voice.cpp:314 — `modulation_sources_[source]` (array of 31, voice.h:180),
      source raw 0..255.
    - OOB read: Source/dsp/voice.cpp:242 — modifier operands `x = modulation_sources_[x];` (raw bytes).
    - OOB read: Source/dsp/voice.cpp:107 — `lut_res_lfo_increments[rate - kNumSyncedLfoRates]`
      (rate raw byte; table has 128 entries, Source/dsp/resources/resources.h:32 — OOB for rate ≥ 143).
    - OOB read: Source/dsp/voice.cpp:190 — `lut_res_env_portamento_increments[part_.portamento_time]`
      (raw PartData byte 6; 128-entry table), same via Source/AmbikaVoice.h:79-86
      (`reprimeEnvelopes` calls `Envelope::Update` with RAW attack/decay/release bytes →
      Source/dsp/envelope.h:101-108 indexes the same 128-entry LUT with bytes up to 255).
    - OOB read: Source/dsp/oscillator.cpp:460 — `wav_res_wavetables + U8U8Mul (shape_ - WAVEFORM_WAVETABLE_1, 18)`
      with raw shape up to 255 (table is 288 bytes; shape 255 → +4212 bytes), and
      oscillator.cpp:465-466 `wavetable_definition[1 + (pointer >> 8)]` (index up to 255).
- severity: high (memory corruption / UB on the audio thread, reachable from a user-loadable file)
- evidence: Every other input path clamps before these bytes reach the Voice, which shows the invariant
  the mod matrix relies on: .PRO loads go through the APVTS (PluginProcessor.cpp:911
  `parvatiPatchByteToValue` → parameter range clamp; ParameterLayout.cpp:622-633); .parvati loads clamp
  (HellcatPreset.cpp:882 via `parvatiValueToPatchByte`, ParameterLayout.cpp:602-619 jlimit to descriptor
  range); APVTS ranges bound env rates to 0..142 (ParameterLayout.cpp:332) and portamento to 0..63
  (ParameterLayout.cpp:374); and the ARP/SEQ PartData bytes are explicitly clamped at the staging site
  "because raw-file loaders must do the same" (SynthEngine.cpp:302-321). The Patch struct has NO
  equivalent clamp on the .MUL/host-state paths. A corrupt or hostile .MUL (drag-and-drop;
  PluginEditor.cpp:4421) or hand-edited host-state blob sets patch byte 51 = modulation[0].destination;
  the next `allocationDirty_` service pushes the frame (SynthEngine.cpp:1852-1854 → pushPartBytesToVoices
  SynthEngine.cpp:1118-1140), and the first note-on executes `dst_[destination]` with the raw byte.
  This is a port-specific hazard: on AVR the firmware reads garbage flash and its UI never emits
  out-of-range bytes; here it is an intra-object/heap OOB write.
- deterministic_check: In an ASAN build (build_asan exists): construct the .MUL bytes (or, in a test,
  write `engine.getPart(0).patchBytes[51] = 200` then `engine.markAllocationDirty()`), then
  headless-render one 256-sample block with a note-on (velocity 100) at 48 kHz. Expected:
  ASAN heap-buffer-overflow WRITE report in `ambika::dsp::Voice::ProcessModulationMatrix`
  (voice.cpp:325). Also flip bits in the .MUL Patch bodies in tests/loader_fuzz_test.cpp's corpus
  (currently only .PRO bodies are mutated) — P2's finite-output assertion alone would not catch it,
  which is why the ASAN run is the check.

## F-eng-2: Arp CHORD direction strands every chord voice on key release (firmware diverges deliberately-guarded behavior)
- file:line — Source/Arpeggiator.h:93-99 (`noteOff` pops the key from `pressedKeys_` and only calls
  `allNotesOff()` when the stack empties); Source/Arpeggiator.cpp:172-183 (`allNotesOff` CHORD branch
  loops `for (i = 0; i < pressedKeys_.size(); ++i) internalNoteOff (pressedKeys_.sorted_note (i).note);`
  — the stack is ALREADY empty at this point, so zero note-offs are emitted); same empty-loop shape at
  Arpeggiator.cpp:103-106 (clockArpeggiator else-branch). Engine entry: Source/SynthEngine.cpp:2006
  (`parts_[p].arp.noteOff (note)` — the only release path for arp-held keys).
- severity: medium (audible stuck notes; chord voices sustain indefinitely and consume polyphony until
  stolen)
- evidence: Chord mode triggers `internalNoteOn` for every held note each step and parks
  `previousNote_ = 60` (Arpeggiator.cpp:84-90). When a key is released mid-phrase, the port does nothing
  for that pitch (it is removed from `pressedKeys_`, so later steps no longer retrigger it, but no
  note-off is ever sent for it); when the LAST key is released, `noteOff` → stack empties →
  `allNotesOff` → the CHORD loop iterates the now-empty stack → no note-offs at all. The firmware
  explicitly guards this exact case (ambika_reference/controller/part.cc:347-354): for CHORD mode it
  calls `InternalNoteOff(note)` at key-up "to avoid stuck notes, since the chord trigger mode doesn't
  really clean after itself". The port dropped that branch. Backstops checked and insufficient:
  `seq.allNotesOff()` (SynthEngine.cpp:2010-2012) only releases the sequencer's note;
  `releaseNoteInPart`'s defensive scan (SynthEngine.cpp:1333-1345) never runs because no note-off ever
  reaches it; CC123/partAllNotesOff (SynthEngine.cpp:1454-1479) does clean up, but that requires the
  user to send it. No test pins chord direction (tests/arp_test.cpp covers Up + the enable-arp
  stuck-note regression only; load_invariants_test.cpp:323 only checks the direction byte clamps).
- deterministic_check: Headless render mirroring arp_test.cpp's stuck-note regression: fresh processor,
  playhead 120 bpm playing; arp_mode=1 (Arp), arp_direction=5 (Chord), resolution 1/16; hold notes
  60/64/67 for ≥ 2 steps; send note-offs for all three; render 3 s idle; count Part-0 active voices
  (loop `engine.getAmbikaVoice(i)` → `isDisplayedActive()`/`isVoiceActive()` filtered by part index).
  Current tree: > 0 (stuck). Expected after fix: 0 (every chord voice released), plus mid-phrase
  single-key release should release that pitch immediately (firmware behavior).

## F-eng-3: FxChain::latency() reads the message-thread-owned `pending_` staging slot unsynchronized from the audio thread
- file:line — Source/dsp/fx/FxChain.cpp:510-533 (slotL lambda; :521-525 dereference
  `pending_[idx]->latency()` guarded only by a plain `!= nullptr` test, and reads non-atomic
  `pendingType_[idx]`); audio-thread call site: Source/dsp/fx/FxChain.cpp:586 (`const int Lc = latency();`
  inside `process()`); message-thread mutation: Source/dsp/fx/FxChain.cpp:161
  (`acquireStagingSlot` CASes the slot state to Filling — including the take-back from Staged) and
  FxChain.cpp:175 (`pending_[slot] = std::move (next);` — destroys any previously staged object).
- severity: medium (rare audio-thread crash / UB; definite data race under the C++ memory model)
- evidence: two interleaved threads/steps:
  (1) MT (message thread): user flips an FX slot type → `setSlotType` → `acquireStagingSlot` CASes
  Staged→Filling (the take-back path); `pending_[slot]` still holds the previously published staged
  processor. (2) AT (audio thread): `process()` → `latency()` → reads `pending_[idx] != nullptr`
  (true). (3) MT: builds the replacement then executes `pending_[slot] = std::move(next)` — the
  unique_ptr assignment DELETES the old staged processor. (4) AT: calls
  `pending_[idx]->latency()` — a virtual call through a destroyed object (UAF) and an unsynchronized
  read TSAN flags on every FX-type change racing a render. The header's own protocol
  (FxChain.h:132-150) makes `pending_` owned by whoever holds the stage state — but `latency()` never
  checks `stageState_`, so the Filling window (which ends in exactly that destruction) is unguarded.
  tests/concurrency_test.cpp case 10 (concurrency_test.cpp:263-275) does hammer `fx{N}_type` churn vs
  render and passes only because the destruction window is a few instructions wide.
- deterministic_check: TSAN build (build_tsan exists): run concurrency_test repeatedly with modeMask
  0x400 (isolate op 10, FX-type churn) while the audio thread renders — expect a
  `data race ... pending_ / pendingType_` report. Or a minimal harness: thread A loops
  `chain.process(buf…, 64)` on a prepared chain with slot 0 enabled; thread B loops
  `chain.setSlotType (0, (i&1) ? FxType::Wavefolder : FxType::Reverb)` (forces the take-back path) —
  TSAN reports the race; under ASAN the UAF window is exercisable by inserting a scheduling delay.

## F-eng-4: CYCLIC polyphony: note 127 sentinel collision can release the wrong voice (faithful-port latent)
- file:line — Source/SynthEngine.h:86-110 (PolyAllocator): `noteOn` stores `pool_[voice] = 0x80 | note`
  (== 0xff for note 127), cyclic `noteOff` stores `pool_[voice] = 0xff` as the cleared sentinel
  (SynthEngine.h:121-126), and `find` matches `(pool_[i] & 0x7f) == note` — so a live note-127 entry is
  byte-identical to a cleared slot, and `find(127)` returns the FIRST match. In CYCLIC mode a
  previously-cleared earlier slot wins over the real sounding slot → `releaseNoteInPart`
  (SynthEngine.cpp:1327-1331 CYCLIC branch) stops the wrong (idle) voice and the note-127 voice is
  never released (stuck until steal/CC123).
- severity: low (latent: requires CYCLIC mode + MIDI note 127 + a cleared slot earlier in the pool;
  byte-identical to the firmware ambika_reference/controller/voice_allocator.cc:80-108, so fixing it
  is a deliberate parity divergence)
- evidence: code path above; firmware reference verified identical, which is why this is latent/ported
  rather than a regression. Note also the POLY-family path is safe because a released entry keeps
  `0x00|note` (pool_ &= 0x7f) and idle slots are 0, not 0xff.
- deterministic_check: headless engine test: part 0 → polyphonyMode 3 (CYCLIC, via applyPartByte(15,3)
  + one render to service), ≥ 2 voices; noteOn(60) (cyclic voice 0), noteOff(60) (slot 0 cleared to
  0xff), noteOn(72) (voice 1), noteOn(127) (voice 0 again — pool_[0] = 0xff), noteOff(127): assert the
  voice currently playing 127 (scan getCurrentlyPlayingNote) enters release and goes inactive after the
  tail; current code leaves it active (the release lands on voice 1).

## Checked and clean (evidence-backed, no findings)
- Realtime safety: all FX processors reserve scratch in prepare() only (grep of resize/assign/push_back
  across Source/dsp found zero growth in process paths); FxChain::process clamps numSamples to
  maxBlock_ (FxChain.cpp:569-580) and PluginProcessor::processBlock clamps first
  (PluginProcessor.cpp:357-385); rebuildVoiceAllocation's voiceIndices push_backs stay within the
  kNumVoices reserve made in prepare (SynthEngine.cpp:182); message-thread frees are deferred to the
  60 Hz reaper (reapRetiredAudioObjects, SynthEngine.cpp:428-440); processTransport reuses
  processedMidi_ capacity.
- Atomics/frames: the frameDirty_/fxDirty_/optionsDirty_/configDirty_ release/acquire publishing and the
  pendingConfig_ seqlock (SynthEngine.h:175-230, bounded reader) are internally consistent; the one
  exception is F-eng-3 above.
- Numeric: SVF resonance floored at 0.05 to avoid the 1/res NaN (analog_filter.cpp:146,158);
  cutoff clamped below Nyquist (analog_filter.cpp:74-85); TransportClock floors samplesPerTick_ ≥ 1 so
  advance() cannot spin (TransportClock.h:38-44,47-58); renderPartFx sub-chunk phase guards step 0
  (sub<=0 → 1); HostRateBridge/Fv1Engine RateBridges bound all reads (m < maxM_, pos clamps,
  zero-order-hold on m==0); Envelope render wrap cannot advance past DEAD (DEAD increment is 0);
  fixed_math integer ops match the AVR semantics (explicit 16/24/32-bit intermediates).
- MIDI note/index paths: noteOn/noteOff multicast routing is bounded (forEachAcceptingPart 0..5,
  keyrange bytes atomic); UNISON_2X v0/v1 index arithmetic stays < n (allocator size halved,
  SynthEngine.cpp:1292-1301); NoteStack has the free-slot bail and saturation guards (NoteStack.h:46-83);
  FX mod-matrix dest/source bounds-checked in renderPartFx (SynthEngine.cpp:2325-2333).
- Stuck-note backstops verified present: killGeneratedNotes_ service, partAllNotesOff (pedal no-op per
  firmware), sustained-note drain with overflow handling (Part::addSustainedNote), and the
  releaseNoteInPart defensive scan — none of which rescue F-eng-2's chord case.

## Count summary
4 findings: 1 high (F-eng-1 OOB write via unclamped .MUL/host-state Patch bytes), 2 medium
(F-eng-2 arp CHORD stuck notes; F-eng-3 FxChain latency()/pending_ audio-thread race), 1 low
(F-eng-4 CYCLIC note-127 sentinel collision, firmware-faithful latent).
````

**10-line summary:**
1. Reviewed engine + DSP tree read-only: SynthEngine, AmbikaVoice, Arp/Seq/NoteStack/TransportClock, dsp voice/filter/osc/env/lfo/fixed_math, full fx/ tree, plus load paths.
2. F-eng-1 (high): .MUL and host-state Patch bytes reach `Voice::ProcessModulationMatrix` unclamped; `dst_[destination]` (voice.cpp:325) is an OOB WRITE up to ~510 bytes past a 19-entry array on the audio thread (heap corruption of adjacent AmbikaVoice state).
3. Same finding family: OOB reads via mod source (voice.cpp:314), modifier operands (voice.cpp:242), LFO-rate/portamento/env LUTs (voice.cpp:107,190; AmbikaVoice.h:83; envelope.h:102-107), wavetable defs (oscillator.cpp:460,465).
4. All other inputs ARE clamped (.PRO via APVTS, .parvati via descriptor jlimit, arp/seq PartData at staging) — the .MUL/host-blob Patch path is the missing one; loader_fuzz does not fuzz .MUL Patch bodies.
5. F-eng-2 (medium): arp CHORD direction never sends note-offs on key release (allNotesOff loops an already-empty stack); firmware part.cc:347-354 explicitly handles this "to avoid stuck notes" — the port dropped it; no test pins chord mode.
6. F-eng-3 (medium, race): FxChain::latency() dereferences MT-owned `pending_` from process() without checking stageState_; the MT take-back + `pending_[slot] = std::move(next)` destroy creates an audio-thread UAF window; TSAN-detectable, concurrency_test op 10 passes only on timing.
7. F-eng-4 (low): CYCLIC allocator's 0xff cleared-slot sentinel is byte-identical to a live note-127 entry → wrong-voice release / stranded note; firmware-identical, latent.
8. Verified clean: realtime allocation discipline, dirty-flag publish patterns, seqlock reader, SVF div-zero floors, TransportClock spin guard, FX block-size clamps, rate bridges, NoteStack guards, UNISON_2X/CHAIN index math.
9. Recommended deterministic checks: ASAN build with patch byte 51 = 200 (.MUL) + one note-on; headless chord-arp release test counting active voices; TSAN build isolating concurrency_test op 10.
10. Findings count: 4 (1 high / 2 medium / 1 low); artifact content for audit/bughunt_20260818/lane_engine.md delivered above (no write tool available; runtime should persist it).

Output artifact: /Users/fuzboxz/parvati/.pi/subagents/artifacts/5dd1ff62_reviewer_0_output.md
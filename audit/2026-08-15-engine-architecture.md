# Parvati Engine Architecture Audit — 2026-08-15

**Scope:** `Source/SynthEngine.{h,cpp}`, `Source/AmbikaVoice.{h,cpp}`, `Source/Arpeggiator.{h,cpp}`, `Source/Sequencer.{h,cpp}`, `Source/NoteStack.h`, `Source/TransportClock.h`, `Source/MidiParameterMap.{h,cpp}`, `Source/ParameterLayout.{h,cpp}`, `Source/PluginProcessor.{h,cpp}`, cross-checked against `ambika_reference/controller/{voice_allocator.cc,part.cc}`, the vendored JUCE 9 checkout at `~/JUCE`, and `tests/` (coverage claims verified by grep).
**Method:** static read-only audit. Every claim below is **verified** (code path read end-to-end) unless explicitly marked *hypothesis*. No files were modified.

## 1. Architecture map

```
Host (DAW)                          UI (Source/ui, PluginEditor)
   │ automation setValue()             │ APVTS attachments / direct engine calls
   ▼                                   ▼
ParvatiAudioProcessor (PluginProcessor.cpp, ~1190 lines)
 ├─ APVTS (ParameterLayout.cpp: ~480 descriptors incl. arp/seq/fx/option routing flags)
 ├─ MidiParameterMap (CC/NRPN → APVTS, runs inside processBlock)
 ├─ file I/O orchestration (.PRO/.MUL/.parvati, state persistence)
 └─ SynthEngine : juce::Synthesiser (SynthEngine.cpp, ~1722 lines)
     ├─ Part[6]: patchBytes/partBytes (AtomicByteArray), arp+seq objects, routing
     │   atomics, pendingConfig_ (seqlock), fxState (PartFxState), voiceAllocation/
     │   voiceSlots, PolyAllocator/monoStack, voiceIndices (pool slices)
     ├─ AmbikaVoice ×96 (pool): integer dsp::Voice @39216 Hz → AnalogFilter → VCA
     │   → FIFO → Lagrange resampler → per-voicecard mono buffer (6)
     ├─ TransportClock (24-PPQN from host bpm) → per-part Arp/Sequencer → note
     │   callbacks → triggerNoteInPart (bypasses channel routing)
     └─ FxChain[6] (Clouds-derived), renderPartFx: mod matrix @980 Hz, rep-voice
         tracker, crossfades, de-click, master EQ — fed from voicecard buffers
Threading model: MT stages (atomics + dirty flags: frameDirty_/configDirty_/
fxDirty_/optionsDirty_/allocationDirty_/resetAllVoicesPending_); AT is sole owner
of voices, voiceIndices, live arp/seq objects, FX chains.
```

**Dependency direction (verified includes):** `dsp/*` ← engine files ← `PluginProcessor` ← `ui/*`; no engine→UI dependency. Two wrinkles: (a) `SynthEngine.cpp:5` includes `ParameterLayout.h` (engine layer depends on the host-parameter layer for the init patch bytes); (b) `ui/PatchPage.cpp:47` holds a direct `SynthEngine&` and mutates it (see F5).

## 2. Top structural problems (ranked)

### F1 — CRITICAL: per-part FX input sum uses pool voice index as voicecard buffer index
**Location:** `SynthEngine.cpp:1534-1536` (mono sum in `renderPartFx`), vs. the actual render routing at `SynthEngine.cpp:1452-1458` (`av->renderNextBlock (voiceCardBuffers_[av->getVoiceCard()], …)`).

```cpp
for (int vi : part.voiceIndices)
    if (vi >= 0 && vi < kNumParts)                                             // ← vi is a POOL index (0..95)
        juce::FloatVectorOperations::add (mono, voiceCardBuffers_[(size_t) vi]…); // ← but indexes CARD buffers (0..5)
```
This was correct in the pre-pool model (voice *i* == voicecard *i*); after the 96-voice pool extension `voiceIndices` holds pool indices, so:
- **Slots ≠ card count:** Part 0 (1 card, 16 slots) gets FX input = buffers 0..5 = *every part's* audio (cross-bleed into the audible main bus, since `PluginProcessor.cpp:276-281` sums `fxOutputBuffers_`); parts whose pool slices start at ≥6 get **silent FX input** (`vi < kNumParts` always false). `tests/voice_slots_test.cpp:123-144` ([c]) creates exactly this layout but asserts only voice counts, never FX output.
- **Non-contiguous card claims in AUTO mode:** the firmware factory multi (bitmasks 0x15/0x2a, cards {0,2,4}/{1,3,5} — `SynthEngine.cpp:34-36` comment) gives Part 0 pool voices 0..2 → sums buffers 0,1,2 = its card-0 + card-2 audio **plus Part 1's card-1 audio, missing its card-4 audio**. Loadable via `loadMultiFile` (`PluginProcessor.cpp:739-801`, `setPartVoiceAllocation(i, pm[3])`).
- **CHAIN doubling** shifts later parts' pool indices past 6 the same way (`SynthEngine.cpp:663-680`).
- **Test gap (verified):** every engine-level FX test (`fx_param_coverage_test.cpp:711,770`, `parvati_fx_voice_mod_test.cpp:58`, `parvati_fx_modrate_test.cpp:289`, `parvati_fx_engine_continuity.cpp`) uses Part 0 with the default allocation; `fx_routing_test.cpp` tests `FxChain` standalone. No test combines slots/bitmask changes with FX rendering.
**Fix:** sum over the part's owned card bitmask (or `av->getVoiceCard()` deduped) — e.g. `for (int vi : part.voiceIndices) if (auto* av = getAmbikaVoice(vi)) cards |= 1<<av->getVoiceCard();` then add each set card's buffer once. Add a regression test: 2 parts, one with slots>cards, FX enabled on both; assert Part 1 FX output non-zero and Part 0 FX output excludes Part 1's signal.

### F2 — IMPORTANT (verified): APVTS `parameterChanged` fires synchronously on the *calling* thread — audio-thread writes violate the engine's single-writer invariants
**JUCE 9 evidence** (vendored checkout): `juce_AudioParameterFloat.cpp:98` / `juce_AudioParameterInt.cpp:72` / `juce_AudioParameterChoice.cpp:75` — `setValue()` calls `valueChanged()` inline; `AudioProcessorValueTreeState.cpp:62-68` (`Parameter::valueChanged` → `onValueChanged`) and `:85-86`, `:148-159` (adapter → `listeners.call(parameterChanged)`). The APVTS Timer (`:472-478`) only flushes to the ValueTree; it does **not** defer listener dispatch. Therefore **host automation (`setValue` on the audio thread) and `MidiParameterMap`'s `setValueNotifyingHost` (called from `processBlock`, `PluginProcessor.cpp:187` → setter lambda `:53-58`) execute `ParvatiAudioProcessor::parameterChanged` (`:319`) on the audio thread.** The project's own comments assume otherwise (`PluginProcessor.h:48` "ValueTree/timer-routed"; `ui/FxSlotCard.cpp:171-172` "synchronous on the message thread") — both are wrong under automation/CC.
Consequences:
1. **Seqlock invariant break:** `Part::pendingConfig_`'s seqlock assumes "Single message thread => sole writer" (`SynthEngine.h:207-213`). CC102-106 (→ arp_mode…arp_resolution, `MidiParameterMap.cpp` cc-map rows 96-111 → `firmware_parameters[49..53]`), NRPN 119-126, or host automation of any `arp_*` param reaches `applyArpParameter` (`PluginProcessor.cpp:408-421`) → `engine_.setArpMode/setArpDirection/…` (`SynthEngine.h:339-346`, `SynthEngine.cpp:240-247`) → `writePendingConfig` **on the audio thread**. Concurrent with a UI arp/seq edit, two writers can interleave `pendingSeq_` even→even over a torn `PendingConfig` — exactly the crash class the seqlock was added to fix. *Runtime manifestation inferred (path verified; not reproduced here).* Recommended verification: extend `tests/concurrency_test.cpp` to drive CC102-106 on the audio thread while a UI-style thread writes arp params, under TSAN.
2. **RT hazard:** automation of `part_select` → `applyOptionParameter` (`PluginProcessor.cpp:426-427`) → `onPartSelect` (`:532-543`) → `loadPartIntoApvts` + `syncAllParamsToEngine` — ~800 engine-setter calls plus ~400 `getParameterAsValue` ValueTree writes (allocation + `LockedListeners` mutex, `AudioProcessorValueTreeState.cpp:179-199`) **on the audio thread**.
3. The `loadingPartIntoApvts_` feedback-suppression flag (`PluginProcessor.h:266-278`) is a plain bool read/written across threads under this dispatch model — same family.
**Fix (pick one, lowest-risk first):** (a) in `parameterChanged`, `jassert (juce::MessageManager::existsAndIsCurrentThread())` in debug and defer non-atomic classes (arp/seq/part_select) via a preallocated lock-free command ring drained by a `Timer`; (b) make the seqlock multi-writer (spinlock around `writePendingConfig`) — cheap, keeps CC latency; (c) mark `part_select` non-automatable (`isAutomatable() == false`). Note (a) is the only one that also fixes the RT hazard.

### F3 — IMPORTANT: "current-part-only" setter API blocks per-part features and forces a UI hack
`applyPatchByte/applyPartByte` (`SynthEngine.cpp:184-218`), all arp/seq setters (`SynthEngine.h:335-346`), and all FX setters (`SynthEngine.cpp:280-357`, macro `PARVATI_FX_CURRENT_PART`) implicitly target `currentPart_`. The Multi page already works around it: `ui/PatchPage.cpp:411-414` saves `currentPart_`, temporarily switches, calls `applyPartByte(15, mode)`, restores — a window in which any concurrent parameter-routed edit lands on the wrong part. Every planned per-part feature (per-part macros, per-part FX editing without part-switch, arrangement presets) needs part-explicit entry points; doing it later means touching every call site twice.
**Fix:** add `applyPatchByte(int part, …)`, `applyPartByte(int part, …)`, `setArpMode(int part, …)`, FX setters with `part`; make the current-part variants thin wrappers; encapsulate `Part` fields (currently fully public — `voiceIndices` is read directly by tests and the FX loop) behind accessors. Mechanical, gated by existing tests.

### F4 — IMPORTANT: arp/sequencer note timing is block-quantized, not sample-accurate
`processTransport` advances the whole block at once and fires all ticks at block start: `SynthEngine.cpp:1372-1390` (`const int ticks = transport_.advance (numSamples); for (t…) …`), with generated notes triggered immediately through callbacks (`SynthEngine.cpp:44-67` → `triggerNoteInPart`, `:866-953`). Pass-through host MIDI, in contrast, gets sub-block sample-accurate splitting in `juce::Synthesiser::renderNextBlock`. Result: up to one block of jitter *and* systematic early bias (a tick near block end fires at block start). At 1/64 resolution a step is ~31 ms; a 512-sample block @48 kHz is ~10.7 ms — audible/tightness-relevant, and inconsistent between hosted and generated notes. `TransportClock::advance` returns only an integer tick count (`TransportClock.h:36-46`) — no sample positions available to callers.
**Fix:** have `advance` emit tick sample positions (small fixed array, bounded by `numSamples/samplesPerTick+1`), queue generated note-ons with offsets, and trigger them at their position (e.g. drain the queue in the `renderVoices` sub-block loop, which already receives `startSample`). Keep the direct-voice path — do *not* route generated notes through the MIDI buffer: octave-shifted arp notes must bypass `findPartForNote`'s keyzone check (`SynthEngine.cpp:814-828`), which is why the callback bypass exists.

### F5 — IMPROVEMENT: SynthEngine is a god-object absorbing every new subsystem
`SynthEngine.cpp` (~1722 lines) + header (~700 lines) now contain: the controller/part model, the voice pool and allocator, the transport/arp/seq driver, the **entire per-part FX rack** (`renderPartFx`, `SynthEngine.cpp:1474-1722: ~250 lines covering fxDirty service, rep-voice tracker, 5 ms crossfade, mod matrix with AC/DC coupling, base de-click, drift-free sub-chunk phase), a versioned serialization format (`captureState/restoreState`, `:329-556`), and permanently-compiled test instrumentation (`debug*` members, `SynthEngine.h:435-480`). The header also defines four nested domains (`PolyAllocator` `:60-114`, `AtomicByteArray` `:123-156`, `PartFxState` `:158-186`, `Part` `:188-278`). Cost: compile coupling (any FX change rebuilds all engine clients), review surface, and every future per-part feature grows the same two files. `docs/ARCHITECTURE.md:33-51` already names the target (engine-authoritative state, unified apply); extraction is the missing half.
**Fix:** extract `PolyAllocator` (+ its own unit test — it currently has none in isolation), a `PartFxRack` owning `renderPartFx` + FX debug hooks, and `EngineStateCodec` (capture/restore). Pure moves; behavior-preserving.

### F6 — IMPROVEMENT: PartData byte-layout knowledge exists in three places
(1) `ParameterLayout.cpp` descriptor `byteOffset` + `+112` part convention (`MidiParameterMap.cpp:284-291` re-derives it, with a string-matched special case for arp IDs `:270-281`); (2) `stageArpSeqFromPartBytes` hard-codes offsets 7..14/16..79 (`SynthEngine.cpp:262-272`); (3) `captureState` overlays the same region (`SynthEngine.cpp:401-410`). Ranges are also duplicated: CC scaling uses the *descriptor* range (`MidiParameterMap.cpp:241-250, 305-330`) while `firmware_parameters[58]` min/max (`:135-230`) are documentation-only — a silent divergence (e.g. `lfo_rate` max `kNumSyncedLfoRates+127` vs firmware 142) changes CC scaling with no test signal. Also: the NRPN handshake state machine is channel-agnostic (`handleBuffer` `:370-445` ignores `msg.getChannel()`), which matches the single-cable hardware but interleaves badly under MPE multichannel input.
**Fix:** one shared table of PartData/Patch byte roles consumed by all three sites; a unit test asserting descriptor ranges == firmware table ranges.

### F7 — NOTES (verified, lower rank)
- **Transport not anchored to the host timeline:** `TransportClock` free-runs from plugin start; arp/seq restart only on `isPlaying` edges (`SynthEngine.cpp:1284-1291`). Host loops/locates within continuous playback leave the arp phase off-grid. 
- **MPE + arp:** arp/seq-generated notes are tagged with the *part's* channel (`SynthEngine.cpp:48-50`), so per-note expression of the held key does not follow the generated voice under an Omni/MPE part (documented at `:874-881`). CC4 (global foot) and CC74 (per-note slide) both write `MOD_SRC_EXPRESSION` — last-writer-wins quirk documented at `:984-1010`.
- **Dead/misleading surface:** `getArp()/getSequencer()` (`SynthEngine.h:534-535`) return references to AT-owned objects, unused in Source — remove before someone calls them from the editor. Stale comments: `killGeneratedNotes_` "flagged by setArpMode (message thread)" (`SynthEngine.h:191-195`; actually set by the AT config service, `SynthEngine.cpp:1257-1265`); `PluginProcessor.h:7` "16 AmbikaVoice instances" (now 96); `setFxMix` "FxChain does not consume these yet" (`SynthEngine.cpp:315-317`; consumed at `:1520-1527`).
- **`NoteStack` accessors** (`NoteStack.h:166-179`) are unchecked; all current call sites clamp correctly (verified in `Arpeggiator::clockArpeggio/stepArpeggio`), but a future edit can OOB-read `pool_[sorted_ptr_[index]]`. Add `jassert`s.
- **`AmbikaVoice::fifo_`** is a `std::vector` used as a FIFO with front `erase` per chunk (`AmbikaVoice.cpp:~497`) — O(n) memmove on the AT per 256-sample chunk; a ring/index would remove it. Reserved correctly (`:65-70`).
- **`setNoteStealingEnabled(true)`** (`SynthEngine.cpp:30`) is dead: `noteOn/noteOff` are overridden (`SynthEngine.h` overrides; `SynthEngine.cpp:1067-1091`), so JUCE's steal path never runs; stealing is entirely `PolyAllocator`'s (correctly per-part).

## 3. Part/voice/card model & the voice-slot extension — assessment

**Verified invariants:** exclusive card ownership enforced at write time (`setPartVoiceAllocation`, `SynthEngine.cpp:717-746`) with first-wins `claimed[]` as backstop (`:612-640`); pool always satisfies the sum of slot settings (96 = 6×16, `jmin` clamp is defensive); allocation never steals between parts (by design, `SynthEngine.h:28-37`); dropped voices get tail-off release, never Kill (`:674-698`) — the "dead voice" glitch avoidance; per-card MONO makes unison size card-count-invariant (`:886-915`, tested `voice_slots_test.cpp:180-211`); state v6 + `.parvati` + `.MUL`-export fallback round-trip slots (`voice_slots_test.cpp:213-290`, `MulExport`).

**How well the pattern extends:** well, with two caveats. (1) **F1 is the one residual "voice == card" assumption** — fix it and the model is consistent. (2) Aux outputs remain card-granular (6), so a 16-slot part's voices share its ≤6 card buses — acceptable hardware parity, but worth a doc note since per-part FX outputs are the new per-part audio boundary. CPU scaling is correctly gated (idle pool voices self-silence, `AmbikaVoice.cpp:441-452`). The `voiceCardForIndex` pre-tagging (`SynthEngine.cpp:1414-1420`) is dead weight after the first rebuild — fold into the rebuild only.

## 4. Refactor roadmap (order, rationale, risk)

| # | Step | Why this order | Risk / gate |
|---|------|----------------|-------------|
| 0 | **Fix F1** (FX mono sum over owned cards) + regression test (2 parts, slots≠cards, FX on both) | Audio-correctness bug in the flagship per-part feature; independent, tiny diff | Very low. Gate: new test + existing FX suites |
| 1 | **F2 discipline**: jassert MT in `parameterChanged`; defer arp/seq/`part_select` writes via preallocated lock-free ring drained by a Timer; mark `part_select` non-automatable | Protects every later step's single-writer assumptions; without it, new per-part setters inherit the race | Medium (dispatch change). Gate: extended `concurrency_test.cpp` driving CC + automation under TSAN |
| 2 | **Part-explicit engine API** (F3): `applyPatchByte(part,…)`, arp/seq/FX setters with part; wrappers for current-part; make `Part` fields private; delete the PatchPage `setCurrentPart` dance | Prerequisite for all planned per-part features; mechanical now, expensive later | Low-medium. Gate: `multitimbral_test`, `voice_slots_test`, `partstate_test` |
| 3 | **Extractions** (F5): `PolyAllocator.h` + unit tests; `PartFxRack`; `EngineStateCodec`; move init-patch bytes out of `ParameterLayout.h` (breaks the engine→parameter-layer include, `SynthEngine.cpp:5`) | Compile-time decoupling; shrinks review surface before feature work | Low (pure moves). Gate: build + full test suite bit-identical |
| 4 | **Sample-accurate arp/seq** (F4): tick positions from `TransportClock`, offset-scheduled generated notes | Timing quality; touches the render loop, so do it after the API/structure settles | Medium. Gate: new timing test asserting first-sample offsets; `parvati_arp_seq_timing_test` |
| 5 | **Single PartData byte-map** (F6) + range-parity test | Removes triple-maintained layout knowledge; cheap | Low |
| 6 | *(Only if needed)* fold the five dirty flags into the per-part version-counter model per `docs/ARCHITECTURE.md:40-44` | Big blast radius; current flags are tested and TSAN-clean for their covered paths | High — defer until a feature demands it |

## 5. Residual risks / hypotheses
- F2's *runtime* manifestation (torn `pendingConfig_`) is path-verified but not reproduced here; JUCE-side dispatch is verified from the vendored JUCE 9 source (cited). Reproduce under TSAN with CC102-106 + concurrent UI edits.
- `setStateInformation` is treated as message-thread throughout (standard JUCE practice); a host calling it off-message-thread would race `Part::name` (plain `juce::String`, `SynthEngine.h:267-271`). Not observed; listed for completeness.
- I did not audit `Source/dsp/**` internals, `FxChain` DSP math, or UI layout (other lanes).

---

## Executive summary (≤40 lines)

1. **CRITICAL — FX input routing bug** (`SynthEngine.cpp:1534-1536`): `renderPartFx` sums `voiceCardBuffers_[vi]` where `vi` is a *pool* voice index (0..95), but voices render by *card* (`:1452-1458`). Correct only in the default single-part layout. With voice slots ≠ card count, parts >0 get **silent FX** and part 0's FX ingests the whole mix; even the factory multi (0x15/0x2a) cross-bleeds. No test covers FX + non-default pool. Fix: sum owned-card bitmask.
2. **IMPORTANT — audio-thread APVTS dispatch breaks single-writer invariants**: verified in JUCE 9 source that `setValue` → `valueChanged` → `parameterChanged` is synchronous on the calling thread. Host automation and the CC/NRPN path (`PluginProcessor.cpp:187→53-58→319`) therefore run engine setters on the audio thread: the `pendingConfig_` seqlock gets a second writer (arp/seq via CC102-106/NRPN/automation — torn config, UB) and `part_select` automation triggers `onPartSelect`'s ~800 setter + ValueTree writes on the RT thread. Project comments assuming message-thread dispatch are wrong. Fix: assert + defer via lock-free ring.
3. **IMPORTANT — current-part-only setters** (`SynthEngine.cpp:184-218` etc.) force the UI's `setCurrentPart` dance (`PatchPage.cpp:411-414`) and block every planned per-part feature; add part-indexed setters now (mechanical).
4. **IMPORTANT — arp/seq timing is block-quantized**: all ticks fire at block start (`SynthEngine.cpp:1372-1390`), no sample positioning — up to one block of jitter/early bias vs sample-accurate hosted notes. Fix: tick sample positions + offset-scheduled triggers.
5. **IMPROVEMENT — god-object drift**: SynthEngine holds controller + pool + transport + full FX rack (~250-line `renderPartFx`) + serialization + test hooks; header embeds 4 domains. Extract `PolyAllocator`/`PartFxRack`/`EngineStateCodec`.
6. **IMPROVEMENT — PartData layout knowledge triplicated** (ParameterLayout / MidiParameterMap / `stageArpSeqFromPartBytes`) with divergent range tables for CC scaling; NRPN handshake is channel-agnostic under MPE.
7. **Voice-slot pattern verdict**: extends well — invariants (exclusive cards, pool partition, per-card MONO, tail-off release, state round-trip) all verified; F1 is the one leftover "voice==card" assumption; aux buses stay card-granular (document).
8. **Roadmap**: (0) fix F1 + regression test → (1) dispatch discipline/defer ring → (2) part-explicit API → (3) extractions → (4) sample-accurate arp/seq → (5) shared byte-map. Risks low→medium, each test-gated.
9. **Notes**: dead `getArp()/getSequencer()`, 4 stale comments (killGeneratedNotes_, "16 voices", setFxMix), unchecked `NoteStack` accessors, `fifo_` vector front-erase on AT, dead `setNoteStealingEnabled`, transport not re-anchored on host loops, MPE+arp notes tagged with part channel.

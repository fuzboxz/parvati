# Parvati Deep Audit — State Management, Threading, Persistence
Date: 2026-08-15 · Scope: Source/ParvatiPreset.{h,cpp}, Source/PatchFile.{h,cpp},
Source/MulExport.{h,cpp}, Source/PluginProcessor.{h,cpp} (get/setStateInformation + cross-thread
plumbing), Source/ui/SharedContainer.{h,cpp,mm}, state-related tests under tests/.
Method: full read of every in-lane file at HEAD; JUCE 9 semantics cross-checked against the vendored
checkout at ~/JUCE (APVTS ParameterAdapter, AudioProcessorParameter listeners, AUv3/VST3 wrappers).
No project files were modified. No builds/tests were executed (read-only audit); recommended
commands for the supervisor are listed in §9.
Severity scale: critical / important / improvement / note. "Verified" = read from source;
"hypothesis" = mechanism verified, manifestation depends on timing/ordering (stated where so).

---

## 1. End-to-end threading model (as designed vs. as actually exercised)

### 1.1 The designed model (verified, and largely good)
- Two threads own the engine: the message thread (MT) mutates staging state; the audio thread (AT)
  is the sole mutator of live render state. Every MT→AT transfer is a release-store on a dirty flag
  serviced at the top of `processTransport` (SynthEngine.cpp:1172–1300):
  resetAllVoicesPending_ (:1179–1183), per-part frameDirty_ → pushPartBytesToVoices (:1189–1194),
  optionsDirty_ (VCA/smoothing/drive, :1199–1212), allocationDirty_ → rebuild + push (:1218–1233),
  configDirty_ → servicePendingConfig (:1240–1272), killGeneratedNotes_ (:1283–1291).
- Patch/part bytes are per-byte atomics (`AtomicByteArray`, SynthEngine.h:112–146) — TSAN-clean
  under concurrent re-dirtying; FX state is the same pattern (PartFxState, SynthEngine.h:155–178).
- pendingConfig_ is a seqlock: MT sole writer / AT sole reader (SynthEngine.h:269–311).
- RT path is lock/allocation-free: processBlock's overrun probe uses steady_clock + relaxed
  atomics + a CAS peak loop (PluginProcessor.cpp:152–304); midiCollector_ and the reused
  processedMidi_ scratch avoid per-block allocation (SynthEngine.h:604–607).
- Latency reporting is staged (`stagedOsLatencyInputSamples_`, PluginProcessor.h:289–296;
  computePluginLatency reads only the staged int — the probe unique_ptr is never dereferenced on
  the AT). Good.

### 1.2 The actual model: parameterChanged runs on the AUDIO thread — and the code assumes it doesn't
Verified chain (this is the audit's central finding):
1. JUCE 9 `AudioProcessorParameter::setValueNotifyingHost` → `setValue` +
   `sendValueChangedMessageToListeners` — synchronous on the CALLING thread
   (~/JUCE/modules/juce_audio_processors_headless/processors/juce_AudioProcessorParameter.cpp:59–63, 104–115).
2. APVTS `ParameterAdapter::parameterValueChanged` → `listeners.call(...)` — synchronous, same
   thread (~/JUCE/modules/juce_audio_processors/utilities/juce_AudioProcessorValueTreeState.cpp:148–159).
3. AUv3 (the primary iOS target): host parameter automation arrives as `AURenderEventParameter`
   inside the render callback → `setAudioProcessorParameter` → `sendValueChangedMessageToListeners`
   — ON THE RENDER THREAD (~/JUCE/modules/juce_audio_plugin_client/juce_audio_plugin_client_AUv3.mm:1517–1526, 1435–1443).
4. VST3: `process()` → `processParameterChanges` → `setValueAndNotifyIfChanged` →
   `setValueNotifyingHost` — ON THE AUDIO THREAD
   (~/JUCE/modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp:3590–3591, 827–834).
5. In-repo, independent of any host: `midiParamMap_.handleBuffer (midiMessages)` runs at the top of
   `processBlock` (PluginProcessor.cpp:187); its setter calls `setValueNotifyingHost`
   (PluginProcessor.cpp:54–58) → `parameterChanged` executes on the AUDIO THREAD for every
   CC/NRPN-mapped parameter.

Consequence: `ParvatiAudioProcessor::parameterChanged` (PluginProcessor.cpp:319–353) MUST be
RT-safe and assume any thread. Most branches are (they stage atomics). Two are not:

#### CRITICAL-1 — Host automation of `part_select` executes ValueTree/UndoManager writes on the render thread
- `part_select` is an automatable `AudioParameterChoice` (ParameterLayout.cpp:418–428; created
  without non-automatable attributes, ParameterLayout.cpp:536–553).
- parameterChanged → isOption → `applyOptionParameter` → `onPartSelect` (PluginProcessor.cpp:344,
  426–428) → `loadPartIntoApvts` + `syncAllParamsToEngine` (PluginProcessor.cpp:532–542).
- `loadPartIntoApvts` writes ~250 parameters via `apvts.getParameterAsValue(id) = value`
  (PluginProcessor.cpp:544–624). Each write is a `ValueTree::setProperty` bound to the
  **UndoManager** (~/JUCE .../juce_AudioProcessorValueTreeState.cpp:356–363: `getPropertyAsValue
  (valuePropertyID, undoManager)`), i.e. heap allocation + undo transaction, executed on the
  render thread, concurrently with the APVTS's own 50 Hz flush timer on the message thread
  (`timerCallback` → `flushParameterValuesToValueTree`, same file :472–478) and with editor
  attachments/undo polling (PluginEditor.cpp:3044). ValueTree is not thread-safe → data race (UB)
  plus guaranteed RT violation (~250 allocations/undo actions per automated part switch).
- Recommendation (concrete): (a) mark part_select non-automatable
  (`juce::AudioParameterChoiceAttributes().withAutomatable(false)` in ParameterLayout.cpp:543–545)
  — part switching is a UI action, not a sound parameter; and/or (b) make `onPartSelect` deferred:
  store the requested part in a `std::atomic<int>` and perform the switch on the message thread
  (AsyncUpdater already exists as a pattern in the UI). Add a TSAN regression test that calls
  `setValueNotifyingHost(part_select…)` from a non-message thread while processBlock loops.

#### IMPORTANT-1 — The pendingConfig_ seqlock has reachable dual writers (torn config = the historical SIGBUS class)
- The seqlock's correctness requires a SOLE writer; SynthEngine.h:277 states "Single message thread
  => sole writer", and tests/mt_harness.h:20–24 codifies that assumption ("its seqlocks assume a
  sole writer"). Two concurrent `writePendingConfig` calls make `pendingSeq_` EVEN while both
  lambdas mutate `pendingConfig_` (each does fetch_add; two adds cancel the odd parity,
  SynthEngine.h:278–286) — the AT reader then accepts a torn snapshot
  (SynthEngine.cpp:1255). The project has already been bitten by exactly this state
  (CHANGELOG.md:496–508: TSAN-confirmed race → SIGBUS via corrupted std::function).
- Reachable writers today:
  * MT: GUI arp/seq edits (setArpMode… setSequenceDataByte, SynthEngine.cpp:243–246;
    SynthEngine.h:363–369), file loads (stageArpSeqFromPartBytes, SynthEngine.cpp:248–265),
    applyParvatiMulti (HellcatPreset.cpp:558–585).
  * AT: MIDI NRPN. `MidiParameterMap::initialise` explicitly maps arp_mode…arp_resolution to
    NRPN addresses 119–123 and seq data to 112+byteOffset (MidiParameterMap.cpp:227–244);
    `handleBuffer` runs in processBlock (PluginProcessor.cpp:187); `midi_nrpn_map[119..123]` is
    mapped (not 255/254) so the NRPN gate passes (MidiParameterMap.cpp:336–343, table :60–96) →
    setter → setValueNotifyingHost → parameterChanged → applyArpParameter/applySequencerParameter
    → writePendingConfig ON THE AUDIO THREAD.
  * AT: host automation render events for arp/seq params (chain in §1.2 items 1–4).
- Failure window: a GUI arp/seq knob drag concurrent with NRPN/automation of any arp/seq param of
  the same Part. Low probability per event, but it is precisely the crash class already observed
  once in production.
- Recommendation: funnel ALL audio-thread-origin parameter writes back to the MT before they reach
  the engine setters (e.g. the midiParamMap setter defers via `MessageManager::callAsync`; the ≤1
  block added latency matches the existing frameDirty_ model), or replace the seqlock with
  per-field atomics / an SPSC edit queue consumed by the AT. Then delete the false sole-writer
  claims in SynthEngine.h:277 and mt_harness.h:20–24, and add a TSAN test driving arp NRPN edits
  from a second (non-MT) thread while the MT edits arp params.

---

## 2. Atomicity of parameter changes mid-block
- Verified good: patch/part bytes apply as whole frames at block top (frameDirty_ exchange +
  pushPartBytesToVoices, SynthEngine.cpp:1189–1194); FX applies whole frames in renderPartFx
  (fxDirty_ service; AT-side fxCached_ reused between edits, SynthEngine.h:581–590); arp/seq
  config applies as a complete snapshot (configDirty_ + seqlock read, SynthEngine.cpp:1240–1272);
  the FX mod matrix is written three-fields-at-once via setFxModSlot to avoid torn slots
  (SynthEngine.h:472–480; applyFxParameter re-reads siblings, PluginProcessor.cpp:494–507).
- Note: parameter changes are block-granular, not sample-accurate (AUv3/VST3 per-sample event
  offsets are collapsed before staging). Acceptable for a knob synth (7-bit params + optional
  smoothing), but worth documenting: a future "sample-accurate automation" claim would be false.
- Note (benign races): `hostSampleRate_` / `lastReportedLatency_` are plain ints written from both
  prepareToPlay and processBlock (PluginProcessor.h:286–296; PluginProcessor.cpp:70–73, 178–186).
  Monotonic small ints; no action required beyond awareness.

## 3. Timer / async patterns — remaining storms?
- The fixed ping-pong: PatchPage card-combo rebuild used `clear()`'s default
  sendNotificationAsync, arming a deferred onChange after `refreshing_` reset → infinite rebuild
  loop at 100% CPU. Fixed with `dontSendNotification` + documented (PatchPage.cpp:321–334).
  Verified no other `clear()`/rebuild path uses async notification on combos (grep across
  Source/ui: only SeqLengthStepper.cpp:50 writes a value async — single-shot, not a rebuild loop).
- Audited remaining patterns: FxSlotCard (AsyncUpdater, coalesced, FxSlotCard.cpp:473–485),
  FxRoutingBar (AsyncUpdater repaint, FxRoutingBar.cpp:244–247), 30 Hz display timers with
  eps-diff gates (FilterResponseDisplay.cpp:95+, EnvelopeDisplay.cpp:51+, OscPreviewDisplay.cpp:116+,
  VoiceMeter.cpp:87+, ModMatrixView.cpp:107, FxMatrixView.cpp:117), editor adaptive 30/4 Hz timer
  (PluginEditor.cpp:3223–3241) with an anti-flicker hold gate on the CPU label
  (PluginEditor.cpp:3073–3085). No remaining unconditional rebuild loops.
- IMPROVEMENT-5: `refreshPartComboNames()` runs every timer tick and ends with an unconditional
  `partCombo_.repaint()` even when no label changed (PluginEditor.cpp:3021; 2985–2995). At 30 Hz
  busy-state this repaints a ComboBox continuously. Guard: track the last label set and repaint
  only on change (mirrors the eps-diff gates used elsewhere).

## 4. Preset/state serialization: versioning and compatibility
### 4.1 Engine-state binary blob (DAW state) — good
- v6 format: magic PVST + version + currentPart + per-part {patch[112], part[84] (arp/seq overlaid
  from pendingConfig_), routing[4], length-prefixed FX block, slots+name} (SynthEngine.cpp:370–446).
  Restore is strictly version-gated 1..6 (SynthEngine.cpp:456–459), reads v1–v5 layouts correctly
  (param-count gate :502–512, v3 keepTails discard :526–533, v6 tail :541–562), and a too-new blob
  is rejected → caller falls back to legacy current-part restore (PluginProcessor.cpp:1087–1101).
  Forward-safety of the FX block is real (length prefix skip, :479–487). Tested: host_state_test
  [3] (v6 round-trip), [4] (hand-crafted v1), [5] (hand-crafted v2), [2] (no-blob legacy).
- NOTE: forward compat is "reject", not "partial load" — a v7 blob in an old plugin silently
  drops 5 parts. Acceptable (documented in CHANGELOG), but a one-line stderr/log would help
  field debugging.

### 4.2 .parvati YAML format — good forward-compat mechanics, unguarded version
- Unknown keys ignored (applyParvatiPatch/Multi skip unmapped IDs — HellcatPreset.cpp:466–475,
  530–533; tested hellcat_preset_test.cpp:160–172). Escape/quoting round-trips (CHANGELOG:80–83;
  unescapeQuoted HellcatPreset.cpp:107–124). Legacy `voice_mode` option parsed-and-ignored
  (HellcatPreset.cpp:645–653) — correct pattern.
- IMPROVEMENT-2: `version:` is emitted (kFormatVersion=1, HellcatPreset.h:21–27) but NEVER read:
  applyParvatiPatch/applyParvatiMulti/detectParvatiFormat ignore it entirely
  (HellcatPreset.cpp:455–517, 522–690, 696–711). Additive evolution is safe, but any future
  semantic change would silently misload old builds / new files. Recommendation: reject or warn
  when `version > kFormatVersion`.

### 4.3 .PRO/.MUL — verified byte-exact and defensively parsed
- RIFF walker bounds-checks every chunk (PatchFile.cpp:35–58), part routing by type-prefix is
  index-clamped (PatchFile.cpp:245–262); writers use TemporaryFile atomic replace
  (PatchFile.cpp:210–218, 357–365); name chunk writer is UTF-8-codepoint-safe
  (PatchFile.cpp:111–150). Round-trips byte-equal vs the vendored reference files
  (roundtrip_test.cpp:76–159).
- MulExport solver is pure and invariant-tested (MulExport.cpp; mul_strategies_test,
  export_fallback_test). Verified `solveChain` always returns ≥1 unit
  (`std::vector<...> units (1)`, MulExport.cpp:222–226) so `units.front()` in saveMultiFile's
  ChainSplit branch (PluginProcessor.cpp:919–931) cannot UB.

## 5. Host-state sync edge cases
### 5.1 IMPORTANT-2 — `loadMultiFile` desyncs the `part_select` parameter from engine state
- PluginProcessor.cpp:793–795 sets processor+engine current part to 0 and refreshes APVTS display
  values, but `loadPartIntoApvts` skips isOption params (part_select; PluginProcessor.cpp:552–554),
  so the parameter — and the top-bar combo bound via ComboBoxAttachment
  (PluginEditor.cpp:2312–2313) — still shows the previously selected part.
- Contrast: `applyParvatiMulti` explicitly writes part_select before refreshing
  (HellcatPreset.cpp:680–684). The .MUL path forgot the equivalent.
- Effect (verified by code path): load a .MUL while on Part 3 → combo reads "Part 3", APVTS values
  show Part 0, and every knob edit routes to Part 0 (engine setters use the ENGINE's currentPart_).
  Clicking "Part 1" then hits the early-return `if (newPart == currentPart_) return;`
  (PluginProcessor.cpp:535–536) with no refresh, and clicking "Part 3" performs a switch the user
  did not intend. Recommendation: mirror the .parvati epilogue (write part_select=1 with
  setValueNotifyingHost before the loadPartIntoApvts(0) call), and add a regression assertion that
  APVTS part_select == engine.getCurrentPart()+1 after every load path (.PRO/.MUL/.parvati/
  setStateInformation).

### 5.2 IMPORTANT-3 — setStateInformation rests on a false claim about `replaceState`; latent value clobbering + currentPart_ tracking depends on undocumented re-entrancy
- PluginProcessor.cpp:1087–1089 comments: "replaceState restored host automation values but fires
  no parameterChanged, so the part-switch handler is not re-driven." Verified FALSE against the
  vendored JUCE 9: `replaceState` assigns `state = newState` → synchronous `valueTreeRedirected`
  → `updateParameterConnectionsToChildTrees` → per-child `setNewState` → `setDenormalisedValue` →
  `setValueNotifyingHost` → **parameterChanged fires for every parameter whose restored value
  differs** (~/JUCE .../juce_AudioProcessorValueTreeState.cpp:396–404, 406–415, 417–440, 454–458,
  171–177).
- Consequence A (mechanism verified; manifestation depends on child ordering, which follows an
  `std::unordered_map` iteration — same file :338–342): if the restored part_select differs, the
  re-entrant `onPartSelect` runs MID-replaceState and `loadPartIntoApvts` pushes pre-restore
  ENGINE values back into the APVTS, overwriting freshly restored host values for parameters
  whose adapters were already re-bound to the new tree. For states WITHOUT engine_state (legacy),
  the concluding `syncAllParamsToEngine()` (PluginProcessor.cpp:1100) then persists that clobbered
  mix into the engine. For states WITH engine_state the later `restoreState` + explicit
  `loadPartIntoApvts` (PluginProcessor.cpp:1090–1096) repair everything — which is why this has
  gone unnoticed.
- Consequence B: processor `currentPart_` correctness after restore *silently depends* on this
  re-entrancy firing (restoreState only sets the ENGINE's copy, SynthEngine.cpp:576–578; nothing
  else in setStateInformation updates the processor member).
- Recommendation: add a `restoringState_` guard that suppresses parameterChanged side effects
  around `replaceState` (same RAII pattern as loadingPartIntoApvts_, PluginProcessor.cpp:549–550);
  explicitly set `currentPart_ = engine_.getCurrentPart()` after restoreState; fix the comment.
  Regression test: legacy state saved on Part 3 → restore → assert all ~256 APVTS raw values equal
  the saved snapshot, then click part_select=1 and assert Part 0's engine bytes load.

### 5.3 IMPROVEMENT-7 — setStateInformation thread contract is implicit
- setStateInformation touches ValueTree/UndoManager (replaceState, loadPartIntoApvts) and calls
  setOversamplingFactor (PluginProcessor.cpp:1070–1104) under a message-thread assumption. Hosts
  normally comply, but AUv3 "full state restoration" and some VST3 hosts restore on worker
  threads while the editor/50 Hz timer may already run — the same ValueTree race class as
  CRITICAL-1. Recommendation: `if (! MessageManager::existsAndIsCurrentThread())` defer the whole
  body via callAsync (state restores are never latency-critical), or document the contract in the
  header.

### 5.4 NOTE — capture paths can snapshot a torn frame
- captureState (SynthEngine.cpp:370–446), partRaw in serializeParvatiMulti
  (HellcatPreset.cpp:300–368), and saveMultiFile (PluginProcessor.cpp:831–880) copy
  patchBytes/partBytes/fxState with relaxed atomics and no acquire pairing against the
  frameDirty_/fxDirty_ release-stores. A save taken mid-knob-drag can serialize a mixed old/new
  frame (invisible on x86; observable on ARM/iOS weak ordering). Rare and self-healing on the next
  save. Recommendation (IMPROVEMENT-1): acquire-load the dirty flag (or double-check it around the
  copy) before serializing.

## 6. IMPORTANT-4 — readPendingConfig spins unbounded on the audio thread
- SynthEngine.h:294–306 is `for (;;)` with `continue` while the writer is mid-update; the comment
  at :292 claims "Bounded retries". If the MT writer is preempted inside writePendingConfig
  (SynthEngine.h:278–286 — realistic under iPad core contention), the AT burns its entire block
  budget spinning (classic seqlock priority inversion). Recommendation: bound retries (~64) and
  fall back to an AT-cached last-good snapshot (the AT already keeps applied config state; only
  re-read when configDirty_ fires).

## 7. SharedContainer / app-group (iOS AUv3 + Standalone)
- Verified design: `getSharedContainerRoot()` returns the App-Group container
  `group.com.805labs.parvati` on iOS, falling back to the process sandbox when the entitlement is
  absent (SharedContainer.cpp:17–25; SharedContainer.mm:18–33); desktop returns
  userApplicationDataDirectory unchanged (SharedContainer.cpp:27–31 — byte-identical to the
  pre-refactor path). All four roots derive from it (PluginProcessor.cpp:690–692, 803–820).
- Failure modes (IMPROVEMENT-6):
  1. Silent root switch: presets saved under the no-entitlement fallback sandbox become invisible
     once entitlements are fixed (different root). Recommendation: probe once at startup, surface a
     one-line diagnostic (status strip already exists), or migrate on first entitled run.
  2. Cross-process first-run extraction is guarded per-PROCESS only
     (FactoryPresetInstaller.cpp:82–88 `std::call_once` + atomic). FACTORY banks are safe
     (writeIfMissing + atomic TemporaryFile rename, :20–28); the TEMPLATES *sync* deletes stale
     files then rewrites (overwriteIfChanged + prune) — a concurrent reader in the other process
     can transiently observe a missing template. Self-healing; note only.
  3. No cross-process locking anywhere — acceptable given atomic renames; documented behavior
     would preempt future bug reports ("preset vanished" after simultaneous first launch).

## 8. UndoManager interaction (IMPROVEMENT-3)
- loadPartIntoApvts writes ~250 params through `getParameterAsValue`, which is bound to the
  UndoManager (~/JUCE .../juce_AudioProcessorValueTreeState.cpp:356–363); the header advertises
  "every parameter write then becomes undoable" (PluginProcessor.h:100–110). Routine part
  switches therefore push bulk transactions into undo history, and an undo landing mid-part-switch
  replays hundreds of parameter changes. Recommendation: bracket bulk refreshes with
  `undoManager.beginNewTransaction ("Part switch")` (or write display refreshes through
  parameter->setValueNotifyingHost, which defers tree writes to the 50 Hz flush) so undo steps
  remain per-gesture. NOTE-4: `loadedProgramName_` is not persisted in host state
  (getStateInformation writes only ui_* + engine_state, PluginProcessor.cpp:1046–1064) — the GUI
  title falls back after a DAW reload; persist it as another root property.

## 9. Test coverage (verified by reading tests/) — gaps vs. strengths
Covered well:
- Host state: full 6-part round-trip incl. per-part FX (host_state_test.cpp [1],[3]); legacy
  no-blob fallback ([2]); hand-crafted v1 and v2 blobs ([4],[5]).
- Concurrency: MT-drives-everything vs. AT render fuzz, incl. preset switching under load and
  optional MIDI injection (concurrency_test.cpp [1]–[6]); TSAN/ASAN wiring documented
  (mt_harness.h:12–17).
- Formats: .PRO/.MUL byte-exact round-trips vs vendored reference (roundtrip_test.cpp);
  .parvati patch/multi round-trips + unknown-key tolerance (hellcat_preset_test.cpp);
  part-state across saves (partstate_test.cpp); MulExport strategy invariants
  (mul_strategies_test, export_fallback_test).
NOT covered (each maps to a finding above):
1. parameterChanged driven from the AUDIO thread — mt_harness explicitly pins all mutations to the
   message thread and states the sole-writer assumption (mt_harness.h:20–24). This hides
   CRITICAL-1 and IMPORTANT-1 from the entire suite. (The chaos test's MIDI injector fires
   CC 1..32 classes only — never the NRPN sequence that reaches arp/seq.)
2. part_select host automation (render-thread switch) — no test.
3. Legacy-restore value fidelity: host_state_test [2] checks a single byte
   (patchBytes[0]==WAVEFORM_SAW) — cannot detect the §5.2 clobbering.
4. APVTS part_select ↔ engine currentPart consistency after each load path (would catch
   IMPORTANT-2 immediately).
5. UndoManager behavior across part switches (transaction count / undo outcome).
6. SharedContainer fallback behavior — iOS-only, untestable headless; acceptable, but a
   unit-testable seam (inject the root resolver) would let the fallback be tested.
Recommended supervisor commands (not run by this audit):
- Baseline: `cmake --build build_tsan -j8 && ./build_tsan/parvati_concurrency_test` (x3).
- After any fix for CRITICAL-1/IMPORTANT-1: same, plus the new focused repro tests specified in
  those findings.

## 10. Findings index
| ID | Severity | Location | Summary |
|----|----------|----------|---------|
| CRITICAL-1 | critical | PluginProcessor.cpp:319–353, 423–428, 532–624; ParameterLayout.cpp:536–553 | part_select automation runs onPartSelect on the render thread → ~250 ValueTree+UndoManager writes on RT + data race with the 50 Hz APVTS flush timer (AUv3/VST3 wrappers verified). |
| IMPORTANT-1 | important | SynthEngine.h:269–311; MidiParameterMap.cpp:227–248, 305–343; PluginProcessor.cpp:187,54–58 | pendingConfig_ seqlock has reachable dual writers (MT GUI edit vs. AT NRPN/automation) → torn snapshot = the historical SIGBUS class. |
| IMPORTANT-2 | important | PluginProcessor.cpp:793–795, 535–536, 552–554 | loadMultiFile leaves part_select param stale → UI shows old part, edits go to Part 0; .parvati path does it right (HellcatPreset.cpp:680–684). |
| IMPORTANT-3 | important | PluginProcessor.cpp:1070–1104 | "replaceState fires no parameterChanged" is false (verified vs JUCE 9) → re-entrant onPartSelect mid-redirect can clobber legacy restores; currentPart_ tracking depends on the accident. |
| IMPORTANT-4 | important | SynthEngine.h:292–306 | readPendingConfig spins unbounded on the AT (comment claims bounded) → priority-inversion xrun window. |
| IMPROVEMENT-1 | improvement | SynthEngine.cpp:370–446; HellcatPreset.cpp:300–368; PluginProcessor.cpp:831–880 | Relaxed-atomic snapshot copies can tear mid-edit on ARM. |
| IMPROVEMENT-2 | improvement | HellcatPreset.cpp:455–517, 522–690, 696–711 | .parvati `version:` never checked on load. |
| IMPROVEMENT-3 | improvement | PluginProcessor.cpp:544–624; PluginProcessor.h:100–110 | Part switches flood the UndoManager (~250 undoable writes each). |
| IMPROVEMENT-5 | improvement | PluginEditor.cpp:2985–2995, 3021 | Unconditional partCombo_.repaint() every timer tick. |
| IMPROVEMENT-6 | improvement | SharedContainer.cpp/.mm; FactoryPresetInstaller.cpp:82–88 | Silent app-group fallback root-switch; per-process-only first-run lock (TEMPLATES sync transient gap). |
| IMPROVEMENT-7 | improvement | PluginProcessor.cpp:1070–1104 | setStateInformation message-thread contract is implicit; AUv3/worker-thread restores race editor state. |
| NOTE-1 | note | PluginProcessor.cpp:70–73, 178–186; PluginProcessor.h:286–296 | hostSampleRate_/lastReportedLatency_ benign cross-thread plain ints. |
| NOTE-2 | note | SynthEngine.cpp:456–459 | Forward version rejection of engine_state is silent (log it). |
| NOTE-3 | note | §1.2 | Parameter application is block-granular, not sample-accurate. |
| NOTE-4 | note | PluginProcessor.cpp:1046–1064 | loadedProgramName_ not persisted in host state. |
```

---

## Executive summary (≤40 lines)

- **Threading model (designed):** MT stages into atomics + release-store dirty flags; the AT is sole mutator of live render state, servicing everything at the top of `processTransport` (SynthEngine.cpp:1172–1300). Patch/part bytes are per-byte atomics; arp/seq rides an SPSC seqlock. This core is sound and unusually well documented.
- **Threading model (actual):** JUCE 9 delivers host automation **on the render thread** (AUv3 render events, VST3 process(); verified against the vendored wrappers), and the in-repo `midiParamMap_.handleBuffer` (PluginProcessor.cpp:187) invokes `parameterChanged` on the AT for every CC/NRPN-mapped param. The code assumes MT-only.
- **Critical:** automating `part_select` (an automatable AudioParameterChoice) executes `onPartSelect` → `loadPartIntoApvts` → ~250 `getParameterAsValue` ValueTree+UndoManager writes **on the render thread**, racing the APVTS 50 Hz flush timer and editor — RT violation plus UB-class data race (PluginProcessor.cpp:319–353/423–428/532–624).
- **Important:** (1) `pendingConfig_` seqlock has reachable dual writers (GUI arp/seq edit vs. AT NRPN addresses 119–123 / host automation) — the torn-snapshot class that already caused the TekDrums SIGBUS. (2) `loadMultiFile` never updates the `part_select` parameter (PluginProcessor.cpp:793–795) → combo shows the old part while edits route to Part 0 (the .parvati path does it correctly). (3) `setStateInformation`'s comment "replaceState fires no parameterChanged" is factually wrong vs JUCE 9 — re-entrant `onPartSelect` mid-redirect can clobber legacy restores, and `currentPart_` tracking silently depends on that accident. (4) `readPendingConfig` spins unbounded on the AT despite claiming bounded retries (SynthEngine.h:292–306).
- **Persistence:** engine-state blob v6 with strict version gating, length-prefixed forward-skip, and legacy fallback is well designed and well tested (host_state_test [1]–[5]); .PRO/.MUL are byte-exact and defensively parsed; .parvati ignores unknown keys (good) but never reads its own `version:` field.
- **SharedContainer:** correct app-group resolution + sandbox fallback; main risks are the silent root switch when entitlements change and the per-process-only first-run lock (benign for FACTORY, transient gap for TEMPLATES sync).
- **Timer/async storms:** the historical onChange ping-pong fix (PatchPage.cpp:321–334) holds; only remaining nit is an unconditional `partCombo_.repaint()` per timer tick.
- **Test gaps:** nothing drives `parameterChanged` from the audio thread (mt_harness.h:20–24 pins the sole-writer assumption), no part_select-automation test, legacy restore checked at one byte only, no part_select↔currentPart consistency assertion after loads, no UndoManager-across-part-switch test.
- **Recommended top fixes:** make part_select non-automatable + defer its handling off-thread; funnel AT-origin parameter writes back to the MT (or make the seqlock multi-writer-safe); write part_select in loadMultiFile; add a `restoringState_` guard around replaceState; bound readPendingConfig retries. Full concrete recommendations with file:line in the report.

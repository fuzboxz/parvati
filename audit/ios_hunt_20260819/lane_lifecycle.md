# Bug hunt 2026-08-19 — Lane: ios-lifecycle (AUv3/host lifecycle, memory, audio session)

Scope: Source/PluginProcessor.{h,cpp}, Source/ui/SharedContainer.{h,cpp,mm},
Source/SynthEngine.cpp prepare paths, Source/PluginEditor.cpp ctor/dtor + timers,
JUCE 9.0.1 AUv3 wrapper (~/JUCE/modules/juce_audio_plugin_client/juce_audio_plugin_client_AUv3.mm),
JUCE iOS audio session (~/JUCE/modules/juce_audio_devices/native/juce_Audio_ios.cpp).
Prior audit audit/IPAD_TOUCH_TODO.md read first (T1–T16 already fixed — not re-reported).
Every finding verified against the working tree at commit b889205.

## F-ios-lc-1: getStateInformation races the message thread on the UI-preference members (incl. two refcounted juce::Strings — UAF class)
- file: Source/PluginProcessor.cpp:1494-1500 (reads), Source/PluginProcessor.h:100/105 + :395-401 (members), writers Source/ui/SettingsPanel.cpp:39 (`proc_.setUiTheme (name)`, message thread) and Source/PluginEditor.cpp:3104 (`setUiLanguage`), :3738 (`setUiZoom`)
- severity: high (crash class; narrow window)
- class: lifecycle
- evidence: `getStateInformation` serializes `uiThemeName_` / `uiLanguage_` (juce::String — refcounted pointer + atomic refcount, NOT safe for concurrent read/write) plus `uiZoom_` (double) / `uiOversampling_` etc. with no synchronization. The AUv3 wrapper's `getFullState` (AUv3.mm:270-291) calls `getStateInformation` directly on whatever thread the host invokes the `fullState` selector — AUM/GB session saves and background autosaves do not funnel through the message thread, while the SettingsPanel theme combo / language combo write those exact members on the message thread. A theme switch concurrent with a host state save = torn String (UAF) or a torn double (garbage persisted zoom). The engine side of getState is fully atomic (`captureState` seqlocks/atomics, SynthEngine.cpp:462+); the UI-pref mirror is the one unsynchronized surface. Same asymmetry in `setStateInformation` (writes those members on the host thread; the editor reads them via `getUiTheme()` on the message thread).
- deterministic_check: TSan: `ParvatiAudioProcessor` + thread A looping `getStateInformation` into a MemoryBlock, thread B (message thread) looping `setUiTheme("Carbon"/"Slate")` + `setUiLanguage(...)` — TSan flags the String race within seconds. Post-fix (mutex- or atomic-guarded copies in both directions) the same harness runs clean; add it to tests/concurrency_test.cpp so the suite owns it.
- classification: REAL BUG

## F-ios-lc-2: No audio-session interruption handling — held notes survive render-resource teardown (stuck notes after phone call / route change)
- file: Source/PluginProcessor.h:36 (`void releaseResources() override {}` — empty), JUCE AUv3.mm:583-602 (`deallocateRenderResources` → `releaseResources()`, nothing else), grep across Source: no AVAudioSession observer anywhere in the plugin; `resetAllVoices()` only fires on patch/multi loads (PluginProcessor.cpp:901, :1030), never on prepare/release cycles
- severity: medium (visible malfunction: stuck sustain)
- class: lifecycle
- evidence: on interruption (call/Siri/route change) AUv3 hosts tear down render resources; on resume they re-allocate → `prepareToPlay` → `engine_.prepare` → `setCurrentPlaybackSampleRate(sameRate)` which JUCE only clears when the rate CHANGES (juce_Synthesiser.cpp:169-176 `approximatelyEqual` guard). Voices gated at interruption therefore resume gated; any note-offs lost during the interruption window are never re-delivered (hosts' panic behavior varies). The on-screen keyboard IS covered for interruptions that cancel the touch (JUCE maps `touchesCancelled` → mouse-up, juce_UIViewComponentPeer_ios.mm:918-923 → KeyboardView mouseUp → note-off), and `~ParvatiEditor` releases held keys (PluginEditor.cpp:3202-3207) — the uncovered class is host/hardware-held notes across a session interruption, plus editor-open-but-offscreen cases. Standalone is separately covered: JUCE's own iOS audio (juce_Audio_ios.cpp:166-186) observes interruption notifications — but that machinery belongs to juce_audio_devices (Standalone I/O), NOT to the AUv3 path.
- deterministic_check: headless pin of the chosen policy once decided: load a note, hold it, run `proc.releaseResources(); proc.prepareToPlay (48000, 256);` + one processBlock, assert the active-voice count — today this FAILS the intended "voices cleared on re-allocate" assertion (documents the gap); the fix (e.g. `engine_.resetAllVoices()` + `midiCollector_.reset()` when prepare follows a release, or on first block after reallocation) flips it green. Device confirmation still needed for real interruption timing.
- classification: REAL BUG (missing defense) / exact host behaviors UNKNOWN-NEEDS-DEVICE

## F-ios-lc-3: Process-global editor teardown side-effects are not reference-counted (screensaver, tap-assign)
- file: Source/PluginEditor.cpp:3169 (ctor `Desktop::setScreenSaverEnabled (false)`), :3230 (dtor → `true`), :3212 (`ParamControl::setTapAssignActive (false)` static reset); JUCE AUv3 wrapper creates/destroys one editor per instance in ONE extension process (AUv3.mm:915-918 `createEditorAndMakeActive`, :765-774 `removeEditor`)
- severity: medium
- class: lifecycle
- evidence: AUv3 extension processes host MULTIPLE plugin instances (AUM: several Parvati AUs → several editors, one process). Editor A closing re-enables the screensaver while editor B is still open (T14's protection silently voided); likewise closing A clears the process-global tap-assign flag mid-session for B. The zoom-global reset is correctly guarded on iOS (`setZoom(1.0)` forced, and the dtor reset only runs `if (zoom_ != 1.0)`) — the screensaver and tap-assign resets have no guard/count.
- deterministic_check: headless: two ParvatiEditors on one processor pair (or two processors), destroy only the first, assert `Desktop::getInstance().isScreenSaverEnabled() == false` (pre-fix: true). Same shape for `ParamControl::isTapAssignActive()` after arming B. Post-fix (a process-wide editor-instance counter consulted by ctor/dtor) both stay asserted.
- classification: REAL BUG

## F-ios-lc-4: Presets saved inside AUv3 hosts never reach the Files app (mirror targets the extension's private Documents)
- file: Source/PluginEditor.cpp:4227-4236 (`mirrorUserSaveToDocumentsIOS` → `juce::File::userDocumentsDirectory`), JUCE juce_Files_mac.mm:184 (iOS `userDocumentsDirectory` == `NSDocumentDirectory`)
- severity: medium (product gap, documented in-tree)
- class: files
- evidence: inside the AUv3 extension process `NSDocumentDirectory` resolves to the extension's OWN PluginKitPlugin container — which the Files app does not browse (UIFileSharingEnabled exposes the CONTAINING app's Documents only). So the T6 mirror works solely for saves made in the Standalone app; the primary iOS workflow (saving a patch from inside AUM/GarageBand) leaves nothing user-visible in Files. The in-tree comment at :4226 acknowledges exactly this ("Documents is the EXTENSION's sandbox, which Files does not browse — the copy is harmless there"). Secondary: `copyFileTo` (not TemporaryFile) means a kill mid-mirror leaves a torn file in Documents — parsed loads of a torn file fail gracefully (chunk-size validation), so that half is low.
- deterministic_check: code-level (no device needed): assert the mirror destination's container differs per wrapper — e.g. a unit test that runs the helper under `wrapperType_AudioUnitv3` and checks the target is NOT under the app-group root and NOT under the app's Documents is impossible headlessly; the practical deterministic check is the REMEDIATION's: (a) containing-app launch-sync of `Parvati/USER` from the shared container into its Documents (testable headlessly against temp dirs), or (b) direct `UIDocumentPicker` export from the extension. Device: save from AUM, open Files.
- classification: REAL GAP (documented limitation) — needs a product decision between (a)/(b)

## F-ios-lc-5: App-group fallback is per-process — Standalone and AUv3 silently use SEPARATE trees when the entitlement is absent
- file: Source/ui/SharedContainer.cpp:13-26 (fallback to `userApplicationDataDirectory`), SharedContainer.mm:14-25 (`containerURLForSecurityApplicationGroupIdentifier` → nil when unentitled), JUCE juce_Files_mac.mm:210 (iOS `userApplicationDataDirectory` == `~/Library` of the CURRENT sandbox)
- severity: low
- class: files
- evidence: with the group entitlement missing (unsigned simulator builds, a mis-signed TestFlight), the AUv3 extension and the Standalone app each fall back to their OWN `Library/Parvati/...` trees: user presets saved in one are invisible to the other, with zero user-visible signal. The split is static per install (no mid-session flip — `containerURLFor...` is deterministic), so no corruption, only confusion. Both processes' first-run extractions and the TEMPLATES sync never meet (atomic TemporaryFile writes would make concurrent first-run extraction safe anyway — verified PatchFile.cpp:221/368 and FactoryPresetInstaller.cpp:19-36/40-56 all use TemporaryFile).
- deterministic_check: headless: stub/compile-time force the app-group resolver to return empty, construct the roots in two simulated "sandboxes" (two temp dirs), save a preset in one, assert it is not visible to the other — then assert a one-line startup diagnostic (the proposed remediation: log + a Settings banner when the fallback is active) fires, so the state is at least observable.
- classification: BY-DESIGN fallback, silent-failure UX is the gap

## F-ios-lc-6: Peak-memory profile per instance (informational, no bug at single-instance scale)
- file: Source/SynthEngine.cpp:11-13 (96 × `new AmbikaVoice()` at construction), Source/dsp/fx/FxProcessors.h:61/80/106/151-157/188-200 (inline FX buffers: Diffuser float[2048]=8 KB, PitchShifter u16[4096]=8 KB, Reverb u16[16384]=32 KB, LoopingDelay i16[2][128008]=512 KB, Granular i16[2][128008]=512 KB per PROCESSOR OBJECT), BinaryData rodata ~1.4 MB (369 files, clean file-backed pages)
- severity: low (capacity note)
- class: perf
- evidence: static estimate per AUv3 instance: 96 voices ≈ 0.5-1 MB heap (+ per-voice Oversampling when staged, ~2-3 KB each ≈ 200 KB at default 2x), worst-case FX ≈ 6.3 MB (all six parts × LoopingDelay+Granular+Reverb), editor component tree ≈ 2-5 MB, engine scratch ≈ 1 MB → ~10-15 MB typical, ~25 MB worst. AUv3 extension memory is per-PROCESS and shared by all instances in a host: 8 Parvati AUs in AUM ≈ 100-200 MB — inside modern extension budgets but worth knowing; no low-memory (`memoryWarning`) hook exists to drop the preset-menu cache / undo history under pressure.
- deterministic_check: none needed for the numbers (static); for the pressure path, a unit test that calls a new `handleMemoryPressure()` seam and asserts the PresetBrowser cache + UndoManager history are released.
- classification: BY-DESIGN (informational); pressure hook = polish

## Verified good (no finding — evidence recorded for the acceptance review)
- AUv3 editor churn (AUM open/close loops): `removeEditor` (AUv3.mm:765-774) deletes the editor on the message thread under the callback lock; `~ParvatiEditor` (PluginEditor.cpp:3189-3252) stops the 30 Hz timer, dismisses active popup menus (W11), releases on-screen-keyboard notes, nulls callbacks, resets process-globals, detaches theme listener and L&F BEFORE member teardown. `paramControlRegistry()` (PluginEditor.cpp:151-155) erases every control in its dtor (:530-534) — no accumulation across open/close cycles.
- The W11 AsyncUpdater deferrals (ParamControl/NoteStepControl) are safe against editor death: `~AsyncUpdater` clears the deliver flag (juce_AsyncUpdater.cpp:61-66) and editors die on the message thread; the audio-thread `triggerAsyncUpdate` path only posts a pre-allocated message (no allocation, MessageManager exists in the extension).
- `restoringState_` guard (PluginProcessor.cpp:1539-1545, RAII RestoreGuard) covers replaceState's synchronous re-entrant parameterChanged dispatch; `loadingPartIntoApvts_` symmetric. The engine-blob restore path is atomic/seqlocked end-to-end (`captureState` SynthEngine.cpp:462+, `restoreState` with the W11 staging fixes).
- Prepare/release cycles: FxChain re-prepares with the new maxBlock (audit F3), per-voice Oversampling is built for the constant kAudioBlockSize=40 so host block changes can't under-size it, the oversized-host-block clamp (PluginProcessor.cpp:355-365) protects every prepare-sized buffer, DC blockers re-prepared + reset per prepare, `midiCollector_.reset(sampleRate)` re-timestamps. Sample-rate change → `setCurrentPlaybackSampleRate` clears all notes under the Synthesiser lock (correct continuity semantics otherwise).
- Factory extraction: process-once (`std::call_once` + atomic, FactoryPresetInstaller.cpp:64-68), all writes atomic (TemporaryFile), two processes racing a first-run extraction converge on identical content; the per-start cost after install is one stat per resource + one read+memcmp per template.
- `getStateInformation`'s engine half is atomic-snapshot based; `apvts.copyState()` is JUCE-locked. `MidiParameterMap` scans on the audio thread without allocation (pre-audited). Save paths (.PRO/.MUL/.parvati) are all TemporaryFile-atomic — no torn shared-container files even under extension kills.
- JUCE AUv3 constructs the AudioProcessor on the message thread (AUv3.mm:150 `jassert ... isThisTheMessageThread`), so the ctor-time factory I/O cannot run on a host background instantiation thread.
- Standalone background audio: `BACKGROUND_AUDIO_ENABLED TRUE` (CMakeLists.txt:82) → `UIBackgroundModes:audio`; the T14 screensaver disable + iOS zoom=1.0 default are in place (the multi-instance hole is F-ios-lc-3, not an omission).

# Count summary
6 findings: 1 high (F-ios-lc-1), 3 medium (F-ios-lc-2, F-ios-lc-3, F-ios-lc-4), 2 low (F-ios-lc-5, F-ios-lc-6).
Classifications: 3 REAL BUG (1,2,3), 1 REAL GAP/documented limitation (4), 1 BY-DESIGN-with-silent-failure (5), 1 informational (6). Device-dependent confirmation noted where it applies (2, 4).

# TOP 3 remediation priorities
1. F-ios-lc-1 — guard the UI-preference members (theme/language Strings + scalars) with a small mutex or atomic copies in BOTH getStateInformation/setStateInformation and the setters; add the two-thread TSan harness to tests/concurrency_test.cpp. Crash-class, fully deterministic to fix and to test.
2. F-ios-lc-2 — add interruption semantics: on `prepareToPlay` following a `releaseResources` (or first block after reallocation) call `engine_.resetAllVoices()` + `midiCollector_.reset(hostSampleRate_)` and decide (and pin in a test) the held-note policy; verifies against a headless release/prepare cycle.
3. F-ios-lc-3 — reference-count the process-global editor side-effects (screensaver, tap-assign) behind a live-editor count so N>1 instances and N→1 close sequences keep the intended state; headless two-editor test.
(F-ios-lc-4 needs a product decision between containing-app launch-sync vs document-picker export; F-ios-lc-5/-6 are polish.)

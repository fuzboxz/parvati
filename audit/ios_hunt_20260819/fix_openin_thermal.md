# iOS hunt 2026-08-19 — fixes (keys: open-in + thermal-label)

The open-in worker's code landed complete but its run timed out during the
final simulator-build validation; the remaining compile fixes, validation,
and this report were finished by the orchestrator.

## open-in — completing the "Open in Parvati" loop
(Files: Source/ui/IosOpenIn.{h,mm} NEW, PluginProcessor.cpp ctor hook,
CMakeLists iOS-only target_sources + test target, tests/ios_openin_test.cpp NEW.)

The document types/UTIs grafted earlier (ios/parvati_filetypes.plist) make iOS
OFFER "Open in Parvati" for .parvati/.PRO/.MUL/.scl/.kbm — but JUCE 9's iOS
glue has NO application:openURL:options: implementation anywhere (verified:
no openURL handler in juce_Windowing_ios.mm / juce_MessageManager_ios.mm /
juce_audio_plugin_client; the only openURL in ~/JUCE SENDS one). The file
event was dropped: the app launched and nothing happened.

- **Pure core (header-inline, every platform):** `openInKindForFile`
  (extension -> Preset/Tuning/None table) + `routeOpenedFile(source, userDir)`
  — presets import into the shared USER tree via the existing atomic
  `PresetBrowser::importIntoUserTree` (collision-overwrite); .scl/.kbm park in
  the Parvati/Tuning sibling (TuningEditor has no fixed dir; interactive
  import by design — no headless tuning apply); non-documents route INVALID.
- **Obj-C++ shim (IosOpenIn.mm, iOS-only target_sources like SharedContainer.mm
  — the Source/ glob covers .cpp/.cc/.h only):** resolves JUCE's
  file-private `JuceAppStartupDelegate` BY NAME at runtime and adds the absent
  selector via class_addMethod (category-style; a swizzle branch future-proofs
  against a JUCE that implements it). The handler brackets
  start/stopAccessingSecurityScopedResource (open-in-place scoped URLs) and
  delivers on the main thread (UIKit delegate delivery).
- **Processor hook (ctor tail, JUCE_IOS-gated, Standalone only):** presets
  route then load through the same main-thread seams the editor's FileChooser
  completions use (.parvati->loadParvatiMultiFile, .PRO->loadProgramFile,
  .MUL->loadMultiFile). AUv3 extensions never receive openURL events.

Three iOS-toolchain compile errors found during the worker's simulator build
and fixed:
1. wave-B thermal sampler: its `#if JUCE_IOS` objc includes sat BEFORE the
   first JUCE header, so JUCE_IOS was undefined there — the block silently
   compiled out on iOS too (desktop-only validation gap). Includes moved
   after PluginProcessor.h with a warning comment.
2. JUCE 9 has no `getWrapperType()` METHOD — it is a public `wrapperType`
   MEMBER (juce_AudioProcessor.h:1352).
3. App-level Obj-C++ does not inherit UIKit from juce_gui_basics.h (JUCE's
   iOS headers are module-internal) — explicit `#import <UIKit/UIKit.h>` /
   `<Foundation/Foundation.h>` (the SharedContainer.mm pattern).

Tests (parvati_ios_openin_test, all green): routing table, preset import
bytes/no-temp/overwrite/idempotence, .txt rejection, tuning parking, empty-dir
guard. **Wiring proof:** full Parvati_Standalone + .appex build under the iOS
simulator toolchain, rc=0, zero errors, codesign-clean.

## thermal-label — the documented 3-line follow-up (F-ios-perf-2 rider)
(Worker-completed; mutation-verified.)

`ParvatiEditor::thermalStatusForTransition` — a public pure 3x3 transition
matrix (None/Hint/StrongHint -> NoOp/Show/Clear, escalation-only posts,
de-escalation clears) — surfaced in the editor's 30 Hz timer under
`#if JUCE_IOS` (one relaxed atomic read per tick; TRANS'd posts through
`ParamControl::postTransientStatus`; de-escalation relies on the transient's
frame expiry — the seam exposes no clear API, documented in code). Pinned by
lifecycle_test [5]: the full matrix, mutation-verified red/green (the worker's
first mutation was vacuous — `n == o` unreachable inside `if (n > o)` — red
was observed, not assumed).

## Validation
- Full release suite: 69 PASS / 0 real failures (settings_roundtrip = arg-taking harness).
- iOS simulator (build_ios_openin): Parvati_Standalone + AUv3 .appex rc=0,
  zero errors — the first full iOS-toolchain compile of the processor with
  all of wave B + this wave's changes.
- Desktop unaffected: Parvati + new/existing tests green.

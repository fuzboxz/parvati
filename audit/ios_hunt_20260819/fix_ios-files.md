# iOS hunt 2026-08-19 — fixes (key: ios-files)

Status: ALL 4 fixes implemented, deterministic tests added and verified
(pre-fix FAIL → post-fix PASS proven for every observable behavior), no
regressions in the editor/preset/multi-load suites.

## FIX 1 — F-ios-files-1 (HIGH): picker saves never reached the USER tree (import-on-load)

The iOS FileChooser can only write into document-provider locations (On My
iPad / iCloud / third-party); the shared App-Group USER tree the PresetBrowser
scans is not part of any provider tree — a saved preset NEVER appeared in the
browser. Remediation:

- `PresetBrowser::importIntoUserTree (file, userDir)` (NEW static, public):
  atomic import (TemporaryFile + rename, house pattern) of an
  outside-the-tree .PRO/.MUL/.parvati into USER/<same filename>; collision
  OVERWRITES (the user just picked the file); guards return an invalid File
  for files already inside the tree / non-preset extensions / missing source.
  Lives on PresetBrowser (NOT ParvatiEditor) because PluginEditor.h is outside
  this key's file ownership (sibling agents are editing it) — the helper is
  the exact code the iOS glue calls, driven directly by the test.
- `ParvatiEditor::applyPatchFile` (PluginEditor.cpp, success tail): on
  JUCE_IOS, after a successful load of a file OUTSIDE the USER tree, import
  via the helper, `mirrorUserSaveToDocumentsIOS (imported)` (FIX 1c — Files
  visibility for picker-location saves) and `presetBrowser_->invalidate()`.
  Desktop is byte-identical (call is `#if JUCE_IOS`-gated).
- DEVIATION from the task text: the helper is
  `PresetBrowser::importIntoUserTree` instead of
  `ParvatiEditor::importIntoUserTreeForTest` — same standalone-static
  testability, different host class, to respect file ownership. The
  invalidate() side stays in the editor (it owns presetBrowser_).

## FIX 1b — F-ios-files-5: the Documents mirror is now ATOMIC

`mirrorUserSaveToDocumentsIOS` used `copyFileTo` straight onto the
Files-visible destination — an iOS suspension mid-copy left a TORN
.parvati/.PRO/.MUL in Documents. Now TemporaryFile + rename: the visible file
appears only complete; an interrupted copy leaves the previous mirror intact.

## FIX 2 — F-ios-files-4: sweep no longer deletes another process's in-flight temp

The stale-template sweep deleted any local `*.parvati` absent from the
embedded set — including `<name>_temp<hex>.parvati`, JUCE's TemporaryFile
shape LIVING IN THE TARGET DIRECTORY. Standalone + AUv3 sweep the SAME group
tree, so process B's sweep could delete process A's mid-rename write. The
sweep now skips names containing `_temp` (they never match an embedded name;
a settled tree is unaffected).

## FIX 3 — F-ios-perf-5: version-marker fast path

`constexpr int kFactoryInstallVersion` (header; bump on embedded-content
changes). A completed FULL pass atomically writes
`FACTORY/.factory-install` = `installed=<n>`. At startup, a matching marker
skips the ~365 per-resource existence stats (one marker read instead — the
walk was measurable inside iOS AUv3 instantiate) and ALSO the legacy-dir
cleanup (a marker implies a version that already ran it). The TEMPLATES
content-sync + stale sweep still run on the fast path (correctness-relevant).
Missing/corrupt/version-mismatched marker → full self-healing pass, exactly
as before. RESIDUAL (documented in-code): with a VALID marker, a
user-deleted factory .PRO/.MUL is not re-extracted until the version bumps.
`resetInstallOnceForTest()` added (test-only; swaps in a fresh once_flag —
std::once_flag itself is non-movable, hence the unique_ptr).

## FIX 4 — F-ios-perf-4: PresetBrowser scan bounds

`scanRecursiveInto` had NO depth bound, entry cap or symlink refusal — a
large/linked USER tree stalled the message thread (measured 3.8-5.1 s).
Added (all one-line-commented in-code):
- depth cap 8 (`kScanMaxDepth`): stop descending silently;
- per-directory entry cap 512 (`kScanMaxEntriesPerDir`): first 512 SORTED
  entries only (deterministic which);
- symlink refusal: `isSymbolicLink()` entries skipped (cycles / provider
  aliases). Also added `debugTreeLeafCount()` (test observable: a followed
  directory link would double every leaf under it).

## Test: tests/ios_file_flow_test.cpp (target parvati_ios_file_flow_test)

34 checks. Pre-fix FAIL proven by temporarily disabling the fixes (sweep
skip / caps / symlink skip; no git commands used): FAIL x5 — [2] decoy
deleted (x2), [4] depth, [4] entry cap, [4] symlink. Post-fix: ALL PASS.
[1]/[3] exercise NEW behavior (the API/marker did not exist pre-fix).

Notes:
- [4]-depth first used a 200-level chain — PATH_MAX (1024) truncation made
  the walk stop at ~level 170 OS-side, so the check passed VACUOUSLY even
  with the cap disabled; the test now uses a 20-level chain of short names
  (well within PATH_MAX, still past the cap). The depth check is real.
- [1] pins the invalidate seam's observable end-to-end: import → USER mtime
  moves → next buildMenu rescans (scanCount 2) → leaf present.
- Marker stat-count precise assertion skipped (nice-to-have per task); the
  behavioral fast-path contract is pinned instead (templates still synced,
  banks untouched, corrupt marker self-heals).
- The editor glue + mirror are JUCE_IOS-gated → not compiled on macOS; the
  SHARED import helper is what the test drives (the exact code the iOS path
  calls). The mirror's atomic mechanism is identical to the tested import.

## Validation

- Own dir build_ios_fx_iosfiles (Release): builds clean (only pre-existing
  warnings in unrelated files).
- build_release (after pgrep coordination — no other agent building):
  parvati_ios_file_flow_test / parvati_editor_test / parvati_preset_test /
  parvati_multi_load_test ALL PASS.
- editor_test [17] (PresetBrowser cache contract) unaffected by the bounds.

## Residual risks / follow-ups

- iOS-device confirmation still needed for: the picker-save → menu-appearance
  flow end-to-end (the headless pin covers the shared helper + browser
  observables), and the AUv3-side Documents visibility (extension sandbox).
- F-ios-lc-4's reverse sync (group→Documents at containing-app launch) is a
  SEPARATE finding (not this key); the import-on-load here closes the
  load-direction gap only.
- Marker residual: deleted factory bank files not re-healed until
  kFactoryInstallVersion bumps (documented in-code).

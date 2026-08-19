# iOS hunt 2026-08-19 — Lane: ios-files (file access, Files app, presets on iOS)

Repo state: HEAD = b889205 (post-W11). JUCE 9.0.1 at ~/JUCE.
Prior audit absorbed: T1–T6 (T6 = the Documents MIRROR design) already fixed — not re-reported.
Method: full source trace of every file-root decision + the JUCE iOS FileChooser
implementation (~/JUCE/modules/juce_gui_basics/native/juce_FileChooser_ios.mm,
juce_TemporaryFile.cpp) + live plist inspection + a UTType case-sensitivity probe.

## Container map (verified, the baseline the findings build on)

| Path | Resolver | Standalone process | AUv3-extension process | Files app |
|---|---|---|---|---|
| FACTORY / FACTORY_MULTI / TEMPLATES / USER | `parvati::getSharedContainerRoot()` (PluginProcessor.cpp:1005,1114,1123,1131) | app-group `group.com.805labs.parvati` | SAME (both entitlements declare it — parvati.entitlements / parvati-auv3.entitlements) | NOT browsable |
| Documents/Parvati/USER (mirror) | `userDocumentsDirectory` (PluginEditor.cpp:4232) | app sandbox Documents | EXTENSION's own sandbox (inert there, documented) | Browsable ("On My iPad/Parvati") |
| Save-picker destinations | user-chosen (UIDocumentPicker export) | Documents / iCloud / providers | same | — |

The four engine roots correctly share ONE tree across both processes; entitlements
match; all save writers are atomic (TemporaryFile in the TARGET's own directory →
same-container POSIX rename — verified JUCE places temps in the target dir,
juce_TemporaryFile.cpp:73-77). The problems are all in the PICKER↔GROUP-TREE gap.

---

## F-ios-files-1: iOS save picker can never write into the preset browser's USER tree — saved presets never appear in the browser; the T6 mirror is dead code on iOS
- file: Source/PluginEditor.cpp:4288 (also :4328, :4370 — the three save dialogs), :4227-4237 (mirrorUserSaveToDocumentsIOS), :4437+ (applyPatchFile); Source/PluginProcessor.cpp:1127-1133 (getUserPatchDir); ~/JUCE/modules/juce_gui_basics/native/juce_FileChooser_ios.mm:247-282 (save-mode controller construction)
- severity: high (core workflow: save → find it in the preset menu fails on iOS)
- class: files
- evidence: The three save dialogs default to `getUserPatchDir()` = the APP-GROUP container. On iOS, JUCE's save-mode FileChooser builds `initForExportingURLs:@[url]` over that default: because the default file does not exist yet, JUCE stages an EMPTY temp file (juce_FileChooser_ios.mm:251-267) and the UIDocumentPickerViewController export session lets the user choose a destination **among the document-provider tree only** (On My iPad = sandbox Documents, iCloud, third-party providers). **The app-group container is not part of any file provider tree** — it cannot be picked. The picked destination comes back in `didPickDocumentsAtURLs` and Parvati then writes the real bytes there (saveProgramFile / saveParvatiPatchFile / saveMultiFile). Therefore:
  1. Every picker-driven save lands OUTSIDE the group tree (typically Documents, first run even without a Parvati/USER folder existing — the mirror is what creates it, chicken-and-egg).
  2. `mirrorUserSaveToDocumentsIOS` fires only when `saved.isAChildOf (userDir)` (PluginEditor.cpp:4229-4230) — no picker-reachable path is a child of the app-group dir on iOS, so the mirror NEVER runs on iOS (it only mirrors saves into the group tree, which the picker cannot produce; there are no non-picker save paths into USER — verified: the only getUserPatchDir writers are the three dialogs + the factory installer).
  3. Nothing imports back: `applyPatchFile` (PluginEditor.cpp:4437+) loads a picked file into the ENGINE only; no copy into the group USER tree; PresetBrowser (PluginEditor.cpp:2368-2371) scans the group tree exclusively.
  Net: on iOS a user who saves a patch and then opens the preset menu does not find it — ever. The desktop T6 mirror rationale ("group tree is the single source of truth") silently inverted on iOS: the DOCUMENTS copy becomes the only copy and the group tree never sees it. AUv3-in-host saves have the same gap plus an extension-sandbox twist.
- deterministic_check: (a) Simulator UI script: Standalone → Save Parvati → pick "On My iPad/Parvati/USER/x.parvati" → assert `getUserPatchDir().getChildFile("x.parvati")` exists → FAILS today. (b) Headless contract pin (no device needed): unit-test that any file path reachable from a UIDocumentPicker destination is never `isAChildOf(getUserPatchDir())` on iOS, then assert the compensating import exists — i.e. after remediation (import-on-load or reverse-sync), a headless test that saves to a Documents stand-in and asserts the group tree + PresetBrowser see it.
- remediation direction (for the parent to schedule): import-on-load (after a successful picker load, copy the file into group USER and invalidate the browser) AND/OR reverse-sync at browser-open (Documents/Parvati/USER → group USER, newest-mtime wins), keeping the mirror for the Files-app visibility story.

## F-ios-files-2: No CFBundleDocumentTypes / UTExportedTypeDeclarations — .parvati/.PRO/.MUL cannot be opened from Files, Mail, or AirDrop; LSSupportsOpeningDocumentsInPlace is inert
- file: CMakeLists.txt:74-85 (PARVATI_IOS_PLIST_ARGS — no doc-type keys exist in any seam); verified built plist: `plutil -p build_ios_sim/.../Parvati_Standalone/Info.plist` shows only `LSSupportsOpeningDocumentsInPlace=true`, `UIFileSharingEnabled=true`, no Document/UTExported keys; repo-wide grep for CFBundleDocumentTypes/UTExportedTypeDeclarations/LSItemContentTypes → none
- severity: medium (visible capability gap: "send a patch to another iPad user" has no in-app path; also the app's own extension is unowned system-wide)
- class: plist
- evidence: With `UIFileSharingEnabled` the Files app SHOWS the mirrored copies, but without declared document types iOS offers no "Open in Parvati"/"Copy to Parvati" for .parvati/.PRO/.MUL received via Mail/AirDrop/Files, and .parvati has no exported UTI (other apps see generic data; the LOAD picker still works because JUCE builds dynamic UTIs from the extension — verified below). Classification: REAL GAP for the share/receive workflow (the send side exists via Files visibility), not a regression.
- deterministic_check: after adding the keys (via the existing PlistBuddy patch block — juce_add_plugin has no doc-type argument), `plutil -p` the built Standalone plist and assert `CFBundleDocumentTypes[0].LSItemContentTypes` contains the exported `com.805labs.parvati.patch` UTI; device: tap a .parvati in Files → Parvati offered.

## F-ios-files-3: Factory bank content updates never propagate to an installed tree (write-if-missing only) — BY-DESIGN today, but an upgrade-policy gap
- file: Source/ui/FactoryPresetInstaller.cpp:22-36 (writeIfMissing), :107-111 (comment "Factory .PRO/.MUL banks are stable -> write-if-missing only")
- severity: low-medium (only bites when a shipped factory file needs a content fix)
- class: files
- evidence: writeIfMissing skips any existing file byte-for-byte; only TEMPLATES are content-synced (`overwriteIfChanged`). If a factory .PRO ships with a bug and is fixed in a later build, every existing install keeps the old copy forever (the app-group tree outlives app updates). The code comment declares this deliberate ("banks are stable"), so BY-DESIGN — flagged as a product decision to revisit (a version-stamp file or overwriteIfChanged-for-banks would close it).
- deterministic_check: unit test calling ensureFactoryPresetsInstalled against a dir pre-populated with a MODIFIED copy of one factory resource, then asserting the embedded content won → fails today (documents the policy); after any policy change, flip the assertion.

## F-ios-files-4: Cross-process template sweep can delete another process's in-flight atomic-write temp file
- file: Source/ui/FactoryPresetInstaller.cpp:169-174 (stale-template sweep `findChildFiles(..., "*.parvati")` + delete); JUCE temp naming verified at ~/JUCE/modules/juce_core/files/juce_TemporaryFile.cpp:73-77: temps are `<name>_temp<hex>.parvati` IN THE TARGET DIRECTORY
- severity: low (self-healing next run; both processes ship identical embedded content from the same bundle)
- class: files
- evidence: Standalone and AUv3 each run the installer once per PROCESS (per-process once_flag) against the SAME group tree. If process B's sweep runs while process A is between temp-write and rename, B lists `Poly_temp3f9a.parvati`, finds it absent from the embedded set, and deletes it → A's `overwriteTargetFileWithTemporary` retries 5×100 ms then fails → that template write is lost for A's run (B wrote identical content itself, so disk ends correct; an interrupted B would self-heal next launch). Real race, low impact, one-line hardening.
- deterministic_check: headless unit test: pre-create `TEMPLATES/Poly_tempdeadbeef.parvati` (the documented JUCE temp shape), run ensureFactoryPresetsInstalled, assert the decoy still exists → FAILS today; after the fix (sweep skips `*_temp*.parvati`), passes.

## F-ios-files-5: mirrorUserSaveToDocumentsIOS copy is non-atomic — a suspension mid-copy leaves a torn file visible in the Files app
- file: Source/PluginEditor.cpp:4231-4237 (`dest.getParentDirectory().createDirectory(); saved.copyFileTo (dest);`)
- severity: low (mirror is an export copy; group tree authoritative; self-heals only on the NEXT save of the SAME preset)
- class: files
- evidence: `copyFileTo` streams directly onto the visible destination; iOS can suspend the app mid-copy (backgrounding), leaving a truncated .parvati/.PRO in Documents that the user can see and open. Every other writer in the repo uses TemporaryFile+rename (PatchFile.cpp:221,368; PluginProcessor.cpp:1323,1377; FactoryPresetInstaller.cpp:28,54) — the mirror is the lone non-atomic writer. (Currently unreachable on iOS per F-ios-files-1, but it is the designated T6 mechanism and becomes live again the moment F1 is remediated — fix together.)
- deterministic_check: code-level pin: grep/unit-assert that every writer of user-visible preset files uses TemporaryFile (a small static check in tools/, mirroring check_combo_clear.py's pattern); functional: make the destination read-only and assert no partial file is left (copyFileTo today leaves none only because it fails fast — the torn case needs the suspension window, hence the static pin).

## F-ios-files-6: iOS-native save picker ignores `warnAboutOverwriting` (and the load picker shows no "Parvati" root shortcut) — UNKNOWN-NEEDS-DEVICE, UX-only
- file: Source/PluginEditor.cpp:4293-4296 (warnAboutOverwriting flag); juce_FileChooser_ios.mm:284+ (flags consumed only for mode/selection — no overwrite prompt on iOS)
- severity: low
- class: files
- evidence: The overwrite warning is a desktop-FileChooser feature; the iOS export sheet's collision behavior is provider-dependent (iCloud prompts, local "On My iPad" may silently replace). No data-loss risk beyond what the user picked. Already adjacent to the audit's deferred "AUv3 FilePicker behavior per host" item — fold into that device pass.
- deterministic_check: device/simulator manual script (AUM + GarageBand + Files): save same name twice, observe provider behavior; no headless assertion possible.

---

## Verified NOT bugs (checked, cleared — recorded so nobody re-hunts them)

- **Extension case-sensitivity on the load picker**: dynamic UTIs are case-INSENSITIVE — live probe: `UTType(filenameExtension:"PRO")` and `UTType(filenameExtension:"pro")` both resolve to `dyn.ah62d4rv4ge81a6xt`, equal. A lowercase `.pro`/`.mul` file IS selectable. (Probe: /tmp/uti_probe.swift, swift run.)
- **Cross-container rename failure**: every atomic writer stages its TemporaryFile in the TARGET's own directory (JUCE constructs it from `target.getParentDirectory()`), so `overwriteTargetFileWithTemporary` renames within one container — no EXDEV-class failure exists anywhere in the preset paths.
- **App-group divergence between Standalone and AUv3**: both entitlements declare `group.com.805labs.parvati`; all four roots route through `getSharedContainerRoot()`; the only other `getSpecialLocation` uses are the (intentional) Documents mirror (PluginEditor.cpp:4232) and the scala-import picker start dir (TuningEditor.cpp:174).
- **Concurrent first-run double extraction** (Standalone + AUv3 extracting the same factory): writeIfMissing check-then-write races, but both writers emit byte-identical content via atomic same-dir rename — last-writer-wins is benign. (The template-sweep sub-case that ISN'T benign is F-ios-files-4.)
- **Externally-deleted browser targets**: Files cannot reach the group tree, so PresetBrowser entries cannot be deleted out from under a running session; picker loads are coordinated reads (JUCE NSFileCoordinator reading intent). The W10 mtime self-heal additionally covers any in-app mutation.

## Count summary

6 findings: 1 high (F-ios-files-1), 2 medium (F-ios-files-2; F-ios-files-3 low-medium BY-DESIGN), 3 low (F-ios-files-4, F-ios-files-5, F-ios-files-6 need-device/UX). Plus 5 verified-clear areas.

## TOP 3 remediation priorities

1. **F-ios-files-1** — close the picker↔group-tree gap (import-on-load and/or Documents→group reverse-sync) so iOS saves become browsable; fold F-ios-files-5's atomicity into the same change.
2. **F-ios-files-2** — declare CFBundleDocumentTypes + export the `.parvati` UTI through the existing PlistBuddy patch block; `plutil` assertion in the build.
3. **F-ios-files-4** — one-line temp-skip in the template sweep + the decoy-temp unit pin (cheap, removes the only real cross-process race).

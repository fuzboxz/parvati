# Lane: ios-build — build system, plists, entitlements, bundle structure

Parent hunt: iOS quality sweep, 2026-08-19 (lane ran 01:08–01:40). Repo at
commit b889205. Own build dir: `build_ios_bh_build` (nothing under
build_release/build_san_*/build_ios* was touched). No Source/ or tests/ edits.

## Environment + verified baseline (all commands recorded below)

- Xcode 26.6 (17F113), SDK iphonesimulator 26.5, deployment target 14.0,
  CMAKE_SYSTEM_NAME=iOS, Xcode generator, no signing team (simulator).
- `cmake --build build_ios_bh_build --config Release --target Parvati_Standalone
  Parvati_AUv3 -- -sdk iphonesimulator` => **BUILD SUCCEEDED** (Release).
- Binaries: Mach-O arm64 simulator, minos 14.0, `codesign -v` clean on both
  bundles; appex embedded + signed at `Parvati.app/PlugIns/Parvati.appex`.
- Standalone plist (Release, plutil -p): CFBundleIdentifier
  com.805labs.parvati; UIDeviceFamily [2]; UIRequiresFullScreen true;
  UISupportedInterfaceOrientations = Landscape L/R (no ~ipad key — base key
  applies for a family-2-only app, valid); UIBackgroundModes [audio];
  UIFileSharingEnabled true; LSSupportsOpeningDocumentsInPlace true;
  UILaunchStoryboardName LaunchScreen; MinimumOSVersion 14.0 == target.
- Appex plist (Release): CFBundleIdentifier **com.805labs.parvati.AUv3**
  (T1's PlistBuddy patch verified working in a real Release build; matches
  PRODUCT_BUNDLE_IDENTIFIER and the entitlements application-identifier child).
  NSExtension AudioComponents: aumu / 805L / Prvt / sandboxSafe / version 256.
- Entitlements (read from the binaries' `__TEXT,__entitlements` sections —
  NOT from `codesign -d`, whose DER view is empty for ad-hoc simulator
  signatures and initially misled me):
  - app:  application-identifier LHXWDDUUAJ.com.805labs.parvati,
          application-groups [group.com.805labs.parvati]
  - appex: application-identifier LHXWDDUUAJ.com.805labs.parvati.AUv3,
           application-groups [group.com.805labs.parvati]
  => App-Group sharing between Standalone and AUv3 is real even in the
  unsigned simulator build. SharedContainer.mm's nil-fallback is a safety net,
  not the expected path. (Xcode also wrote matching
  DerivedSources/Entitlements-Simulated.plist files.)
- No `@available`/`#available`/UIScene/UIDevice API drift in Source/ — the
  iOS seams (zoom=1.0, screen-saver, edge-aware safe-area, mirrors) are all
  JUCE-level or iOS-7-era Foundation; nothing requires > iOS 14.

## F-ios-build-1: No CFBundleDocumentTypes / UTI declarations — Files-import dead end
- build_ios_bh_build/Parvati_artefacts/Release/Standalone/Parvati.app/Info.plist (built artifact); cause is the whole plist pipeline: CMakeLists.txt:73-77 (PARVATI_IOS_PLIST_ARGS — no DOCUMENT_TYPES seam exists in juce_add_plugin), CMakeLists.txt:356-408 (PlistBuddy patch block — only patches LSSupportsOpeningDocumentsInPlace + AUv3 id)
- severity: medium (visible malfunction of the advertised files workflow)
- class: files | plist
- evidence: `plutil -p <app>/Info.plist | grep -iE 'document|UTExport'` returns only `LSSupportsOpeningDocumentsInPlace => true`. No CFBundleDocumentTypes, no UTExportedTypeDeclarations/UTImportedTypeDeclarations, in the app OR the appex. Consequences: (1) `LSSupportsOpeningDocumentsInPlace` is INERT (it only modifies behaviour of declared document types); (2) .parvati/.MUL/.PRO/.scl/.kbm received via AirDrop/mail/Files cannot be "Opened in Parvati" — the only import paths left are the in-app document picker (browse) and dropping into the Files-visible Documents mirror (T6). The app's whole file story (UIFileSharingEnabled + the T6 mirror) advertises a workflow the OS cannot complete from outside.
- deterministic_check: build, then `plutil -p <app>/Info.plist | grep -c CFBundleDocumentTypes` == 0 (fails today). Post-fix: add CFBundleDocumentTypes (5 extensions, role Viewer) + UTExportedTypeDeclarations (com.805labs.parvati-preset/-multi/-program/-scala/-kbm, conforming to public.data+public.content) via the existing PlistBuddy pattern or a `CUSTOM_PLIST` snippet, and extend the configure-time FATAL_ERROR verify to assert the key exists (same shape as the LSSupports check at CMakeLists.txt:376-386).

## F-ios-build-2: `CONFIGURATION=Release` build-command form poisons Parvati_artefacts/Release
- repro: `cmake --build build_ios_bh_build --target Parvati_Standalone Parvati_AUv3 -- -sdk iphonesimulator CONFIGURATION=Release` (everything after `--` goes to xcodebuild verbatim; `CONFIGURATION=Release` is a build-SETTING override, not a config selector)
- severity: low (build hygiene; cost this lane two rebuild cycles, would silently break a CI script)
- class: build
- evidence: with that form the build SUCCEEDS but writes everything to Parvati_artefacts/**Debug** (0 'artefacts/Release' hits in the log), and Xcode leaves 8-byte `APPL????` placeholder stubs at Parvati_artefacts/Release/{Standalone/Parvati.app,AUv3/Parvati.appex}. The NEXT correct build (`--config Release`) then fails rc=65 with `error: unable to create directory .../Parvati.app` (mkdir refuses: path exists as a file) until the stubs are `rm`-ed. No repo doc currently prescribes any iOS build command (README/CONTRIBUTING/docs greps: none), so the tempting form is undocumented-but-likely (it's what a fresh contributor types).
- deterministic_check: run the bad form, then `cmake --build ... --config Release ... -sdk iphonesimulator`; expect rc != 0 with 'unable to create directory'. Remediation: document `--config Release` before the `--` separator in the CMakeLists.txt iOS block comment (or CONTRIBUTING), and/or add a pre-build artefacts-config mismatch guard.

## F-ios-build-3: parvati_tests is not compilable under the iOS toolchain
- tests/hellcat_tests.cpp:58 (`std::system` inside toolAvailable)
- severity: low (blocks simulator CI of the deterministic suite; the plugin lib itself is iOS-clean — everything else compiled)
- class: build | tests
- evidence: `cmake --build build_ios_bh_build --config Release --target parvati_tests -- -sdk iphonesimulator` => rc=65, `tests/hellcat_tests.cpp:58:17: error: 'system' is unavailable: not available on iOS`. Contrast: `parvati_multi_load_test` builds CLEAN for iOS sim (rc=0) — so the suite is one unportable call away from being largely iOS-buildable.
- deterministic_check: the command above (fails today). Remediation: guard toolAvailable()/its callers with `#if ! JUCE_IOS` (the function only probes for optional external analyzers — desktop diagnostics).

## F-ios-build-4: No app icon declared or shipped
- built Parvati.app (no Assets.car / .appiconset output; plutil shows no CFBundleIconName/CFBundleIcons)
- severity: low (App-Store submission blocker eventually; known-deferred in audit/IPAD_TOUCH_TODO.md "Deferred" — re-listed with built-artifact evidence)
- class: plist
- evidence: plutil -p shows zero icon keys; `ls Parvati.app` = {CodeResources, Info.plist, LaunchScreen.storyboardc, Parvati, PkgInfo, PlugIns}.
- deterministic_check: `plutil -p <app>/Info.plist | grep -c Icon` == 0. Fix needs a branding asset (out of scope for code).

## F-ios-build-5: `ENABLE_BITCODE NO` is dead configuration
- CMakeLists.txt:302 and :310 (set_target_properties ... XCODE_ATTRIBUTE_ENABLE_BITCODE NO)
- severity: low (stale surface; no behaviour — bitcode was removed in Xcode 14)
- class: build
- evidence: Xcode 26 build ignores the flag entirely (also visible in the built xcschemes/settings). Mirrors T16's earlier removal of the dead JUCE_XCODE_EXTRA_PLIST_ENTRIES no-op.
- deterministic_check: grep ENABLE_BITCODE CMakeLists.txt (2 hits today). Remediation: delete both lines + comment.

## F-ios-build-6: AudioComponentBundle points at a bundle that does not exist in the iOS appex — UNKNOWN-NEEDS-DEVICE
- built appex Info.plist: NSExtension.NSExtensionAttributes.AudioComponentBundle = com.805labs.parvati.internal; appex contents = {Info.plist, Parvati, PkgInfo, LaunchScreen.storyboardc, _CodeSignature} — no .framework/.bundle with that id
- severity: low (suspicious but JUCE-standard; every JUCE CMake iOS AUv3 ships this shape)
- class: plist | lifecycle
- evidence: ~/JUCE/extras/Build/CMake/JUCEUtils.cmake:1458-1466 builds the `Parvati_AUv3_Framework` (the bundle AudioComponentBundle references) ONLY `if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")`; under iOS the AU links directly into the appex, but juceaide's plist (juce_PlistOptions.cpp:377, fed by JUCEUtils.cmake:1851 `<bundle>.internal`) still declares the key. macOS hosts resolve the key to find the AU; iOS hosts appear to ignore it (JUCE iOS AUv3 plugins demonstrably load in hosts), but only an on-device instantiation proves it for Parvati.
- deterministic_check (needs device): install the .ipa on an iPad, then either open AUM/GarageBand and instantiate "805LABS: Parvati", or run `pluginkot`/`pluginkit -mAvvv` via the device console and confirm the AU registers + opens its view.

## Verified-PASS observations (no action; recorded so they are not re-hunted)

- T1 AUv3 bundle-id convergence works in Release (see baseline).
- App-group entitlements embedded in both simulator binaries (see baseline;
  `codesign -d --entitlements` alone is misleading for ad-hoc simulator
  signatures — parse `__TEXT,__entitlements` instead).
- Appex carries a LaunchScreen.storyboardc (JUCE adds the storyboard to every
  bundle): ~24 KB of inert bloat; harmless, left as-is.
- No `UISupportedInterfaceOrientations~ipad` needed for a family-2-only app
  with UIRequiresFullScreen=true (base key governs iPad).
- The bogus-form stub files documented in F-ios-build-2 are build-dir state,
  NOT committed anywhere (git status clean apart from this audit file).

## Count summary

6 findings: 1 medium (F-ios-build-1), 5 low (2,3,4,5,6-of which 6 is
UNKNOWN-NEEDS-DEVICE). 1 known-deferred re-listed (F-ios-build-4).

## TOP 3 remediation priorities

1. **F-ios-build-1** — add CFBundleDocumentTypes + UTExportedTypeDeclarations
   for the five patch formats via the existing PlistBuddy/config-verify
   pattern; this completes the file workflow the app already advertises
   (UIFileSharingEnabled + open-in-place + the T6 Documents mirror).
2. **F-ios-build-3** — `#if ! JUCE_IOS` the std::system probe so the
   deterministic test suite becomes buildable (and then CI-able) for the iOS
   simulator; multi_load_test already proves the rest compiles.
3. **F-ios-build-2 + F-ios-build-5** — document the correct
   `--config Release` invocation (and/or guard the artefacts mismatch) and
   delete the dead ENABLE_BITCODE lines while in that file.

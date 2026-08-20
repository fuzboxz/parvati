# Parvati Release-Readiness Audit (read-only, 2026-08-20)

Repo state verified at commit `e134bcf`. JUCE = 9 checkout at `~/JUCE`.

## (a) BLOCKERS — must fix before release

1. **macOS binaries are ad-hoc signed only; no Developer ID / hardened runtime / notarization.**
   All signing is `codesign --force --sign -` (CMakeLists.txt:641 VST3 re-seal; deploy target ~CMakeLists.txt:1945–2000 for AU/CLAP/Standalone copies). Grep for `notary|Developer ID|hardened` across CMakeLists + tools/ returns nothing. Gatekeeper will block downloaded, un-notarized VST3/AU/CLAP/.app; README already tells users to `xattr -cr` (README "clear the quarantine" block).
   **Action:** add Developer-ID signing (`--options runtime --entitlements` with `com.apple.security.cs.disable-library-validation` if needed), `codesign --verify --deep`, and `xcrun notarytool submit` + `stapler` steps to a release script/target; drop the `xattr` workaround from README after.

2. **iOS signing is a documented placeholder — no device/.ipa path exists.**
   `PARVATI_IOS_DEVELOPMENT_TEAM ""` (CMakeLists.txt:313–315); entitlements say "PLACEHOLDER ids pending a product/signing decision" (parvati.entitlements:10, parvati-auv3.entitlements:9); `deploy_ios` is an explicit template that "will fail at the codesign step" (CMakeLists.txt:2038–2068). App-Group `group.com.805labs.parvati` must be registered with the team.
   **Action:** set a real team id, register the App ID + App Group (com.805labs.parvati, .AUv3, group.…), fill ios/exportOptions.plist, and produce a signed .ipa (or ship macOS-only in v0.1.0 and state that clearly).

3. **Licensing: vendored Mutable Instruments clouds/rings/warps/stmlib (MIT) is entirely absent from NOTICES.md.**
   ~50 files under Source/dsp/clouds/ carry "Copyright 2012 Emilie Gillet … MIT/CC-MIT" headers (e.g. Source/dsp/clouds/stmlib/stmlib.h:1–15); grep for `clouds|stmlib|warps|rings` in NOTICES.md/README.md finds nothing. MIT requires the copyright + permission notice accompany distributions.
   **Action:** add a "Mutable Instruments Eurorack DSP (clouds/rings/warps/stmlib, MIT)" section to NOTICES.md with upstream URLs; keep the in-file headers (already present).

4. **NOTICES.md misstates the JUCE license.** It claims JUCE core modules are "ISC License … for most uses"; JUCE 9 is dual-licensed **AGPLv3 / commercial** (~/JUCE/LICENSE.md). Since Parvati is AGPL-3.0 this is compatible, but the notice text is wrong. Also `JUCE_DISPLAY_SPLASH_SCREEN=0` (CMakeLists.txt:606) is a **no-op in JUCE 9** — juce_gui_basics.cpp:60-61 emits "This version of JUCE does not use the splash screen, the flag is ignored". **Action:** correct the JUCE section (AGPLv3/commercial, ISC only for a few modules per JUCE's LICENSE.md), delete the dead splash define, and confirm the release ships corresponding source (AGPL §13/§6).

5. **README describes removed features and omits current ones.** The "Microtonal tuning" section (README.md:98–126) and the Features bullet (README.md:32) document Custom 12-entry tables + Scala `.scl`/`.kbm` import — deleted in the 2026-08-19 QoL wave (CHANGELOG [Unreleased] "Custom Scala tuning removed"; only raga presets 1–33 remain). README never mentions iOS/AUv3, Apple-silicon-only, or any system requirements (grep: zero hits).
   **Action:** rewrite the tuning section, add Requirements (macOS arm64, JUCE 9 @ ~/JUCE, iOS 14+ for AUv3) and note x86_64/Rosetta is dropped (CMakeLists.txt:105–110 arm64-only default).

6. **Version 0.1.0 + `[Unreleased]`**: CMakeLists.txt:4 `VERSION 0.1.0` with the entire history still under `[Unreleased]` (CHANGELOG.md:5 — no released version exists). `PARVATI_VERSION` flows only into the editor label (Source/PluginEditor.cpp:3029, 4150).
   **Action:** decide the release number, bump CMake `project(VERSION …)`, convert `[Unreleased]` → `[x.y.z] — 2026-08-20` (it's enormous; extract user-facing highlights, keep detail), and tag.

## (b) SHOULD-DO before release

1. **No CI at all** (no `.github/`). Add a GitHub Actions macOS-14/15 arm64 workflow: configure + build VST3/AU/CLAP (JUCE via cache/checkout), run `ctest` (94 test sources, 118 checks reported green locally — not re-run in this audit), run `auval -v aumu Prvt 805L`, VST3 `validator`, and pluginval ≥ level 5. The static scanners are already one target: `cmake --build --target parvati_static_checks` (CMakeLists.txt:2150–2160).
2. **macOS deployment target unset for desktop** (only iOS sets 14.0, CMakeLists.txt:312) → binaries target the build SDK minimum, silently limiting the install base. Set `CMAKE_OSX_DEPLOYMENT_TARGET` (e.g. 13) for macOS and record it in README.
3. **No packaging**: artefacts land in `build/Parvati_artefacts/<Config>/{VST3,AU,CLAP,Standalone}` but there is no DMG/zip/pkg. Add a `package` target that zips the three bundles + .app (post-notarization) with a README/NOTICES copy.
4. **Release-notes must call out arm64-only** (Intel Macs unsupported; CMakeLists.txt:105–110, universal possible via `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`).
5. **Manual QA that can't be automated:** (i) run `auval -v aumu Prvt 805L` on the shipped AU; (ii) VST3 SDK `validator` (the moduleinfo re-seal at CMakeLists.txt:636–641 exists precisely for it); (iii) pluginval; (iv) DAW smoke matrix (Logic AU, Live/Reaper VST3, Bitwig CLAP, Standalone audio/MIDI); (v) iOS on-device checklist audit/ios_device_checklist.md D1–D8 — note D4's "open-in NOT yet implemented" text is stale (IosOpenIn.mm landed per CHANGELOG) and it still lists .scl/.kbm UTIs removed with Scala; refresh the checklist and run it.
6. **ATTRIBUTION.md vs LICENSE inconsistency**: presets/ATTRIBUTION.md claims "Parvati is therefore itself licensed under the GPL-3.0" while LICENSE/NOTICES are AGPL-3.0. GPL-3.0→AGPL-3.0 embedding is one-way compatible, but fix the sentence.
7. **Documented non-fixes to disclose in release notes** (CHANGELOG "Deliberately NOT done"): Spectral 64 ms unreported latency, ClockedDelay 1–3.75 s retarget glide, sub-32 kHz AA, plus undo-clears-on-part-switch UX (W2 doctrine).

## (c) NICE-TO-HAVE

1. First-run experience is solid (embedded goldencard banks auto-extract; FactoryPresetInstaller.h is idempotent + marker fast-path) — consider an explicit "factory presets installed" toast for first-time users.
2. Ships with `-Werror`-clean Source/: zero TODO/FIXME/XXX/HACK and no real placeholder strings (all grep hits are benign comments, e.g. Source/ui/PatchPage.cpp:48).
3. Add `spctl --assess` + `codesign -dv` verification to the future CI; pin clap-juce-extensions to a tagged release when one exists (currently SHA c1a5ad0, CMakeLists.txt:196–199).
4. ambika_reference/ (~GPL reference tree) is outside Source/ and never built except by firmware_parity_test — keep excluding it from release archives.

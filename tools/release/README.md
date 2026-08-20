# tools/release — macOS distribution signing + notarization

`sign_and_notarize.sh` turns a fresh `build/Parvati_artefacts/Release` tree
into a Gatekeeper-clean distribution:

1. **Signs** every bundle it finds (`Parvati.vst3`, `Parvati.component`,
   `Parvati.clap`, `Parvati.app`) with your **Developer ID Application**
   certificate, the **hardened runtime**, a secure timestamp, and the
   `disable-library-validation` entitlement from `macos_release.entitlements`
   (required for audio plugins — hosts load plugin binaries into their own
   processes; JUCE documents this setting for VST3/AU/CLAP distribution).
   Inner Mach-O binaries are signed before their bundles (no `--deep` for the
   actual signing), and every bundle is then verified with
   `codesign --verify --deep --strict`.
2. **Notarizes** each bundle: `xcrun notarytool submit --wait`, then
   `xcrun stapler staple` + `stapler validate`.
3. **Packages** a `Parvati-macOS.zip` (via `ditto`, preserving signatures and
   resource forks) containing the four bundles **plus `README.md` and
   `NOTICES.md`** — the AGPL/MIT/GPL notices must accompany the distribution.
   Final `spctl --assess` sanity check on each bundle.

## Prerequisites (one-time, per machine)

- Apple Developer Program membership; a **Developer ID Application**
  certificate in your login keychain:
  ```bash
  security find-identity -v -p codesigning
  # export DEV_ID="Developer ID Application: ACME Inc (ABCD123456)"
  ```
- **App Store Connect API key** (recommended) — create one at
  <https://appstoreconnect.apple.com/access/integrations/api> with the
  **Developer** or **App Manager** role; notarization needs at least
  `Create App Store Connect API Key` + upload access. Download the `.p8`
  (it is shown **once**). Notarytool can also be driven by Apple ID +
  app-specific password, or a stored keychain profile — all three are
  supported:
  ```bash
  # Option A: ASC API key (CI-friendly, no interactive prompts)
  export ASC_KEY_ID=ABCDE12345 ASC_ISSUER_ID=12345678-... \
         ASC_AUTH_KEY_PATH=/secure/AuthKey_ABCDE12345.p8

  # Option B: store a keychain profile once, then name it
  xcrun notarytool store-credentials parvati-notary \
        --apple-id you@example.com --team-id ABCD123456 \
        --password <app-specific-password>   # appleid.apple.com → Sign-In & Security
  export PARVATI_NOTARY_PROFILE=parvati-notary

  # Option C: pass the Apple-ID trio directly
  export APPLE_ID=you@example.com APPLE_ID_PASSWORD=xxxx-xxxx-xxxx-xxxx TEAM_ID=ABCD123456
  ```

## Run

```bash
cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release
cmake --build build_release --target Parvati_VST3 Parvati_AU Parvati_CLAP Parvati_Standalone -j

export DEV_ID="Developer ID Application: ..."
# ...plus one credential set from above...
tools/release/sign_and_notarize.sh build/Parvati_artefacts/Release dist
```

`SKIP_NOTARIZE=1` signs/verifies/packages without contacting Apple (useful to
test the signing path, e.g. with a self-signed cert — the script still refuses
identities not present in the keychain).

## What this does NOT cover

- **iOS / .ipa** — App Store distribution uses a different (Xcode archive /
  `xcrun altool` / Transporter) path and the App-Group/App-ID registration
  (`group.com.805labs.parvati`); see `audit/release_readiness.md` BLOCKER 2.
- Version stamping — the version comes from `project(VERSION …)` in
  `CMakeLists.txt`; bump it before building release artefacts.
- The VST3 re-sign in `CMakeLists.txt` (post-build, seals `moduleinfo.json`)
  runs *before* this script and is superseded by it.

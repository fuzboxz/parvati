# Parvati iOS — On-Device Verification Checklist

Source: iOS quality hunt 2026-08-19 (five lanes, `audit/ios_hunt_20260819/lane_*.md`).
Covers every item the lanes classified `UNKNOWN-NEEDS-DEVICE` — behaviour that only
a real iPad (or the simulator where noted) can settle. Headless-deterministic parts
of the same findings are already pinned by tests; this file is the manual half.

Build for the device first:
```
cmake -S . -B build_ios_dev -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 -DPARVATI_IOS_DEVELOPMENT_TEAM=<TEAMID>
cmake --build build_ios_dev --config Release --target Parvati_Standalone Parvati_AUv3 -- -sdk iphoneos
```
(Install the Standalone .app; hosts pick up the AUv3 appex embedded in it.)

---

## D1. AUv3 instantiation in real hosts (F-ios-build-6: AudioComponentBundle)

**Why:** the appex plist declares `AudioComponentBundle = com.805labs.parvati.internal`,
but no bundle with that id exists inside the .appex. This is the standard JUCE iOS
AUv3 shape (JUCEUtils.cmake only builds the referenced `_Framework` bundle on Darwin;
iOS hosts are believed to ignore the key) — only a device proves Parvati loads.

**Steps:**
1. Install the app (which embeds `Parvati.app/PlugIns/Parvati.appex`) on the iPad.
2. Open **AUM** → add instrument → look for `805LABS: Parvati` (aumu).
3. Tap the added unit: the editor must open, render, and play notes from an on-screen
   or host keyboard.
4. Repeat in **GarageBand** ("Audio Unit Extensions" track) and **BeatMaker 3** if available.
5. Optional console check (device attached): `xcrun devicectl device console` filtered
   for `parvati` while adding the unit — the AU registration must produce no
   `AudioComponentBundle` load errors.

**Expected:** Parvati appears and instantiates in every host; no console error
referencing `com.805labs.parvati.internal`. If ANY host fails with a bundle error,
the fix is to make JUCEUtils build the AUv3 framework on iOS or strip the key.

---

## D2. Interruption / route-change behaviour (F-ios-lc-2, post-fix)

**Why:** AUv3 hosts tear down and re-allocate render resources on phone calls, Siri,
screen lock, and headphone connect/disconnect. Held/hardware-played notes used to
survive the cycle gated (stuck sustain) because note-offs lost in the window were
never re-delivered and JUCE only clears notes on a *rate change*.

**Steps:**
1. In AUM (or GarageBand), instantiate Parvati, load a patch with a long release.
2. Hold a chord from a **hardware** MIDI keyboard (or host-sequenced long notes).
3. While sustaining: accept an incoming call (or trigger Siri), then end it.
4. Repeat while unplugging/plugging wired headphones mid-chord.
5. Repeat with the screen locking mid-chord.
6. After each interruption, listen and watch the voice state.

**Expected:** after re-allocation no voice sustains forever; notes held *through* the
interruption follow the documented policy (voices cleared on the re-allocate seam —
see the fix lane's report for the chosen semantic), audio resumes without a click
beyond the normal fade-in, CPU state (FX, tuning, presets) is intact. Any infinite
sustain = FAIL.

---

## D3. Save-from-host visibility in Files (F-ios-lc-4 / F-ios-files-1, post-fix)

**Why:** inside an AUv3 host, the app-group preset tree is not browsable by the
UIDocumentPicker save sheet, so a patch saved from AUM/GB used to land outside the
tree the preset browser scans — the user's save never appeared in the preset menu.

**Steps:**
1. In AUM, load Parvati, edit a patch, Save As… → pick a name → choose a destination
   (test all three: *On My iPad*, *iCloud Drive*, and any third-party provider).
2. Open the **Files app** → navigate to the chosen destination: the file must be there,
   complete, and (for .parvati) openable in a text view (not 0 bytes).
3. Reopen Parvati's preset menu (in the SAME host session and after restarting the host):
   the saved preset must now appear under USER (verify the import/reverse-sync path the
   fix lane implemented — see its `fix_ios-files.md` for the mechanism).
4. Repeat the whole flow in the **Standalone** app; the saved preset must be visible in
   BOTH apps afterwards (shared container round-trip).

**Expected:** every save path produces a complete file AND a USER-menu entry in both
apps. A save that only exists in Files with no browser entry = the import path failed.

**D3b. Launch-sync of host saves into Files (F-ios-lc-4, the option-(a) mirror):**
1. In AUM/GB, save TWO different presets (they land in the shared App-Group USER tree;
   the per-save Documents mirror inside the extension is invisible by design).
2. Do NOT open the Standalone app yet. Check Files: those two presets are legitimately
   NOT yet under On My iPad/Parvati/USER (the sync runs at Standalone launch).
3. Open the Standalone app once, wait a few seconds, re-check Files: BOTH presets must
   now appear under On My iPad/Parvati/USER, complete (not 0 bytes).
4. Delete one copy in Files, re-open the Standalone: it must NOT resurrect while the
   source is older (newest-wins, additive-only). Saving that preset again in the host
   must bring it back on the next Standalone launch.

---

## D4. Open-in from Files / AirDrop / Mail (F-ios-build-1; integration LANDED — device verify only)

**Why:** `CFBundleDocumentTypes` + `UTExportedTypeDeclarations` are declared
(CMakeLists grafted via `ios/parvati_filetypes.plist`) for .parvati/.PRO/.MUL
(**3 types** — the former .scl/.kbm entries were removed together with the
Scala/custom-tuning subsystem in the 2026-08-19 QoL wave), which is what makes
the OS offer "Open in Parvati"/"Copy to Parvati".

**Integration status:** the open-in HANDLING is implemented —
`Source/ui/IosOpenIn.{h,mm}` routes an opened preset (.parvati/.PRO/.MUL)
atomically into the shared USER tree and loads it through the normal
message-thread seams (headless-pinned by `tests/ios_openin_test.cpp`).
What remains is purely on-device confirmation of the OS interaction.

**Steps:**
1. Build config sanity first (deterministic, no device):
   `plutil -p build_ios_dev/Parvati_artefacts/Release/Standalone/Parvati.app/Info.plist`
   must show `CFBundleDocumentTypes[0].LSItemContentTypes[0] = com.805labs.parvati-patch`
   and `UTExportedTypeDeclarations[0..2].UTTypeIdentifier` (the three ids:
   parvati-patch / parvati-program / parvati-multi).
2. AirDrop a .parvati from another iDevice → the share sheet must offer Parvati.
3. Mail yourself a .PRO/.MUL → long-press the attachment → "Copy to Parvati" appears.
4. In Files, tap a .parvati → Parvati opens (icon shown by type).
5. Choose "Copy to Parvati": the app launches AND the patch must appear in the
   USER bank of the patch browser (imported + loaded). If the app launches but
   the file does not appear, capture the console (`xcrun devicectl device console`)
   — the security-scoped-URL read is the most likely failure point.

**Expected:** steps 2–5 all pass. A .scl/.kbm file must NOT offer Parvati anymore
(negative check — types removed).

---

## D5. Hardware-keyboard: host key return + combo focus (F-ios-touch-6)

**Why:** `EDITOR_WANTS_KEYBOARD_FOCUS=TRUE` makes the AUv3 view a first responder so
KeyboardView musical typing works. Unverified: whether keys the editor does not
consume still reach the host, and how the FX type combo behaves once focused.

**Steps:**
1. Attach a Smart Keyboard/BT keyboard to the iPad.
2. In AUM with Parvati focused: press host-relevant keys (arrows to move between AUM
   units, space/transport if the host binds it). Record which ones AUM still receives.
3. Tap the on-screen KeyboardView and play (musical typing works; letters, not combos).
4. Press Tab repeatedly: focus must never land on an invisible control — if typing goes
   dead while "nothing is focused", that is the parked-zoom-trio class (F-ios-touch-3,
   fixed by the touch lane — verify the fix holds: after any Tab walk, tapping KBD
   resumes musical typing immediately).
5. Tap an FX type combo, then press ↑/↓: the FX type must change WITH engagement
   seeding (audible immediately) — never a silent type switch.

**Expected:** host navigation keys still work when the editor does not consume them;
musical typing survives the Tab cycle; combo arrows seed audibly. Record any host
(AUM vs GB) that swallows arrows entirely — that would justify returning unhandled
keys explicitly.

---

## D6. Save-picker overwrite prompt (F-ios-files-6)

**Why:** JUCE's iOS save FileChooser ignores `warnAboutOverwriting`; collision
behaviour is provider-dependent.

**Steps:**
1. Save a preset named `dupe.parvati` → success.
2. Save another preset under the SAME name to the SAME destination:
   - *On My iPad* destination,
   - *iCloud Drive* destination.
3. Observe: is an overwrite confirmation shown, or a silent replace?
4. Open the file afterwards and confirm it contains the SECOND patch's content.

**Expected:** no crash either way; final content is the second save; note per provider
whether a prompt appeared (iCloud expected to prompt, local may silently replace —
document as known platform behaviour; warn in the save dialog text if it matters).

---

## D7. Thermal behaviour at 2x oversampling (F-ios-perf-1/-2, post-gate)

**Why:** desktop-measured worst-case (96 voices, 2x filter OS) ≈ 0.26x realtime on
M-series silicon; scaled to an A12-class core that is borderline (≈0.65–1.04x) at the
absolute worst case. The wave adds an iOS oversampling gate (see the perf lane's fix
report); sustained-play thermal behaviour still needs a device.

**Steps:**
1. iPad Settings → Battery → verify charge ≥ 50% and no thermal warning banner.
2. In AUM: one Parvati, ALL 6 parts, Voices 16, long-release patch, dense playing for
   **10 minutes** at the iOS-default oversampling.
3. Watch the status-bar CPU readout and listen for crackle/dropouts every ~2 min.
4. If the device shows the "hot" banner or throttles, note the elapsed time and
   the CPU% shown.
5. Repeat at the iOS max allowed OS setting (per the gate) for 5 minutes.

**Expected:** no audible dropout at the default during sustained typical play (<32
voices most of the time); at the absolute 96-voice worst case, CPU stays below ~80%
of realtime budget and no thermal banner within 10 minutes. If the banner appears,
lower the iOS default/ceiling (the gate's constants are in SettingsPanel/processor —
see the perf fix report) and re-run.

---

## D8. GarageBand musical-context feed + the manual arp clock (AUv3 follow-up, 2026-08-19)

**Why:** community reports say GarageBand for iOS sends NO tempo/transport to AUv3
plugins (no `AUHostMusicalContextBlock`), which would leave tempo-synced arp/seq
free-running. Parvati now resolves the arp clock as HOST bpm when the playhead
carries one, else a persisted MANUAL bpm (Settings ▸ Arp Clock; default 120), with a
live source status line and a one-time "No host tempo" transient hint. Whether GB
feeds context at all — and what AUM/Logic/Cubasis do — only a device settles.

**Steps:**
1. In GarageBand, load Parvati on an instrument track, enable the arpeggiator
   (resolution 1/8), set a distinctive project tempo (e.g. 90 BPM).
2. Open Settings ▸ Arp Clock: note the status line ("Host tempo: …" vs "No host
   tempo - manual tempo active") and whether the one-time hint fired.
3. If GB feeds no tempo: set the manual slider to 90 and confirm the arp locks to
   the project tempo by ear/metronome.
4. Repeat steps 1–2 in AUM (tempo enabled), Logic Pro for iPad, Cubasis — expect
   "Host tempo: 90.0 BPM (manual ignored)" there.
5. Save the GB project, kill the host, reopen: the arp clock source + manual bpm
   must round-trip with the session state.

**Expected:** no host feeds a WRONG bpm; every no-tempo host reports Manual and the
slider governs the arp rate; every tempo host reports Host and the slider is inert
("manual ignored"). GB specifically: expect Manual (community-consistent) — if it
reports Host instead, update this section and the CHANGELOG note.

---

## Post-run ledger

Record results back into `audit/ios_hunt_20260819/` (e.g. append a `device_results.md`)
with device model, iPadOS version, host app versions, and PASS/FAIL per section.
Anything FAILing here becomes a new hunt item with the device evidence attached.

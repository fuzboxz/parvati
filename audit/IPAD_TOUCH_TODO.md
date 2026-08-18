# Parvati — iPadOS / Touch Audit TODO

Source: full-codebase touch/iPadOS audit (three-lane review: input handling, HIG/layout,
platform integration). Evidence-backed from the working tree at commit `a3987db`.
Check items off (`[x]`) as they are implemented and verified.

---

## Blockers

- [x] **T1. AUv3 bundle-id mismatch (signing/validation failure on device builds).**
  Generated AUv3 plist hard-codes `CFBundleIdentifier = com.805labs.parvati.parvatiAUv3`
  (juceaide template wins over Xcode attr), but `CMakeLists.txt:226` sets
  `PRODUCT_BUNDLE_IDENTIFIER com.805labs.parvati.AUv3` and `parvati-auv3.entitlements`
  pins `application-identifier $(AppIdentifierPrefix)com.805labs.parvati.AUv3`.
  → Make the three agree. Preferred: keep intended id `com.805labs.parvati.AUv3` and
  add a build-time PlistBuddy patch of the AUv3 plist's `:CFBundleIdentifier`
  (fold into the T2 build-time plist step). Verify with
  `plutil -p build_ios/.../Parvati.appex/Info.plist`.

- [x] **T2. Move iOS plist patching to build time + add `UIRequiresFullScreen`.**
  `CMakeLists.txt:249-277` patches the Standalone plist at *configure* time guarded by
  `if(EXISTS …)` — silently no-ops in a fresh build dir (juceaide generates the plist
  during the build), dropping `UIBackgroundModes:audio`, `UIFileSharingEnabled`, and the
  landscape orientation arrays.
  Also: plist has `UIRequiresFullScreen=false` + landscape-only iPad orientations →
  ITMS-90474 App-Store rejection. Add `UIRequiresFullScreen true` (also disables
  Split View/Slide Over — matches design).
  → Replace configure-time patch with `add_custom_command(TARGET Parvati_Standalone
  PRE_BUILD …)` patching `CMakeFiles/Parvati_Standalone.dir/Info.plist` (the actual
  INFOPLIST_FILE); same mechanism patches the AUv3 plist (T1).

- [x] **T3. FX-slot power/bypass toggle hit area ~10×12pt.**
  `FxSlotCard.cpp:547`: `powerToggle_->setBounds (header.removeFromRight (kHeaderH - 2).reduced (2))`
  with `kHeaderH = 16`. Only enable/bypass control per FX slot; reliably untappable.
  → Grow the hit area to 44×44 (invisible transparent button shell painting the small
  glyph, or reserved 44pt band). Visual glyph may stay small.

- [x] **T4. Landscape-only layout has no scroll safety net.**
  Generator hosts `SynthWorkspace.cpp:132-141` / `FxWorkspace.cpp:117-126` are plain
  `juce::Component`s with no Viewport; any host frame shorter than the tuned content
  height clips unrecoverably. Same for `PatchPage.cpp:559-590` (6×56pt rows + hosted
  Global page, no scroll).
  → Wrap the active-editor host content in a vertical `juce::Viewport` (both workspaces)
  and wrap PatchPage's hosted page (mirroring `FxMatrixView`'s viewport pattern).

  DONE (T4 batch): both workspace active-editor hosts + PatchPage body now scroll
  vertically (as-needed scrollbar; no layout change when content fits — reflowToWidth
  grows a fitting page to the view height). Wheel-over-knob scrolls (verified at JUCE
  source level: Slider calls the base class when its wheel is disabled, which bubbles
  to the Viewport); touch drags on control cells don't scroll (ParamControl + PartRow
  set the viewport ignore-drag flag), background drags do; desktop mouse drag behaviour
  unchanged (default ScrollOnDragMode::nonHover is touch-only). Regression checks added
  in tools/editor_test.cpp ([3d]) and tests/editor_test.cpp ([6b]).

## Major

- [x] **T5. AUv3 ships on iPhone (`TARGETED_DEVICE_FAMILY "1,2"`)** — panes ~330-560pt
  vs the 1024pt width floor (`PluginEditor.cpp:2831`).
  → Set `TARGETED_DEVICE_FAMILY "2"` (iPad-only) on `Parvati_Standalone` + `Parvati_AUv3`
  until a compact layout exists.

- [x] **T6. Files-app visibility claim not realized.**
  `UIFileSharingEnabled` is set ("saved patches reachable in Files"), but user saves go
  to the App Group container (`PluginProcessor.cpp:823-825` → `Parvati/USER`), invisible
  to Files.
  → Write user patch saves (Save .PRO / Save Parvati default dirs, `PluginEditor.cpp:3525,
  3555`) to `<app-sandbox>/Documents` (desktop behaviour unchanged; iOS only).

  DONE (T6 batch): chose the MIRROR design over redirecting saves — the App-Group tree
  stays the single source of truth (PresetBrowser + AUv3 extension + Standalone keep one
  tree; older saves remain loadable), and each successful save into the USER area is
  copied to `<sandbox>/Documents/Parvati/USER/…` (`mirrorUserSaveToDocumentsIOS`,
  `#if JUCE_IOS`, PluginEditor.cpp). Load picker + PresetBrowser unchanged (browser
  reads the shared tree; the iOS document picker opens at its browse root where the
  mirrored Documents are reachable). Failed copies are non-fatal; stale mirrors are
  never deleted (group tree is authoritative). Desktop paths byte-identical
  (helper + calls compiled out).

- [x] **T7. Sub-44pt combos and popup menu rows (systemic).**
  ParamControl combos 28pt (`PluginEditor.cpp:949`), PatchPage combos 24pt, SettingsPanel
  28pt; every `PopupMenu` except the FX type picker uses ~22pt default rows — including
  PresetBrowser's nested Factory▸Bank menu (core workflow).
  → One L&F override in `ParvatiLookAndFeel`: `getIdealPopupMenuItemSize` → 44pt rows
  (app-wide, inherits to every popup); raise combo *hit* heights to 44 on touch/iOS
  (transparent pad ok, visual height may stay smaller).

- [x] **T8. Keyboard glissando is visual-only (reads as stuck note).**
  `KeyboardView.cpp:97-102`: `mouseDraggedToKey` ignores the swept key; base class
  re-lights it while the engine holds the originally-pressed note.
  → Use the existing per-source map: on key change, note-off old + note-on new
  (`fireNoteCallback`), update `mouseDownNotesBySource_`. Off-key release guard
  (`mouseUp`, lines 118-124) already covers failure modes.

- [ ] **T9. Small primary controls: SeqLengthStepper − / + ≈32×20pt
  (`SeqLengthStepper.cpp:56-70`); FxRoutingBar ◀ ▶ topology steppers 24×28pt
  (`FxRoutingBar.cpp:26-27`).**
  → Reach 44pt targets (stack − above + full-width if needed; flow row is 50pt tall).
  Also bump EQ knobs 42→44 (`kEqKnobSize`), "+ Add Modulation" 30→44pt
  (`ModMatrixView.cpp:882`, `FxMatrixView.cpp:1004`), PatchPage zone knobs 40→44.

  STATUS (partial — T9 batch): (b) steppers 44×44 DONE, (c) EQ knobs DONE — note
  the dial actually drew at 36px (row-capped), now 44 via `kEqRowH` 52→60 —
  (d) Add-Mod buttons 44pt DONE, (e) PatchPage zone knobs 44pt DONE. (a) STOPPED
  pending a product decision: two 44×44 buttons cannot fit the 72×64 step-grid
  cell, and even a modest cellH bump keeps stacked buttons at ~62×18. Options:
  redesign the grid (e.g. 6 columns × 96×70 cells — fits the generator host at
  ~640×285 — or popup-based length entry reusing the T7 44pt rows). ALSO
  DISCOVERED (pre-existing, RESOLVED): the FxRoutingBar Dry/Wet row no longer
  starves to 0×0 at the default editor size — FxWorkspace's kTopRowNaturalH
  (264) floors the top row tall enough for flow + EQ + ctrl; no layout-budget
  decision needed (W7 note).

## Minor

- [x] **T10. Unify touch slop constants.** Long-press cancel is >8px
  (`PluginEditor.cpp:1108`) but clean-tap gate is <=5px (`:1135`) — 6-8px drift opens
  the context menu from what felt like a tweak. One shared constant (5px).

- [x] **T11. Two-finger long-press hazard.** Finger 1 long-press can open the modal
  menu while finger 2 is mid-drag on another knob (`ParamControl::mouseUp` doesn't
  check for other active drags). Bail out of `showContextMenu()` if another
  `MouseInputSource` is dragging.

- [x] **T12. Audio-load probe reset is right-click-only** (`PluginEditor.h:767`) —
  unreachable on iPad while its tooltip advertises it. Accept touch tap/long-press.

- [x] **T13. `[MOD]` mode dead-ends non-source sub-tabs.** `GroupPager.cpp:77-87`
  returns unconditionally even when the tab maps to no mod source → pager becomes
  inert with no hint. Only `return` when `src >= 0`; else fall through to
  `TabBarButton::clicked`.

- [x] **T14. No screen-wake.** `Desktop::setScreenSaverEnabled(false)` while an editor
  exists; restore in `~ParvatiEditor` (mirror the zoom-reset pattern at
  `PluginEditor.cpp:2877-2881`). iOS only.

  DONE (T14/T15 batch): constructor disables the screensaver next to the
  iOS zoom default (`#if JUCE_IOS`, same gate); destructor re-enables it next
  to the zoom reset. Desktop compiled out.

- [x] **T15. Knob readout shrink floor 9pt** (`ParvatiLookAndFeel.cpp`
  `drawRotarySlider` `juce::jmax (9.0f, …)`) — raise floor to 11pt; flow-diagram
  8-10pt fonts → 10/12pt.

  DONE (T14/T15 batch): shrink floor 9 → 11pt (over-long values now draw past
  the dial edge rather than shrink below touch readability); FxFlowDiagram
  blockFont 10 → 12, endFont 8 → 10 (FX labels still degrade to a bare digit
  in narrow blocks; IN/OUT sized to glyphs).

- [x] **T16. Remove dead no-op** `JUCE_XCODE_EXTRA_PLIST_ENTRIES ""`
  (`CMakeLists.txt:219-222`, comment itself declares it broken).

## Deferred (needs assets / on-device decisions)

- iOS app icon set (no ICON/xcassets yet — needs branding asset).
- AUv3 FilePicker behavior per host (AUM vs GarageBand) — needs device testing.
- Hover-driven dest/row highlight gating on `!isTouch()` (cosmetic).
- Concurrent internal DnD drags share one affordance flag (rare, visual-only).
- **AUv3 panes below the 1024pt floor collapse header chrome (W7-known,
  round-3 lane-C finding 1).** The AUv3 wrapper force-resizes the editor to
  the host's pane (setResizeLimits is desktop-only advice), and the header's
  fixed budgets mean SYNTH/FX/Part/preset collapse to 0px width in narrow
  panes (AUM with keyboard open ~570pt, GarageBand panes ~700pt). Content
  pages degrade gracefully (44pt floors + scroll) — the header needs an
  adaptive design (overflow "…" folding and/or a horizontal viewport) before
  it degrades. Not fixed in W7: a product/design decision, not a contained
  patch.
- **Note routing is first-match, firmware triggers every accepting part
  (W7-known, lane-B finding 4).** With an Omni part ahead of a channel part,
  a note on that channel plays only the first on Parvati but BOTH on hardware
  (also misroutes the on-screen keyboard when the current part's channel
  collides). Changing it alters multitimbral semantics — needs an explicit
  product decision (and the UI keyboard injection resolved to a uniquely
  matching channel).
- **Polyphonic aftertouch is silently ignored (W7-known, lane-B finding 5).**
  Channel pressure works (MOD_SRC_AFTERTOUCH per channel); per-note poly AT
  would need a `handleAftertouch` override writing the channel-tagged active
  voices. Firmware `Part::Aftertouch` writes MOD_SRC_AFTERTOUCH to the part's
  voices — a contained port once prioritized.

---

## Verification notes

- iOS compile check: `cmake --build build_ios --target Parvati_Standalone` (Xcode dir).
  NOTE: with the default device SDK (`iphoneos`) Xcode requires a development
  team to sign; for a signing-free verification build configure with
  `-DCMAKE_OSX_SYSROOT=iphonesimulator` (the simulator SDK does not enforce
  entitlements signing).
- Desktop regression: `cmake --build build -j` + run `tests/` suites touched by sizing
  changes (`parvati_ipad_hig_sizing_test` pins touch constants — update alongside).
- After T1/T2: fresh build dir configure→build→`plutil -p` the Standalone + appex plists.

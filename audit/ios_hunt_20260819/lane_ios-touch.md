# Lane: ios-touch — iPad interaction quality (HIG/touch)
Parent hunt: iOS quality remediation, 2026-08-19. Reviewed at HEAD = b889205
(after W11). JUCE 9.0.1 at ~/JUCE. Prior audit audit/IPAD_TOUCH_TODO.md read
first; T1–T8, T10–T16 items were re-verified as done/known where they touch
this lane and are NOT re-reported (except T9a, still open — see F-ios-touch-2).

Scope reviewed: PluginEditor.cpp (W9 adaptive header, resized(), safe-area trim,
ParamControl long-press/context menu, key handling), ui/FxSlotCard.cpp
(FxTypeCombo incl. the new W10b keyPressed), ui/KeyboardView.{h,cpp},
ui/PresetBrowser.h (popup), ui/CentralModBar.cpp, ui/SeqLengthStepper.cpp,
ui/FxRoutingBar.cpp, ui/FxMatrixView.cpp, ui/PatchPage.cpp, ui/SettingsPanel.cpp,
ui/MulExportDialog.cpp, ui/TuningEditor.cpp, ui/IconButton.h,
ParvatiLookAndFeel popup sizing, and the JUCE iOS peer sources
(juce_UIViewComponentPeer_ios.mm, juce_Windowing_ios.mm, juce_PopupMenu.cpp,
juce_FocusHelpers.h) for platform-behaviour evidence.

---

## F-ios-touch-1: CentralModBar `<`/`>` pill scrollers are 30×30 — below the 44pt HIG floor and unpinned
- Source/ui/CentralModBar.cpp:46 (`constexpr int kNavW = 30`), :494–497 (`navH = juce::jmin (kNavW, kPillH)` = 30; `navPrev_/navNext_` bounds 30×30); role: :383–390 (the ONLY way to scroll the pill band — one tap = one full viewport width)
- severity: medium
- class: touch
- evidence: the nav scrollers are `juce::TextButton`s laid out at exactly 30×30, vertically centred in the pill band. A deliberate "compact, quiet chrome" visual choice (comment at :41–45), but they are the primary navigation for an overflowing mod-source band — the exact class the HIG contract test exists to gate. tests/ipad_hig_sizing_test.cpp pins 16 named constants (kStepBtnW/H, kEqKnobSize, kPowerHitSize, kPopupRowHeight, …) but has NO entry for the nav buttons, so this target escapes the contract. On iPad, a 30×30 chevron beside 56pt pills is a mis-tap magnet (adjacent chevron is 4pt away from the pill viewport — a slightly-overshot tap lands on a PILL and switches generator pages).
- deterministic_check: (a) pin it: add `static_assert`/check in tests/ipad_hig_sizing_test.cpp for a new `CentralModBar::kNavHit` >= 44 once the fix lands (e.g. keep the 30pt glyph, pad the hit band to 44×kPillH via a transparent border or a sibling hit-test override, the exact idiom FxSlotCard::kPowerHitSize/T7 combos use). (b) regression: extend tools/editor_test.cpp's tree walk to find the two nav TextButtons ("<"/">") in CentralModBar and assert `getWidth() >= 44 && getHeight() >= 44`.

## F-ios-touch-2: SeqLengthStepper −/+ ≈32×20 — the STOPPED T9a item is still open and unpinned
- Source/ui/SeqLengthStepper.cpp:74–83 (resized: number takes `min(b.h-18, 20)`, buttons get the leftover row — at the Sequencer group's clamped ~72×64 cell that is ~32×20 per button); natural cell declared at Source/PluginEditor.cpp:2523 (`{ "Sequencer", "SEQ", ..., 6, 150, 80 }`, compressed by the R3 clamp at the 1024 floor)
- severity: medium
- class: touch
- evidence: the prior audit's T9 was STOPPED pending a product decision (documented in audit/IPAD_TOUCH_TODO.md: "two 44×44 buttons cannot fit the 72×64 step-grid cell… options: redesign the grid… or popup-based length entry reusing the T7 44pt rows"). Nothing has changed since: the stepper still draws two ~32×20 buttons, the only per-step length control, with no alternative interaction. Re-reported so the open item is visible in this hunt's ledger (not a regression).
- deterministic_check: whichever remediation is chosen — (a) popup-entry: long-press/any-tap on the number opens a 44pt-row 1..16 popup (then pin via tools/editor_test.cpp: invoke the stepper's popup seam headlessly and assert 16 items at kPopupRowHeight); or (b) grid redesign: pin the new cell size in tests/ipad_hig_sizing_test.cpp.

## F-ios-touch-3: parked zoom trio is keyboard-focusable at (0,0,0,0) — Tab on a hardware keyboard can land focus on an invisible control and silently kill musical typing
- Source/PluginEditor.cpp:2424–2432 (`zoomInButton_/zoomOutButton_/zoomResetButton_` constructed + `addAndMakeVisible`d; resized() NEVER places them — the layout_minwidth_test documents them as parked at (0,0,0,0) "by design"); JUCE juce_Button.cpp:89 (`Button::Button` ctor: `setWantsKeyboardFocus (true)`); JUCE juce_FocusHelpers.h:58–63 (`findAllComponents` filters ONLY `isVisible() && isEnabled()` — NOT extent)
- severity: low (needs an attached hardware keyboard to manifest) but it is exactly the "invisible-but-focusable" class this lane was asked about (the W9-FOLDED controls are safe: they get `setVisible(false)` and are excluded from traversal; the zoom trio does not)
- class: lifecycle
- evidence: the three zoom buttons remain VISIBLE at zero extent, so they sit in the focus-traversal list. With an iPad hardware keyboard, Tab/arrow traversal can hand focus to an invisible button; keyboard focus then shows nothing (the comp paints nothing at 0×0) and — because KeyboardView is a SIBLING, not an ancestor — letter keys fall to `Button::keyPressed` (unhandled) → `ParvatiEditor::keyPressed` (Cmd-combos only) → dead: musical typing silently stops until the user re-taps the on-screen keyboard. Space/Return while focused would even fire the invisible button's onClick (zoom). NOTE the header comment at :2422 ("visible on every platform") is stale relative to the parked reality.
- deterministic_check: extend tools/editor_test.cpp: after constructing the editor, walk `juce::FocusTraverser().getDefaultComponent (editor)` and iterate `getNextComponent()` around the cycle; assert no focusable stop has zero width/height (`getWidth() <= 0 || getHeight() <= 0`). Red today (the zoom trio), green once they get `setVisible(false)` (their logic already lives in the "..." overflow popup — the same treatment the W9 folded controls get).

## F-ios-touch-4: external file drag-and-drop is dead code on iOS — JUCE's iOS peer implements no drop interactions
- Source/PluginEditor.cpp:4488–4511 (`ParvatiEditor::isInterestedInFileDrag` / `filesDropped` accept .PRO/.MUL/.parvati and call applyPatchFile); JUCE ~/JUCE/modules/juce_gui_basics/native/juce_UIViewComponentPeer_ios.mm — grep for `UIDropInteraction|UIDragInteraction|handleDragDrop|handleDragEnter` = 0 hits; the file-drop entry points (`ComponentPeer::handleDragDrop/…`, juce_ComponentPeer.cpp:478–571) are only driven by the macOS/Windows/Linux peers
- severity: medium (a missing expected-iPad interaction, not a malfunction)
- class: files
- evidence: iPad users expect to drag a patch from the Files app onto the plugin view. JUCE 9's iOS peer has no UIKit drag/drop wiring, so the editor's FileDragAndDropTarget overrides can never fire on iPadOS; the only path is the Load button → iOS document picker (works — juce_FileChooser_ios). Drag-OUT (presets to Files) is not implemented anywhere either. Remediation would need a native UIDropInteraction layer (or JUCE's iOS DnD support when available) — a project-level decision, flagged here for the ledger.
- deterministic_check: source-level gate: a grep-based check (tools/check_*.py style) asserting the iOS peer's capability, or an Xcode build + `xcrun simctl` device test once implemented. Headless pre-fix: `grep -c UIDropInteraction ~/JUCE/.../juce_UIViewComponentPeer_ios.mm` == 0 documents the limitation.

## F-ios-touch-5: knob dials are only conditionally ≥44pt and no test sweeps live knob sizes
- Source/PluginEditor.cpp:1043–1050 (`dial = juce::jmin (kKnobDiameterCap /*52*/, b.getWidth(), b.getHeight())` where b is the cell minus the 15+3pt label band); cellH is 64 for the standard groups (:1654/:1662/:1674) → dial = 46pt at natural cells — compliant
- severity: low
- class: touch
- evidence: the 52pt CAP is not the dial size; the dial is cell-constrained, so any group whose cells the R3 clamp compresses below ~62pt height yields <44pt dials. tests/ipad_hig_sizing_test.cpp pins named constants only — there is no live sweep asserting every placed knob ≥44 at the swept widths (layout_minwidth_test pins the HEADER strip only). Latent, consistent with the prior audit's scoping (it raised the EQ knob specifically), so this is a coverage gap note rather than a new defect.
- deterministic_check: extend tests/layout_minwidth_test.cpp's sweepWidth() (it already walks visible controls for positive extent) to additionally assert `w >= 44 && h >= 44` for visible Slider descendants inside the content area, with a documented allowlist for the (deliberate) musical keyboard keys and any <44 visual-with-44-band idioms.

## F-ios-touch-6: hardware-keyboard semantics with EDITOR_WANTS_KEYBOARD_FOCUS=TRUE — intended musical typing, but unhandled keys may not return to the host; combos (incl. the new FxTypeCombo override) consume all four arrow keys
- CMakeLists.txt:231 (`EDITOR_WANTS_KEYBOARD_FOCUS TRUE`); JUCE juce_audio_plugin_client_AU_1.mm:1700–1706 (EditorCompHolder takes `setWantsKeyboardFocus(true)` — the AUv3 view becomes a first responder and receives hardware keys, which is what enables KeyboardView musical typing); Source/ui/FxSlotCard.cpp:242–257 (W10b `keyPressed` handles up/down/left/right and `return true` even at list ends); Source/ui/KeyboardView.cpp:592–660 (musical typing: bare keys only, modifiers pass through, focusLost releases held notes)
- severity: low
- class: touch
- evidence: verified coherent BY DESIGN — KeyboardView grabs focus on its own mouseDown, releases notes on focusLost, never hijacks Cmd/Ctrl/Alt, and the editor only consumes Cmd/Ctrl combos. Residual platform questions that need a device: (a) whether unhandled key events inside the AUv3 first-responder view return to the host (AUM/GarageBand arrow-key navigation), (b) whether tapping the FX type combo (Desktop mouse-click-grabs-focus default TRUE) + arrows behaves acceptably on hardware keyboards (it changes the FX type with engagement seeding — correct, but discoverable only with the popup path pinned by editor_test [12d]/[12e], which DO cover the seam headlessly).
- deterministic_check: no headless check possible for host-key return (platform behaviour); [12d]/[12e] already pin the combo seam. Recommend a device-test checklist entry rather than a test.

---

## Verified good (no report; evidence checked this lane)
- **W9 fold/unfold symmetry under LIVE resize (grow AND shrink)**: resized() sets `setVisible(false)` AND skips setBounds when folded, and re-`setVisible(true)`+setBounds when unfolded — symmetric in both directions (PluginEditor.cpp:4015–4131). layout_minwidth_test sweeps ASCENDING widths 560→1800 (:431–433) over the real resized() pass, so growth-reappearance is pinned; primary controls never fold; folded controls are `!isShowing()` so they are excluded from focus traversal (no invisible-but-focusable from the FOLD path).
- **The "..." overflow popup re-evaluates breakpoints at click time** (PluginEditor.cpp:2450–2486): a resize between layout and click cannot desync the menu; a resize WHILE the menu is open leaves stale sections whose items still drive the real seams (`showTopPage`, `triggerClick` on hidden buttons — works via postCommandMessage, juce_Button.cpp:359–362; `partCombo_.setSelectedId(1..6)` matches the combo's 1-based ids) — benign, documented.
- **Popup menus while the editor dies**: W11's `juce::PopupMenu::dismissAllActiveMenus()` FIRST in ~ParvatiEditor closes every open menu before the L&F members they borrowed can dangle (F-ui-4) — this retroactively covers the editor L&F pointer held by the zoom popup, ParamControl context menu and FxTypeCombo picker.
- **KeyboardView touch core**: per-source multitouch map with cross-source dedup (mouseDownOnKey :98–114, glissando retarget :120–147, per-source stuck-note guards :149–172 incl. the off-key mouseUp fallback), integer key grid (getKeyPosition :178–186) keeping hit-testing == drawing, focus-lost releases held computer notes (:676–680), teardown releases every held source (:580–586). touchesCancelled delivers upAndCancel + a second up (JUCE peer :927–932) — the per-source release is idempotent, no double-off escapes (fireNoteCallback on a released source is a no-op path).
- **Predicted/coalesced touches and Apple Pencil**: the JUCE iOS peer's touchesMoved uses the raw event touches (no `coalescedTouches`/predictedTouch delivery) — no ghost keys from Pencil prediction; pen arrives as its own MouseInputSource, which the per-source map handles. Palm rejection is NOT possible through JUCE (no touch majorRadius exposure) — platform limitation, noted for the device checklist.
- **ParamControl long-press vs scroll**: arm-on-timer, OPEN-on-release (PluginEditor.cpp:1186–1300) — a menu never opens mid-slider-drag; >5px drag cancels a pending OR armed press (T10 unified slop); two-finger guard cancels if any other source is dragging (T11); combos never arm (they own their popup). Context menu popup rows are 44pt (L&F getIdealPopupMenuItemSize, ParvatiLookAndFeel.cpp:253–269 — consulted for every menu without an explicit height, i.e. PresetBrowser's nested Factory▸Bank tree).
- **Internal drag-and-drop on touch**: mod-matrix grips are 44pt (FxMatrixView.cpp:564, ModMatrixView same), GroupPager tab drag vs click suppression is guarded (T13 fix re-verified at Source/ui/GroupPager.cpp:60–97).
- **Safe-area trim (W9 edge-aware)**: display-point comparison with 4pt tolerance, per-side trim only for spanned edges, zoom-corrected via the global scale divisor (PluginEditor.cpp:3906–3942); zoom == Desktop global scale on iOS is supported (juce_Windowing_ios.mm:640,718,816–825).
- **44pt coverage confirmed in**: header icon strip/combos-with-44pt-band (ParamControl combos PluginEditor.cpp:1063–1073), PatchPage rows (PatchPage.cpp:182–226), SettingsPanel combo/toggle rows (:233–239), TuningEditor rows (kRowHeight=44), MulExportDialog buttons (130×~52), FxMatrixView rows (mute 44 / clear ≥44 / depth slider ~44 tall on the 48pt row), FxRoutingBar steppers + EQ knobs (pinned).

Count: 6 findings — 0 high, 3 medium (F-ios-touch-1, -2, -4), 3 low (F-ios-touch-3, -5, -6).
TOP 3 remediation priorities:
1. F-ios-touch-1 — grow the CentralModBar nav hit bands to 44pt (glyph can stay 30): small, mechanical, fits the existing kPowerHitSize/T7 idiom, and extend the HIG test to pin it.
2. F-ios-touch-2 — close the stopped T9a decision: popup-based 1..16 length entry (reuses the 44pt L&F rows) is the lower-risk option vs the grid redesign; pin with a headless editor_test seam either way.
3. F-ios-touch-3 — `setVisible(false)` the parked zoom trio (their logic already lives in the "..." overflow popup) + add the focus-traversal zero-extent walk to editor_test so the whole class stays closed.

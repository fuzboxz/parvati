# Parvati Editor/Workflow QoL Audit (JUCE 9.0.1, verified ~/JUCE)

## 1. Zoom — per-editor transform vs. global scale
Current: `PluginEditor.cpp:3867` `setZoom()` uses `Desktop::setGlobalScaleFactor` (process-wide; limitation documented `PluginEditor.h:539-548`). JUCE alternative: `Component::setTransform` (`~/JUCE/modules/juce_gui_basics/components/juce_Component.h:255-274`) — mouse coords auto-remap; doc warns transforms are ignored on desktop windows (not the plugin-embedded case) and demands getLocalPoint/getLocalArea for all coordinate math.

Plugin-code audit for transform safety:
- **Breakage risk (must fix)**: `PluginEditor.cpp:4064` `peer->localToGlobal (getLocalBounds())` (iOS safe-area logic) assumes editor local == peer coords; with a transform on the editor the peer math skips it. Would need `getScreenBounds()` instead. Severity: blocker for the change, trivial fix.
- Safe: `PluginEditor.cpp:1220,1241` ParamControl long-press `e.getScreenPosition()` — MouseEvent converts through the transform chain. Safe: `PluginEditor.cpp:1102` `getEventRelativeTo(this)` + `getLocalBounds()` (mouseEnter/Exit, PluginEditor.cpp:3589 same). No `getScreenBounds` calls exist in Source/; KeyboardView/FxRoutingBar use local coords only.
- Pattern needed: `setSize(base*z)` + `setTransform(scale(z))`, and scale `setResizeLimits` (`PluginEditor.cpp:3186`) by z. Note: on iOS AUv3 the editor is wrapped in a host view (not a desktop window) so transform applies; iOS Standalone editor is peer content — verify there (desktop-only zoom is already `#if !JUCE_IOS`, PluginEditor.cpp:3192-3195).
- Residual: drag-image positioning (`CentralModBar.cpp:288` `startDragging`) has known historical transform quirks; test mod-source drag under zoom.

## 2. Resizability — already done
`PluginEditor.cpp:3184-3186`: `setSize(1280,634); setResizable(true,true); setResizeLimits(1024,500,1800,1100)`. Reflow works via `ParamPage::reflowToWidth` (PluginEditor.h:290-310; driven from SynthWorkspace.cpp:204, FxWorkspace.cpp:192). No custom constrainer; no fixed aspect. Gap: size persistence is host-only; zoom is persisted (`PluginProcessor.h:107,115`) but editor W/H is not stored in plugin state — minor; hosts + JUCE Standalone (`juce_StandaloneFilterWindow.h`) persist window bounds themselves. QoL: add W/H to processor uiPrefs for cross-host consistency. Severity: low.

## 3. Preset/undo workflow
- `PresetBrowser.h` (whole file): hierarchical PopupMenu (Factory banks A/B/F/S + Multi, User recursive, Templates), mtime-cache, no search, no favorites, no prev/next API. QoL opportunities: (a) prev/next preset stepping (cached tree already holds ordered leaves — `cachedTree_`/`menuFromNode` at PresetBrowser.h:~420); (b) search-as-you-type via `TextEditor` in menu or a filterable list; (c) favorites (a persisted name list is enough, no file moves needed).
- `keyPressed` `PluginEditor.cpp:3892-3941`: Cmd/Ctrl `+/-/0` zoom, `Z/Shift+Z/Y` undo/redo only. Missing: prev/next preset (common: `[`/`]` or `Ctrl+←/→`), Load/Save (`Ctrl+O`/`Ctrl+S` → openLoadDialog/openSaveDialog at 4390/4430), part switch (`Ctrl+1..6` → partCombo_), Patch/Synth/FX page keys. All seams exist. Severity: medium (workflow).

## 4. Accessibility
Verified JUCE: `Button` handlers fall back to `getTooltip()` for the title (`~/JUCE/modules/juce_gui_basics/detail/juce_ButtonAccessibilityHandler.h:67-79`).
Present (good): KeyboardView (`KeyboardView.cpp:318-327,701-711`, role group), EnvelopeDisplay.cpp:242, OscPreviewDisplay.cpp:355, FilterResponseDisplay.cpp:215, FxSlotVisualizer.cpp:717, FxRoutingBar.cpp:233-235 (group).
Gaps (no `createAccessibilityHandler`/accessible name):
- `CentralModBar.cpp` pills (painted text; E1/L1/ARP…) — screen readers see nothing.
- `WheelsComponent.cpp` pitch/mod wheels — custom-drawn, unnamed.
- `GroupPager.cpp` sub-tabs; `ModMatrixView.cpp`/`FxMatrixView.cpp` rows; `FxSlotCard.cpp` power/bypass custom toggles.
- `IconButton.h:23` constructs `juce::Button({})` — empty name; saved only by the tooltip fallback (Undo/Redo/Settings tooltips set in `applyChromeTranslations`, PluginEditor.cpp:3950-3960). Recommend explicit `setTitle()`/`setDescription()` per icon. Severity: medium.

## 5. iOS / touch
- Viewports: SynthWorkspace.cpp:29,51 / FxWorkspace.cpp:32,48 / ModMatrixView.cpp:563 / FxMatrixView.cpp:620 / PatchPage.cpp:633 (vertical-only). JUCE default `ScrollOnDragMode::nonHover` (`juce_Viewport.h:299-312`) means touch-drag already scrolls; CentralModBar.cpp:370 explicitly sets `never` (nav pills instead). Gap: JUCE has **no momentum/fling** (no match in juce_Viewport.cpp) — a custom inertia layer or nested-scroll guidance would be the QoL win. Severity: low-medium.
- Keyboard multi-touch: implemented correctly per-source (`KeyboardView.cpp:86-107`, 154-177; `mouseDownNotesBySource_`); EDITOR_WANTS_KEYBOARD_FOCUS TRUE (CMakeLists.txt:228).
- Safe-area/zoom math divides by global scale (PluginEditor.cpp:4085) — would need updating under transform zoom (see §1).

## 6. Standalone
No `StandaloneFilterWindow`/custom standalone code in Source/ or CMake (grep: zero hits). Defaults from `~/JUCE/modules/juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h:731-743`: window titled with app name, standard Options gear + AudioDeviceSelector, autoOpenMidiDevices true, plugin window persisted. QoL candidates: custom window title ("Parvati — Synthesizer"), icon, DocumentWindow buttons, colour; needs a custom `StandaloneFilterWindow` subclass or `JUCE_DISABLE_STANDALONE...`+own main. Severity: low (cosmetic).

## 7. Other
- Tooltips: `TooltipWindow` hover-only (`PluginEditor.cpp:2336`); on iPad there is no hover — ParamControl help reaches touch users only via long-press menu item tooltips (`popupTooltipWindow_`, PluginEditor.h comment ~305). Consider first-long-press showing help, or keeping the status-strip hint on touch. `SettingsPanel` tooltip toggle exists.
- Double-click-to-default: `ParamControl::mouseDoubleClick` (PluginEditor.cpp:1109) is consumed by the mod-slot jump; default reset is right-click/long-press menu only. QoL: Alt/Cmd+click reset, or double-click resets when `aggregateAmount==0`.
- FileChooser: async + overwrite-warning save + failure alerts — good (`PluginEditor.cpp:4390-4460`); only one member `fileChooser_` (a second dialog overwrites the first — acceptable, minor).
- DragAndDropContainer is used for internal mod-source drags; preset drag-in works (`isInterestedInFileDrag`/`filesDropped`); no preset drag-out (JUCE internal DnD only) — not actionable in JUCE 9.

Priority: per-editor zoom (fix `peer->localToGlobal` first) > preset prev/next + keyboard shortcuts > a11y names for custom controls > preset search/favorites > momentum scroll > standalone chrome.

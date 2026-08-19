# Editor QoL Lane — Host Context Menus + Preset Stepping / Shortcuts

## 1. Host parameter context menus (PluginEditor.cpp `ParamControl::showContextMenu`)
Pattern: resolve the editor via **`findParentComponentOfClass<ParvatiEditor>()`** (NOT the old `getParentComponent()` dynamic_cast — ParamControls are parented to ParamPages, so that cast was a silent no-op; fixed the tooltip-hide with it). Then `ed->getHostContext()` → `getContextMenuForParameter(apvts.getParameter(desc_.paramID))` → `getEquivalentPopupMenu()` is taken as the **base menu**; our Reset/Randomize tooltip-bearing items are **appended below a separator** (host entries on top, where users expect them). Null host-context / empty host menu (AU/AUv3/standalone/headless) falls through to the unchanged local-only menu. Long-press (iOS) and right-click share the same function, so both get host entries for free. itemIDs moved to 1001/1002 (collision-proof against host-numbered entries; nothing consumes the ID). Popup tooltip-window behavior unchanged. API verified against `~/JUCE .../juce_AudioProcessorEditorHostContext.h` + `juce_AudioProcessorEditor.h:209/216` (VST3 wrapper calls `setHostContext`).

## 2. PresetBrowser step semantics
`selectNext()/selectPrev()` flatten the CACHED tree in **exact menu order** (submenus-then-leaves recursion, same as `menuFromNode`; subs 0..6 = Factory A/B/F/S, Multi, User-recursive, Templates), refresh-first if cache invalid/watched-dirs changed. **WRAP at the ends** (documented). Unanchored current file: next→first leaf, prev→last. Selection routes through a new `selectLeaf()` — the same `onSelect_` (load) seam a menu pick fires — and records `currentFile_`. `setCurrentFile()` lets the editor anchor out-of-menu loads; wired into `applyPatchFile` (covers menu picks, Load…, drag-drop, open-in; idempotent). Empty tree → invalid File → editor passes the key through. Button label stays owned by `applyPatchFile` (parsed .PRO name, not filename).

## 3. Shortcut table added (`ParvatiEditor::keyPressed`)
| Key | Action | Seam |
|---|---|---|
| `[` / `]` (plain **or** Cmd/Ctrl) | prev/next preset | `handleStepPresetShortcut` → `selectPrev/Next` |
| Cmd/Ctrl+O | Load picker | `handleLoadPresetShortcut` → `openLoadDialog` (desktop-gated) |
| Cmd/Ctrl+S | **Save .parvati** (full-fidelity; .PRO/.MUL stay on the Save button menu) | `handleSavePresetShortcut` → `openSaveParvatiDialog` (desktop-gated) |
| Cmd/Ctrl+1..6 | Part select | `partCombo_.setSelectedId(n, sendNotificationSync)` — the part-menu seam |

Plain `[`/`]` were verified unclaimed (KeyboardView musical typing = letters/z/x/c/v only, modifier combos pass through; ComboBox consumes nav keys only; focused TextEditor consumes them itself) — belt-and-braces `TextEditor` focus guard added. Zoom keys untouched. Helpers are small testable seams returning consumed/not; the Load/Save launch is **desktop-gated** (the `showFileOpFailure` headless idiom) so headless calls fire the seam without a picker. Load/Save button tooltips now advertise the shortcuts (FR/DE added to both tables; `check_translations.py` violations unchanged at the 3 pre-existing ones).

## 4. Tests (`tests/editor_test.cpp` §[18], runs in `parvati_multigui_test`)
(a) Deterministic temp-tree browser: step order A→Multi→User→Templates, wrap both ends, backward walk, onSelect fired per step, `setCurrentFile` anchoring, empty-tree invalid-File. (b) Real editor: `getHostContext()==nullptr` headless (the documented degradation), `keyPressed(']'/'['/Cmd+']')` consumed, Cmd+3 → engine part 2 (0-based), Cmd+7 passes through, Cmd+O/S consumed headless, Cmd+0 zoom still works.

## 5. Validation
`parvati_multigui_test` (the `tests/editor_test.cpp` target) ALL PASS incl. 25 new checks; `parvati_editor_test` (the `tools/` Phase-4 binary), `parvati_keyboard_view_test`, `parvati_modbar_click_test` PASS; `parvati_lifecycle_test` + `parvati_ipad_hig_sizing_test` PASS (Translations.cpp consumers). Builds hit two transient environment issues, not code: the sibling render lane's in-flight PluginProcessor edit (waited, then clean), and a stale `libparvati_factory_presets.a` needing a reconfigure + 2-pass regen.

## 6. Risks
- Host menus are VST3-only by JUCE design; AU/standalone unchanged (null path).
- `handleStepPresetShortcut` stepping from a REPO-path anchor (drag-drop of presets/FACTORY files) selects the first app-data leaf — deterministic, matches "not-in-list" semantics.
- No `PresetBrowser.h` regression risk to menu building: stepping reuses the identical traversal/scan code paths.

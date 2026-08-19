# Accessibility Names/Handlers — Custom-Drawn Controls (work_a11y)

All edits are accessibility-only (no painting/layout/behavior changes). 124 insertions / 2 deletions across 8 files. Every touched TU syntax-checks clean via `build/compile_commands.json` (`-fsyntax-only`) against the current tree, which already includes sibling lanes' in-flight changes.

## Per-component results

1. **IconButton.h** (`ui/IconButton.h`) — Added `static juce::String iconTitle (Icon)` mapping Undo/Redo/Gear → `TRANS("Undo"/"Redo"/"Settings")` (keys already exist in Translations.cpp, so FR/DE localize for free); `setTitle()` in ctor and `setIcon()`. JUCE-verified: `Button`'s default handler (`detail/juce_ButtonAccessibilityHandler.h:64-80`) reads `Component::getTitle()` first, falls back to `getButtonText()`, exposes `getTooltip()` as help, and free-attaches toggle-state + On/Off value on toggleable buttons. `setTitle` was chosen over ctor-button-text to leave `Component::getName()` untouched (PluginEditor sets names like "headerUndo"). Undo/Redo/Gear now covered without editing PluginEditor.

2. **CentralModBar.cpp — ModPill** — `setTitle(fullName_)` in ctor (ModSourceCatalog names are untranslated Ambika hardware terms per Translations.h policy — same string as the tooltip). New `createAccessibilityHandler()` override: `AccessibilityRole::button` + `AccessibilityActions().addAction(AccessibilityActionType::press, → owner_.invokeClicked(enumValue_))` — the press action routes the SAME callback a real click fires, so VoiceOver/switch-control activation selects the generator. Sentinel pills (enumValue_ < 0, e.g. Note Sequencer) take the same path as mouseUp — consistent with existing behavior.

3. **WheelsComponent.cpp** — Discovered the wheels are `juce::Slider`s: JUCE's built-in `Slider::createAccessibilityHandler` (`juce_Slider.cpp:1834`) already attaches a full **ranged numeric value interface** (-1..1 pitch, 0..1 mod, adjustable). No custom handler needed — added `setTitle`/`setDescription` (`TRANS("Pitch Wheel")`/`TRANS("Mod Wheel")`) to the two sliders. Names were the only missing piece (NoTextBox sliders expose no visible text).

4. **GroupPager.cpp — NO EDIT (verified already accessible)**. `DraggableTabButton : juce::TabBarButton : Button`, and `TabBarButton::TabBarButton(name, bar) : Button(name)` (`juce_TabbedButtonBar.cpp:38-42`) — so each sub-tab's visible text IS `getButtonText()`, which the default Button handler announces as the title. No override blocks it. Deliberate no-op.

5. **ModMatrixView.cpp — ModMatrixRow** — `setTitle(TRANS("Mod ") + N)` in ctor + `createAccessibilityHandler()` group-role override (matches the established EnvelopeDisplay/FxRoutingBar pattern). Rows announce as "Mod N, group" containers around their combos/slider/buttons.

6. **FxMatrixView.cpp — FxMatrixRow** — Same pattern: `setTitle(TRANS("FX Mod ") + N)` + group-role handler. (Transient compile damage from an edit-anchor collision was caught by clangd and repaired immediately; final TU is clean.)

7. **FxSlotCard** — Card: `setTitle("FX" + N)` (proper noun, matches the painted "FX1" header + FxRoutingBar's untranslated ids) + `createAccessibilityHandler()` group override (declared in `FxSlotCard.h`, defined beside the dtor). PowerToggle: `setTitle(TRANS("FX ") + N + TRANS(" enable / bypass"))` — toggle state + On/Off value come free from the Button default handler (the card already drives `setToggleState` from the `fx{N}_enabled` Value). TypeStepButton prev/next: `setTitle` with their existing localized tooltip chains. FxTypeCombo: `setTitle` with the "FX N algorithm" chain (ComboBox's built-in handler reads the title).

8. **Translations.cpp** — Added the 4 new suffix-key fragments ×FR/DE: "Pitch Wheel"/"Mod Wheel" (FR: Molette de pitch/modulation, DE: Pitchrad/Modulationsrad), "Mod " (identity both), "FX Mod " (FR "Mod FX ", DE "FX-Mod. "). All pre-existing keys reused unchanged.

## JUCE APIs verified against ~/JUCE (9.0.1)
- `AccessibilityHandler::getTitle()` → `Component::getTitle()`; `Component::setTitle` (`juce_Component.h:2532,2542`).
- Default `Component::createAccessibilityHandler()` returns an *unspecified*-role handler (`juce_Component.cpp:3307`) — plain components are visible but unnamed/role-less, which is why the group/button overrides matter.
- `ButtonAccessibilityHandler` title fallback/help/toggle-value (`juce_ButtonAccessibilityHandler.h:39-99`).
- `Slider::createAccessibilityHandler` built-in ranged value (`juce_Slider.cpp:1834`).
- `AccessibilityActions::addAction(AccessibilityActionType::press, cb)` (`juce_AccessibilityActions.h:97`); `press` enum verified at `:49`.
- `TabBarButton` inherits Button's handler with buttonText as title (`juce_TabbedButtonBar.cpp:38`).

## Deliberately skipped
- ModPill selected-state (`AccessibilityState::withChecked`): requires a custom handler class overriding `getCurrentState()`; judged beyond "name + role button is enough" per task.
- WheelDragLabel caption strips (PITCH/MOD drag sources): out of task scope (wheels themselves were the target); they remain visible-but-unspecified via the default handler.
- Any change to PluginEditor.*, PatchPage.*, engine/processor/layout files, CHANGELOG, docs, CMake — untouched (other lanes own them).

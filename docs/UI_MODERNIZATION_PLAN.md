# Parvati UI Modernization Plan

> **STATUS: COMPLETE (all phases 1–4c delivered).** Debug + release builds are
> clean; all 14 test targets pass; the standalone launches. See the
> "Completion Log" at the bottom of this file.

**Goal:** transform Parvati's UI from a functional but primitive generated
grid into a **modern, scalable, themeable, accessible** plugin GUI. Keep it a
faithful, descriptor-driven view of the engine. The UI does not emulate the
Ambika hardware UI (a 2×16 char LCD). It delivers a contemporary
software-synth experience.

---

## 1. Current State (what exists today)

| Area | Status |
|------|--------|
| **Architecture** | `ParvatiAudioProcessor` owns `SynthEngine` (96-voice pool: 6 multitimbral Parts x 16 slots each) + APVTS exposing ~104 params via a `PatchParamDescriptor` table. |
| **GUI generation** | `PluginEditor.cpp` auto-generates `ParamControl` cells (rotary `Slider`/`ComboBox`) into 9 `ParamPage`s + 1 custom `PatchPage`, wrapped in `Viewport`s in a `TabbedComponent`. It cannot drift from the engine (good — keep this). |
| **Colour** | One hard-coded `col::` palette (dark bg + gold accent). Every control gets ~8 manual `setColour()` calls. No theme system, no `LookAndFeel`. |
| **Layout** | Fixed-pixel grid (e.g. 214×106 cells). Resizable window, but the grid does **not** reflow/scale — controls get clipped or padded. |
| **Scaling** | None beyond JUCE default DPI. No user zoom. Fixed `setSize(980,660)`, `setResizeLimits(720,480,1600,1100)`. |
| **Tooltips** | None. `PatchParamDescriptor` carries no help text. |
| **Controls** | Stock `juce::Slider` (rotary) + `juce::ComboBox` + `juce::Label` + `juce::ToggleButton`. No custom drawing, no value popups, no right-click menus, no double-click-to-default. |
| **Visualization** | None (no keyboard, no scope, no envelope display, no meter). |
| **UX/Accessibility** | No keyboard focus management, no `AccessibilityHandler` attention, no context menus. |
| **Persistence** | Patch/multi load + state save/load. No UI prefs (theme/zoom) persisted. |
| **Tests** | 18 test exes incl. `parvati_multigui_test` (editor builds + Part selector). Must stay passing. |

---

## 2. Gap Analysis (what a modern GUI needs)

### A. Foundation / infrastructure
1. **LookAndFeel layer** — one `ParvatiLookAndFeel` that centralizes all
   widget drawing.
2. **Theme system** — `ParvatiTheme` palette struct + `ThemeManager` (multiple
   named themes, runtime switch, persisted).
3. **UI scaling** — user zoom (75–200%) + correct HiDPI, applied uniformly.
4. **Tooltip / help system** — per-param help text + `TooltipClient` +
   `TooltipWindow`.

### B. Layout / responsiveness
5. **Responsive grid** — reflow columns to width (replace the fixed-pixel
   grid).
6. **Grouped panels** — related controls in bordered sub-panels with
   headings (e.g. "Osc 1", "Osc 2"), derived from param-ID prefixes.
7. **Scalable typography** — font sizes scale with zoom.

### C. Modern controls / widgets
8. **Improved knob** — vector arc, bipolar support (mod amounts),
   double-click reset, Shift = fine drag, value popup, velocity-sensitive
   drag.
9. **Consistent ComboBox** styling.
10. **Segmented toggles** for small enums (filter mode LP/BP/HP/Notch;
    on/off).
11. **Styled toggles/switches.**

### D. Visualization ("modern experience")
12. **Envelope display** — ADSR shape that reacts to the A/D/S/R knobs.
13. **Virtual keyboard** — `MidiKeyboardComponent` that shows/triggers notes,
    per-part channel colour.
14. **Voice activity meter** — per-part allocator state (one cell per
    allocated voice, up to 16; the former global 96-voice pool view on the
    Patch page was removed — the per-part Voices rows now show the
    allocation).
15. **Step-sequencer grid editor** — replace 32 raw sliders with a 16-step
    grid.
16. **Oscilloscope/spectrum** (lower priority).

### E. UX / interaction
17. **Right-click context menu** — reset to default, randomize, copy/paste
    value, MIDI-learn placeholder.
18. **Keyboard navigation & visible focus ring.**
19. **Accessibility** — JUCE 9 `AccessibilityHandler` per control.
20. **Value-change visual feedback.**

### F. Persistence / settings
21. **Settings panel** — theme, zoom, tooltips on/off — in a side panel;
    persisted in processor state.

---

## 3. Target Architecture

### New files (under `Source/ui/`)
- `ParvatiTheme.h/.cpp` — palette struct + built-in themes.
- `ThemeManager.h/.cpp` — owns themes, current index; `ChangeBroadcaster`;
  persistence helpers.
- `ParvatiLookAndFeel.h/.cpp` — `LookAndFeel_V4` subclass that draws
  everything from the active theme.
- `ParamHelp.h` — `paramID → description` map (tooltips). Keeps the
  byte-bridge `PatchParamDescriptor` pure.
- `ParamControl`, `ParamPage` — move from `PluginEditor.*` into their own
  files (optional refactor; see note).
- `EnvelopeDisplay.h/.cpp` — ADSR preview component.
- `KeyboardComponent` wrapper — `MidiKeyboardComponent` fed by a
  `MidiKeyboardState` that the engine populates.
- `VoiceMeter.h/.cpp` — active-voice indicator.
- `SettingsPanel.h/.cpp` — theme/zoom/tooltips controls.

### Theme system
```cpp
struct ParvatiTheme {
    juce::String name;
    juce::Colour windowBackground, panelBackground, panelBackground2, panelHeader;
    juce::Colour outline, divider;
    juce::Colour accent, accent2;          // primary/secondary
    juce::Colour text, textDim, textValue;
    juce::Colour knobArc, knobTrack, knobMod; // bipolar/mod overlay
    bool isDark = true;
};
```
Built-in themes: **Carbon** (current dark/gold), **Midnight** (dark
blue/teal), **Obsidian** (near-black/violet), **Paper** (light), **Crimson**
(dark/red).

### Scaling approach
Use `juce::Desktop::getInstance().setGlobalScaleFactor(zoom)` for the
user-facing zoom (it scales fonts, controls, hit areas uniformly; the
standard approach for JUCE plugin zoom controls). Rely on JUCE's native
per-display DPI for HiDPI. Default zoom 1.0; persisted.

### Layout approach
`ParamPage` computes `columns = clamp(width / minCellWidth, 1, maxCols)` and
lays cells with `FlexBox` (row wrap), so the page reflows inside its
`Viewport`. Sub-groups (`osc1_*`, `osc2_*`, `mix_*`…) render as bordered
`GroupComponent` panels that contain their cells.

---

## 4. Work Breakdown & File Ownership (for subagents)

| Phase | Worker | Files owned (may create/edit) | Depends on |
|-------|--------|-------------------------------|------------|
| **1a** | W-theme | `Source/ui/ParvatiTheme.*`, `Source/ui/ThemeManager.*` (new only) | — |
| **1b** | W-help | `Source/ui/ParamHelp.h` (new only) | — |
| **2** | W-core | `Source/ui/ParvatiLookAndFeel.*` (new) + rewrite `Source/PluginEditor.*` to use theme + L&F + global scale + `TooltipWindow` + responsive FlexBox layout + grouped panels | 1a, 1b |
| **3a** | W-env | `Source/ui/EnvelopeDisplay.*` (new) | 2 |
| **3b** | W-kbd | `Source/ui/KeyboardView.*` (new) | 2 |
| **3c** | W-meter | `Source/ui/VoiceMeter.*` (new) | 2 |
| **3d** | W-settings | `Source/ui/SettingsPanel.*` (new) + processor-state persistence of theme/zoom | 1a |
| **4** | W-polish | `PluginEditor.*` (single writer): wire in 3a–3d, right-click menus, keyboard nav, accessibility pass, fill all tooltip text, theme color balance, default-size/limits review | 3a–3d |
| **review** | reviewer | read-only reviews between phases | each phase |

**Safety rule:** phases 1a/1b are parallel (disjoint new files). Phase 2 is a
single writer (it owns `PluginEditor.*` + `ParvatiLookAndFeel.*`). Phase 3
workers own only their new files (no `PluginEditor` edits), so they can run
in parallel. The parent (orchestrator) integrates them, or phase-4 W-polish
integrates. This keeps one writer per shared file at all times.

---

## 5. Sequencing

1. **Plan lock** (this doc).
2. **Phase 1** (parallel): theme system + help metadata (isolated new files).
3. **Phase 2** (single writer): core rewrite — L&F, theme wiring, scale,
   tooltips, responsive grouped layout. Reviewer. Build +
   `parvati_multigui_test`.
4. **Phase 3** (parallel, new files): env display, keyboard, voice meter,
   settings/persistence. Parent integrates.
5. **Phase 4** (single writer): polish — context menus, accessibility, full
   tooltip text, color tuning, final size policy. Reviewer.
6. **Final gate:** full `cmake --build build` + all test exes pass + the
   standalone launches.

---

## 6. Baked-in Defaults (override if you disagree)
- **Themes:** Carbon, Midnight, Obsidian, Paper, Crimson (Carbon = default =
  the current look, preserved).
- **Zoom:** 0.75–2.0, default 1.0, slider in settings panel + keyboard
  `+`/`-`.
- **Visualization priority:** virtual keyboard + envelope display + voice
  meter (high impact, moderate cost). Scope/spectrum = optional later.
- **Grouping:** derived from param-ID prefixes (`osc1_`, `osc2_`, `mix_`,
  `filter1_`, …) → bordered panels; no change to `PatchParamDescriptor`.
- **Tooltips:** separate `ParamHelp.h` map (keeps the byte-bridge pure); show
  name + current value + range + description.
- **Keep generated:** all ~104 params stay auto-generated from the descriptor
  table.

---

## 7. Constraints
- Never break the `PatchParamDescriptor` ↔ APVTS ↔ engine byte-bridge (the
  GUI stays generated).
- Never edit `ambika_reference/` (read-only).
- Keep all 18 test executables passing; `parvati_multigui_test` must pass
  after every phase.
- One writer per shared file per phase.
- C++17, JUCE 9.0.0, CMake glob picks up new `Source/ui/*` files
  automatically.

---

## Completion Log (Phases 1–4c)

All delivered and verified (debug + release builds clean, 14/14 tests pass,
the standalone launches).

| Phase | Delivered | New/edited files |
|-------|-----------|------------------|
| **1a** Theme system | `ParvatiTheme` (5 themes: Carbon/Midnight/Obsidian/Paper/Crimson), `ThemeManager` (ChangeBroadcaster, name/index select, ValueTree persistence) | `Source/ui/ParvatiTheme.{h,cpp}`, `Source/ui/ThemeManager.{h,cpp}` |
| **1b** Tooltips | `ParamHelp` map — **183/183 params covered** (119 curated + 64 generated seq steps, runtime-verified) | `Source/ui/ParamHelp.{h,cpp}` |
| **2a** LookAndFeel + wiring | `ParvatiLookAndFeel` (centralized, drives all stock widget colours from theme); removed legacy `col::`; `TooltipWindow`; `setZoom`; `ChangeListener` theme refresh | `Source/ui/ParvatiLookAndFeel.{h,cpp}`, `Source/PluginEditor.{h,cpp}` |
| **2b** Responsive layout | Grouped panels (`groupForId` → bordered `GroupComponent`s); FlexBox-style reflow to window width; dense Mod-Matrix/Sequencer handling; viewport reflow contract | `Source/PluginEditor.{h,cpp}` |
| **3** Visualization components | `EnvelopeDisplay` (ADSR preview), `KeyboardView` (engine-mirrored + click-play), `VoiceMeter` (part-relative activity, up to 16 cells) | `Source/ui/{EnvelopeDisplay,KeyboardView,VoiceMeter}.{h,cpp}` |
| **4a** Integration + settings + persistence | Keyboard + meter wired in; `SettingsPanel` (theme/zoom/tooltips) in a `SidePanel`; thread-safe `MidiMessageCollector` click-play; backward-compatible UI-pref persistence in processor state | `Source/PluginProcessor.{h,cpp}`, `Source/PluginEditor.{h,cpp}`, `Source/ui/SettingsPanel.{h,cpp}` |
| **4b** Polish (reviewer fixes + UX) | Atomic voice-activity snapshot (removes data race); zoom leak fixed (reset on teardown) + documented; zoom keyboard shortcuts (Cmd/Ctrl +/−/0); right-click context menus (reset-to-default / randomize); dead-code NITs fixed | `Source/AmbikaVoice.{h,cpp}`, `Source/PluginEditor.{h,cpp}`, `Source/ui/KeyboardView.{h,cpp}`, `Source/ui/SettingsPanel.{h,cpp}` |
| **4c** Env displays + QA | `setGroupDecoration` ParamPage hook; 3 ADSR previews on the Env/LFO page; theme-contrast QA (no changes needed) | `Source/PluginEditor.{h,cpp}` |

### How to use the new UI
- **Themes:** Settings panel (gear button, top-left) → Theme combo. 5 themes.
  Persisted.
- **Zoom:** Settings → Zoom slider, or **Cmd/Ctrl + / −** (step 10%),
  **Cmd/Ctrl + 0** to reset (0.75×–2.0×). Persisted.
- **Tooltips:** hover any control for name+value+description (all 183
  params). Toggle in Settings. Persisted.
- **Right-click** any knob/combo → Reset to default / Randomize.
- **Virtual keyboard** (bottom): click to play (routes to the current part's
  MIDI channel); it shows the sounding notes from the engine.
- **Voice meter** (status strip): part-relative — one cell per allocated
  voice of the current part (up to 16; the pool itself is 96 voices across 6
  parts) + active count.

### Known limitations / future enhancements (documented, not blocking)
- **Zoom is process-global** (JUCE `Desktop::setGlobalScaleFactor`): multiple
  Parvati instances in one host share one zoom. The zoom resets to 1.0 on
  editor teardown (no leak). Per-editor transform-based zoom is the
  documented future enhancement (it would need changes to the reflow
  layout that keys off `getWidth()`).
- **Scope/spectrum analyzer** (plan gap D16) intentionally deferred — the
  keyboard + env display + voice meter had the highest priority.
- **MIDI-learn** is a placeholder in the right-click menu (not yet wired).

---

## QoL Audit & DSP-Settings Pass (post-modernization)

This pass came from an audit of the JUCE professional checklist, filtered for
Parvati's faithful-port architecture.

### Implemented
- **Preset Save** — `.PRO` export (byte-exact inverse of parser, round-trip
  verified); "Save…" button.
- **Undo/Redo** — `UndoManager` on the APVTS; knob drags, combos,
  context-menu reset/randomize, patch loads are all undoable; Cmd/Ctrl+Z /
  Shift+Z + Undo/Redo buttons.
- **Accessibility** — `AccessibilityHandler`s on the virtual keyboard, voice
  meter (live "N of M" where M is the current part's allocation), envelope
  displays (the 182 generated controls already had JUCE defaults).
- **Computer-keyboard play** — standalone-ONLY (in a DAW the host owns
  musical typing); modifier keys passthrough for zoom.
- **🐛 Byte-bridge truncation fix** — `parvatiValueToPatchByte` now
  `juce::roundToInt` (was truncating 62.9999→62). Init-neutral; it fixes
  load/live/save of float-roundtripped values.
- **HW-accelerated rendering** — already ON by default in JUCE 9 (Direct2D
  on Windows unconditional; CoreGraphics on macOS). No flag needed.
- **Latency/PDC reporting** — `setLatencySamples(round(2·hostRate/39216))`
  (Lagrange resampler algorithmic latency) + filter-OS latency when active.
- **Offline-render detection** — `setNonRealtime`/`isNonRealtimeRender()`
  scaffolding for a future max-quality mode.
- **Parameter smoothing** (opt-in, default OFF, bit-identical) — 20 ms linear
  per-sample ramp of cutoff+reso+VCA.
- **Filter oversampling** (opt-in 1×/2×/4×, default 1× = bit-identical) —
  oversamples the digital filter MODEL only; oscillators stay fixed-rate
  (39216 Hz) for authenticity. Min-phase ⇒ sub-0.15 ms added latency,
  reported via PDC.

### Confirmed NOT applicable (faithful-port reasoning)
Oscillator oversampling (fixed-rate authentic), SIMD-full-refactor (faithful
per-voice scalar), sidechain, ARA, Web UI, multilanguage-of-param-names
(mirror hardware), linear-phase by default.

### Remaining (Tier 3 — architecture)
Multi-output (stereo main + voicecard aux), MPE, SIMD where low-risk,
multilanguage (UI chrome).

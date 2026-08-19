# Tuning / Scala QoL Audit — Parvati (JUCE 9.0.1)

## 1. How tuning flows today
- **Import**: Only inside the TuningEditor popover — "Import .scl/.kbm…" opens a multi-select FileChooser starting at `userDocumentsDirectory` (`Source/ui/TuningEditor.cpp:172-181`); the callback enforces exactly one .scl + ≤1 .kbm, order-independent (`:181-234`), then `applyScalaText` (`:317-334`) runs `parvati::importScala`, fills rows, shows warnings inline, applies live.
- **Manual editing**: 12 note-class rows (drag slider, ±127 units of 1/128 semitone, cents readout, mute sentinel "—"); every edit calls `applyTable()` → `SynthEngine::setPartTuningCustom` (`Source/ui/TuningEditor.cpp:289-315`; engine at `Source/SynthEngine.cpp:1023-1056`).
- **Entry point**: Patch page Tune combo id 34 "Custom…" (`Source/ui/PatchPage.cpp:415, 476-482`) → `openTuningEditor` (`PatchPage.cpp:754-766`).
- **Per-part, never global**: `Part::customTuning` 12×int16 LE + `customTuningActive` (`Source/SynthEngine.h:195-211`); resolved mode = byte-4 raga preset wins, else custom flag → 33 (`SynthEngine.cpp:1016-1021`).
- **Persistence**: host state via engine blob v7 tuning block (`SynthEngine.cpp:462+`, restore ~`:671-673, :790`); `.parvati` via `tuning_mode`/`tuning_offsets` (`Source/ParvatiPreset.cpp:638-657` write, `:909-962` read, asymmetry clear `:949-961`); `.MUL`/`.PRO` carry only the raga byte — loaders clear custom when byte 4 == 0 (`PluginProcessor.cpp:1073-1080, 1204-1212, 1510-1522`); `part_raga=0` clears custom (`PluginProcessor.cpp:692-707`); .MUL export warns of loss (`Source/ui/MulExportDialog.cpp:218-240`).
- **Drag-drop**: editor-wide target accepts only `.pro/.mul/.parvati` (`Source/PluginEditor.cpp:4661-4667`); `filesDropped` applies the first match and `break`s (`:4669-4680`) — a mixed drop silently ignores .scl/.kbm.
- **iOS**: open-in parks .scl/.kbm in `<group>/Parvati/Tuning`, no auto-apply by design (`Source/ui/IosOpenIn.h:100-121`; `PluginProcessor.cpp:236-252`).

## 2. QoL gaps + recommendations

**G1 (High) — No drag-drop for .scl/.kbm.** `PluginEditor.cpp:4661-4666` rejects them. Sketch: accept `.scl/.kbm` in `isInterestedInFileDrag`; in `filesDropped`, collect them and route to a new `applyTuningFiles(files, engine, currentPart)`: read contents, reuse the pairing/validation from `TuningEditor.cpp:186-234` (extract into a shared static helper, e.g. `parvati::pairScalaFiles`), call `importScala` + `setPartTuningCustom`. Surface errors/warnings via the existing non-blocking `showFileOpFailure` pattern (`PluginEditor.cpp:4657`); if the TuningEditor popover is open, forward to its `applyScalaText` instead. Add a headless test driving `filesDropped` (it's public for exactly that — `PluginEditor.h:611-618`).

**G2 (High) — Multi-file drop pair handling.** `filesDropped` breaks on first file (`PluginEditor.cpp:4677`); the .scl+.kbm pair logic only exists in the chooser. Covered by G1's helper; don't `break` on scl/kbm — process the pair atomically (error on 2× .scl, mirroring `badSelection`, `TuningEditor.cpp:196-228`).

**G3 (Med) — Apply-tuning-to-all-parts.** Nothing today. Sketch: footer button "Apply to all parts" next to Clear/Done (`TuningEditor.cpp:379-387` footer layout) looping `for p in 0..5 engine_.setPartTuningCustom(p, table)` (clamping + D4 byte-4 clearing are internal, `SynthEngine.cpp:1023-1056`); also a modifier on the G1 drop path. Keep storage per-part — do NOT introduce a global table (breaks part-faithful semantics).

**G4 (Med) — Scale name never surfaced.** `SclFile` drops line 1 (`ScalaImport.cpp:107-111, 118`); `ScalaImportResult` has no name (`ScalaImport.h:88-95`). Sketch: add `juce::String name`, set from `lines[0]`; show in the TuningEditor message band and G1's toast. Display-only — hardware files can't carry it, so don't persist.

**G5 (Med) — Warnings only visible inside the popover.** Rich warnings exist (`ScalaImport.cpp:415-428`: quantization, clamps, mutes, subset mapping) but render only in `messageLabel_` (`TuningEditor.cpp:317-334`, fixed 64px band). G1's drop path must re-surface them; optionally also the status/tooltip bar (`PluginEditor.cpp:3624-3626` precedent).

**G6 (Low) — FileChooser start dir ≠ iOS parked dir.** Chooser starts at Documents (`TuningEditor.cpp:174`) but open-in files park in `Parvati/Tuning` (`IosOpenIn.h:100-104`). Sketch: start in the app-group Tuning dir when it exists.

**G7 (Info) — .tun support: recommend against.** No .tun code anywhere; iOS plist registers only scl/kbm. `.tun` expresses absolute full-range per-note tables — incompatible with the octave-repeating ±1-semitone 12-class contract; most real .tun files would clamp or fail. If ever added, convert through the same rejection paths with mandatory warnings; keep it out of the drop accept list meanwhile.

**G8 (Med) — On-screen keyboard not tuning-aware.** KeyboardView wraps `MidiKeyboardComponent` with custom `drawWhiteNote/drawBlackNote` (`Source/ui/KeyboardView.cpp:207+`) and knows nothing of tuning: muted classes look playable, detuned keys look normal. Sketch: give KeyboardView a 12-entry snapshot (offsets + muted mask via `engine.resolveTuningOffsets(currentPart, …)`, `SynthEngine.h:589-597`); in the draw overrides dim/hatch muted classes (optionally a cents marker). Editor pushes it from `tuningEditorApplied`/display-version poll (`PluginEditor.cpp:3790-3793`). Display-only — the engine already enforces AcceptNote.

## 3. Risks / constraints (contracts — do not break)
- **±127 clamp & 1/128-semitone quantization**: enforced at every writer (`ScalaImport.cpp:412-419`; `SynthEngine.cpp:1044-1046`; `TuningEditor.cpp:306-308`; `ParvatiPreset.cpp:924-933`). Any new path must clamp identically and surface the clamp warning (G5).
- **Octave-repeating only**: non-2/1 formal octave scales are REJECTED, not approximated (`ScalaImport.h` header contract). Applies to G7 and any future format.
- **32767 mute sentinel** must pass verbatim end-to-end (firmware AcceptNote semantics; `SynthEngine.h:590-597`, `TuningTables.h:24`).
- **D4 invariant**: custom active ⇒ raga byte 0 (`SynthEngine.h:196-201`; `ParvatiPreset.cpp:935`). G3's loop must go through `setPartTuningCustom`; a raw byte-4 write would shadow the custom table.
- **Thread-safety**: all UI writes are message-thread; drop handlers run there too — no new races. Respect TuningEditor's launch-parent lifetime guards (`TuningEditor.h:86-102`, `.cpp:289-299`) if G1 forwards drops into an open popover.
- **.MUL/.PRO lossiness**: apply-to-all multiplies the D14 export warning surface — already handled at `MulExportDialog.cpp:218-240`; no change needed.

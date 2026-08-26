# Custom-Tuning (Scala) Subsystem Removal — Work Report

## Deleted files
- `Source/ScalaImport.h/.cpp` — .scl/.kbm parser/converter
- `Source/ui/TuningEditor.h/.cpp` — custom-tuning popover
- `tests/scala_import_test.cpp` + its CMake `parvati_scala_import_test` block

## Changed files (this lane only)
- **SynthEngine.h/.cpp** — removed `Part::customTuning`/`customTuningActive`/`tuningDirty_`, `setPartTuningCustom`/`clearPartTuningCustom`, the custom branches of `resolvedTuningMode`/`resolveTuningOffsets`, and the `tuningDirty_` AT service loop. `resolvedTuningMode` is now simply `partBytes[4]`. Kept: TuningTables, raga resolution, the 32767 sentinel + `isNoteAcceptedByPartTuning`, `pushTuningToVoices` (byte-4 edits ride `frameDirty_`).
- **PluginProcessor.cpp** — removed the `part_raga=0` custom-flag clear, the .PRO/.MUL/.parvati loader backstop clears, and the open-in `.scl/.kbm` comment/branch. `tuning_mode`/`tuning_offsets` stay in the `.parvati` part-recognition key list so legacy files still parse as valid.
- **HellcatPreset.cpp** — serializer no longer emits `tuning_mode`/`tuning_offsets` (raga rides `params: part_raga`). Loader accepts legacy keys: 1..32 → raga byte; 0/33 → 12-EDO. Dropped the `TuningTables.h` include.
- **PatchPage.h/.cpp** — removed "Custom…" (id 34), `openTuningEditor`, `tuningEditorApplied`, TuningEditor include; kept `chooseTuningMode`/`getDisplayedTuningMode`/`syncTuningDisplay` (raga-only, ids 1..33).
- **PluginEditor.cpp** — dropped the `customTuning` flags array + dialog arg; removed the `<array>` include.
- **MulExportDialog.h/.cpp** — removed `customTuningParts` ctor/launch param, member, and the D14 warning block (+ 2 translation lines, Translations.cpp; TuningEditor comment refs reworded).
- **ParamHelp.cpp** — part_tuning/part_raga help no longer mention custom/Scala.
- **IosOpenIn.h** — removed `OpenInKind::Tuning`, the `.scl/.kbm` branch, and the Parvati/Tuning parking.
- **ios/parvati_filetypes.plist** — removed the 2 Scala doc types + 2 UTIs (5→3 UTIs); **CMakeLists.txt** PlistBuddy verify updated to `UTExportedTypeDeclarations:2 == com.805labs.parvati-multi`.
- **CMakeLists.txt** — also: scala test target removed, tuning-test comment updated, iOS open-in + shadow-state comments updated.
- **tools/trans_allowlist.txt**, **tools/check_async_this_allowlist.txt** — removed entries for deleted strings/files.

## Backcompat decisions
- **Engine blob**: capture bumped **v7 → v8** (no tuning block). Restore accepts 1..8; a **v7 tuning block is parsed + size-checked but ignored** (truncated/foreign still REJECTED, never mis-parsed). Legacy custom mode 33 restores as **12-EDO** (its raga byte was 0 by the custom-active invariant); real presets ride `partBytes[4]` unchanged. v6 and older unaffected. v9+ still strictly rejected.
- **.parvati**: old files with `tuning_mode: 33` + `tuning_offsets` load as 12-EDO (offsets parsed-and-ignored, never fail); `tuning_mode: 1..32` maps to the raga byte, applied after params (authoritative, as before).
- **.PRO/.MUL/host state**: raga byte path byte-identical; nothing else carried tuning.

## Tests updated
- `tuning_test.cpp` — rewritten raga-only; NEW v7-blob backcompat case (synthesizes a real v7 layout with mode-33 tail; asserts restore succeeds, presets intact, custom dropped to zeros) + v6/v9 views; legacy `tuning_mode:33`→12-EDO and `:5`→raga-5 `.parvati` acceptance; serializer emits NO tuning keys.
- `hellcat_preset_test.cpp` [6][7] — raga-is-whole-state + no-keys-emitted round-trip.
- `ios_openin_test.cpp` — [2] now asserts `.scl/.kbm` route INVALID, no Tuning dir created, nothing in USER; kind table rows → None.
- `shadow_state_test.cpp` / `loader_fuzz_test.cpp` / `roundtrip_golden_test.cpp` — custom pollution replaced with raga-byte pollution; fuzz `tuningMode999` mutator retargeted to an injected legacy key.
- `editor_test.cpp` — TuningEditor sections removed; preset/12-EDO combo paths kept.
- `load_invariants_test.cpp` — tuning-mode invariant 0..33 → 0..32; legacy-key corpus cases renamed.

## Validation
`clang++ -fsyntax-only` via real `build_relfp/compile_commands.json` flags: 0 errors in SynthEngine, PluginProcessor, PluginEditor, PatchPage, MulExportDialog, ParvatiPreset, ParameterLayout + all 9 touched tests. `plistlib` validates the plist (3 doc types, 3 UTIs). Full cmake build NOT run (per task constraints — orchestrator builds).

## Risks
- Other workers are concurrently editing PluginEditor.cpp/ParameterLayout.cpp (disjoint hunks; both syntax-clean).
- Pre-existing tool failures unchanged: `check_translations.py` (3 violations) and `check_async_this.py` (4 findings) fail identically on HEAD.
- iOS configure not verified (plist-graft PlistBuddy path needs a device/iOS configure; index arithmetic verified by hand: 3 UTIs → index 2 = parvati-multi).

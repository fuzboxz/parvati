# Bug-hunt lane: state & file-format integrity (2026-08-18, read-only audit)

Scope: Source/ParvatiPreset.{h,cpp}, Source/PatchFile.{h,cpp}, Source/MulExport.{h,cpp},
Source/ScalaImport.{h,cpp}, Source/TuningTables.{h,cpp}, PluginProcessor.cpp
(get/setStateInformation, program/part state), Source/MidiParameterMap.cpp, undo paths,
SynthEngine.cpp captureState/restoreState, tests/loader_fuzz_test.cpp,
tests/shadow_state_test.cpp (contract), tests/scala_import_test.cpp, tests/midi_param_test.cpp.
All findings verified against source; severity per taxonomy
(high = crash/corruption/data loss, medium = wrong load/save behavior, low = latent).

## F-state-1: parseParvatiYaml recursion is depth-unbounded — stack-exhaustion crash on a crafted .parvati
- file:line — Source/ParvatiPreset.cpp:136 (`ParseResult parseBlock (...)`, no depth limit),
  reached via Source/ParvatiPreset.cpp:264 (`parseParvatiYaml`), called from
  Source/PluginProcessor.cpp:1405-1411 (loadParvatiMultiFile pre-parse) and
  Source/PluginProcessor.cpp:1341-1348 (loadParvatiPatchFile pre-parse).
- severity: high (crash)
- evidence: `parseBlock` recurses once per indentation level (`ci = childIndent (i + 1, baseIndent);
  parseBlock (lines, i + 1, ci)`) with no depth counter or cap. Each nesting level consumes one
  input line, so a file of `a:\n  a:\n    a: ...` (2 extra bytes per level) drives ~1 recursion
  frame per 2 file bytes. Frame cost is ~150-300 B (ParseResult + juce::String locals), so
  ~4k-10k levels (an 8-20 KB file) overflows a 1 MB stack (iOS main thread / small message
  threads); ~20k-40k levels (40-80 KB) overflows an 8 MB desktop stack. Both load entry points
  run the pre-parse on the file's raw text BEFORE any `format:` sniff, so a `.parvati`-extension
  file with hostile nesting crashes the process during a user load. Uncovered by tests:
  loader_fuzz's structural YAML edits (`bogusYamlTail` etc.) are at most 1-2 levels deep; no
  deep-nesting case exists anywhere in tests/.
- deterministic_check: generate a text file `format: parvati-multi\nparts:\n` + N repetitions of
  `  a:\n`-style nesting for N in {1000, 5000, 20000} (indent +2 per line, ~4·N bytes total);
  call `loadParvatiMultiFile` (message thread pinned to a 512 KB-1 MB thread as an iOS proxy).
  Expected after fix: clean `false` return (depth-cap error) and no crash for every N; today the
  5000+ cases terminate the process (stack overflow) under ASan (`build_asan/parvati_*`).
  Add the case to tests/loader_fuzz_test.cpp's `multiEdits()` as a `deepNest` edit.

## F-state-2: .kbm parser reserves key vector from unvalidated S — multi-GB allocation / DoS
- file:line — Source/ScalaImport.cpp:252 (`f.keys.reserve ((size_t) juce::jmax (0, f.s));`),
  fed by Source/ScalaImport.cpp:224 (`f.s = t.getIntValue();` — isDigits-only validation, no
  upper bound); the `S != 12` gate lives far later in `importScala` (Source/ScalaImport.cpp:279).
- severity: medium (crash under memory pressure / app kill on iOS; wrong-load class)
- evidence: `parseScl` caps its count at 1..1024 (Source/ScalaImport.cpp:164-169:
  `if (f.n < 1 || f.n > 1024) return error`), but `parseKbm` has no symmetric cap. A hand-edited
  or hostile `.kbm` whose first token is e.g. `2000000000` passes `isDigits`, sets `f.s =
  INT_MAX`, and `keys.reserve` immediately attempts an ~8 GB allocation (vector<int>, 4 B/entry,
  < max_size so it reaches the allocator) BEFORE any semantic gate runs. On desktop this is a
  multi-GB transient spike (freed when the key-entry loop errors out); under pressure /
  iOS AUv3 it is std::bad_alloc — uncaught in `TuningEditor::applyScalaText`
  (Source/ui/TuningEditor.cpp:317-325 has no try/catch) — or a jetsam kill. The call chain is
  user-driven (Import .scl/.kbm…). Uncovered by tests: tests/scala_import_test.cpp only uses
  small S values (0, 5, 12, 13, 19 — lines 291-393); no huge-S case exists.
- deterministic_check: `importScala (kScl12tet, "! big\n 2000000000\n 0\n 11\n 60\n 60\n 261.6\n 12\n")`
  wrapped with an allocation watchdog (or run under the ASan allocator limit
  `ASAN_OPTIONS=allocator_may_return_null=1:max_allocation_size_mb=64`): after fix, expect
  `result.ok == false` with a "size out of range" error and peak allocation < 1 MB; today the
  process aborts on allocation failure. Mirror parseScl's cap (`if (f.s < 1 || f.s > 1024) error`).

## F-state-3: RIFF walker bounds math can wrap in 32-bit size_t (latent port hazard); chunk-header bytes never fuzzed
- file:line — Source/PatchFile.cpp:44 (`if (static_cast<size_t> (off) + 8u + csz > size) break;`)
  and Source/PatchFile.cpp:27 (`std::min<size_t> (size, static_cast<size_t> (8) + bodySize);`);
  consumer `trimName` at Source/PatchFile.cpp:90-96 walks `p[n-1]` backwards from `csz`.
- severity: low (latent — all current targets are 64-bit: macOS/iOS arm64/x64)
- evidence: on a 32-bit build (size_t 32-bit), `off + 8u + csz` wraps: with off=12 and a name
  chunk `csz = 0xFFFFFFF8` the sum wraps to 0x0C, the `> size` guard passes, and `trimName`
  reads `p[0xFFFFFFF7]` — wild OOB read (the `obj` branch is saved only because payloadLen ==
  112/84/56 exact-match gates the memcpys). `mbksBodyEnd`'s `8 + bodySize` can wrap the other
  way (end=7 → clean reject — benign but inconsistent). On 64-bit both are safe (verified: header
  read guarded by `off + 8 <= end <= size`; payload guarded against `size`; `off` advance can only
  grow). Coverage gap: loader_fuzz's bit flips are confined to payload regions (.PRO offsets
  48..159/175..187; .MUL routing bytes @48+ and PartData @104+i*220+124+3 — the RIFF size @4,
  chunk-size fields, and obj type prefixes are never mutated), so even the 64-bit clean-reject
  behavior for inflated `csz` is untested.
- deterministic_check: craft `RIFF` + LE32(0xFFFFFFF8) + `MBKS` + `name` + LE32(0xFFFFFFF0) +
  16 payload bytes → `parseAmbikaProgram` must return false; run under a 32-bit build with ASan
  (today: OOB read) and on 64-bit (today: clean false — pin it). Extend
  tests/loader_fuzz_test.cpp `runProBitFlips`/`runMulBitFlips` with a header-region sweep
  (offsets 4..7, 16..23, 36..47 and each obj's 8-byte header).

## F-state-4: .parvati multi serializer reads seqData without the bound guard the loader has
- file:line — Source/ParvatiPreset.cpp:429 (`return static_cast<float> (pc.seqData[(size_t)
  (d.byteOffset - 16)]);`) vs the guarded write path Source/ParvatiPreset.cpp:814
  (`else if (d->byteOffset >= 16 && d->byteOffset < 80)`).
- severity: low (latent — descriptor-table drift)
- evidence: `partRaw`'s sequencer branch indexes `seqData[byteOffset - 16]` for any isSequencer
  descriptor that is not one of the three by-ID `seq_length_*` params, with no range check. If a
  future seq descriptor carries byteOffset outside 16..79 (e.g. 15 or 80), serialization reads
  `seqData[-1]`/`seqData[64]` — OOB read at multi-save time (pendingConfig_::seqData is
  uint8_t[64], SynthEngine.h). Today the descriptor table only uses 12..14 (by-ID) and 16..63
  (steps, ParameterLayout.cpp:393-398+), so the paths are safe; the asymmetry with the guarded
  apply side is the fragility.
- deterministic_check: add a static/unit assertion in parvati_preset_test.cpp: for every
  descriptor d with `d.isSequencer`, assert `(d.byteOffset >= 16 && d.byteOffset < 80) ||
  paramID ∈ {seq_length_1,2,3}`; or add the same `if (d.byteOffset >= 16 && d.byteOffset < 80)`
  guard to partRaw and assert a descriptor with byteOffset 15 serializes as 0 instead of OOB
  (verified today only by absence of such a descriptor).

## F-state-5: applyParvatiMulti applies ANY APVTS paramID found under `options:` before part_select is reset
- file:line — Source/ParvatiPreset.cpp:964-967 (`for (const auto& p : oobj->getProperties())
  ... if (auto* param = proc.getApvts().getParameter (p.name.toString()))
  param->setValueNotifyingHost (param->convertTo0to1 ((float) p.value));`), executed before the
  part_select reset at Source/ParvatiPreset.cpp:970-973.
- severity: low (latent — hand-edited files only; violates the "file is the whole truth" contract)
- evidence: the serializer emits ONLY isOption params under `options:`
  (Source/ParvatiPreset.cpp:665-680), but the loader accepts a superset: any key matching an
  APVTS parameter ID is applied. Per-part params (e.g. `osc1_shape`) applied here route through
  parameterChanged → currentPart_, which is still the PRE-LOAD part (part_select is reset to
  Part 0 only afterwards at :970-977), so the value overwrites that part's just-loaded file
  bytes; `options: { part_select: n }` re-enters onPartSelect mid-load. Only `voice_mode` is
  filtered (:966); `part_select` (an isOption descriptor) is NOT. Contrast the `params:` path,
  which validates every key against the descriptor table and clamps to its range (:777-782).
- deterministic_check: set current part to 3, save a .parvati multi, hand-append `  osc1_shape: 5`
  under `options:`, load → assert `engine.getPart(3).patchBytes[0]` equals the FILE's part-3
  value (today: 5). After fix (restrict the loop to isOption descriptors excluding part_select),
  assert unknown/per-part keys under options: are ignored.

## F-state-6: restoreState commits the v6+ voice-slots byte without the 1..16 clamp
- file:line — Source/SynthEngine.cpp:760 (`part.voiceSlots.store (s.slots != 0 ? s.slots :
  (uint8_t) restoredSlots, std::memory_order_relaxed);`), fed by Source/SynthEngine.cpp:639.
- severity: low (corrupt/hand-edited host state only; graceful but wrong load)
- evidence: every other write path clamps — `setPartVoiceSlots` jlimits 1..kMaxVoicesPerPart(16)
  (SynthEngine.cpp:990), `setPartVoiceAllocation` derives popcount ≤ 6 (:971-973) — but the blob
  restore stores the raw byte. A corrupted v6+ blob (slots=255) makes rebuildVoiceAllocation
  grant that part the whole 96-voice pool (`n = jmin (want, kNumVoices - nextVoice)` — bounded,
  no OOB — SynthEngine.cpp:890) starving the other parts; no legit save path can produce
  slots > 16. Coverage gap: loader_fuzz section [4] only TRUNCATES the outer
  setStateInformation buffer — the base64 `engine_state` payload bytes are never bit-flipped,
  so inner-blob corruption (this byte, tuneMode > 33, channel > 16) is entirely untested.
- deterministic_check: capture a valid state (getStateInformation), locate the base64
  `engine_state` property, flip a bit in one part's slots byte (blob offset: 6 +
  p*(112+84+4+4+78+1+1+nameLen+4+25) + 202), setStateInformation → after fix assert
  `getPartVoiceSlots(p) <= 16` (or the restore rejects); today it returns the raw byte (e.g.
  131). Also add engine_state-payload bit-flips as a new fuzz corpus section.

## F-state-7: .parvati header fields are write-only; `version:` is never validated on load
- file:line — written at Source/ParvatiPreset.cpp:548-552 (patch) and :598-603 (multi)
  (`version`, `parvati_version`, `author`, top-level `name`); no read of `version`/
  `parvati_version`/`author` exists in applyParvatiPatch (:558-589) or applyParvatiMulti
  (:590-978); the in-document `name:` is also never applied — loadedProgramName_ is set from
  the FILENAME at Source/PluginProcessor.cpp:1370 and :1445.
- severity: low (latent forward-compat hazard)
- evidence: the loader ignores `version:` entirely (loader_fuzz's `version99` edit loads clean —
  proof the field is unchecked), while the engine blob strict-rejects unknown versions
  (SynthEngine.cpp:577 `if (version < 1 || version > 7) return false`). If a future format bump
  changes field semantics, older builds silently misparse v2 documents instead of rejecting.
  The top-level `name:`/`author:` asymmetry is benign metadata (the filename is deliberately
  authoritative per the loadParvatiPatchFile comment), but the version asymmetry is a real
  policy gap.
- deterministic_check: in parvati_preset_test.cpp, load a doc with `version: 99` plus a
  deliberately-semantics-changing key and pin the intended policy (reject with error, or
  documented accept-and-ignore) — today the test would pass only because nothing checks
  version; assert `applyParvatiMulti` on `version: 99` either returns false or is explicitly
  documented + asserted as ignored (matching kFormatVersion == 1).

# Verified-correct highlights (evidence)
- walkMbks bounds discipline on 64-bit (PatchFile.cpp:40-47): header reads guarded by
  `off+8 <= end <= size`, payloads guarded against `size`, memcpys gated on exact sizes
  112/84/56 — no unguarded memcpy/index on user buffers in .PRO/.MUL load.
- SynthEngine::restoreState is two-phase (parse-all-into-snapshots, commit only on full
  success, SynthEngine.cpp:568-805): every failure return precedes any mutation — the P1/P3
  contract loader_fuzz pins (296/296 cases pass, incl. truncated engine blobs; log:
  audit/bughunt_20260818/last_run_parvati_loader_fuzz_test.log).
- Validate-before-mutate on file loads: loadMultiFile's all-6-parts check
  (PluginProcessor.cpp:1019-1026), loadParvatiMultiFile's parts pre-parse (:1404-1433),
  loadParvatiPatchFile's params pre-parse (:1341-1348) — failed loads leave state byte-identical.
- Atomic writes everywhere: TemporaryFile+overwrite in writeAmbikaProgramFile
  (PatchFile.cpp:190-199), writeAmbikaMultiFile (:346-355), saveParvatiPatch/MultiFile
  (PluginProcessor.cpp:1323-1331, 1379-1387); ChainSplit writes primary first then rolls back
  ALL written siblings on unit failure (:1273-1292) — no half-generation on disk.
- Endianness/determinism: all formats byte-oriented LE (readLE32/le32; engine blob explicit
  byte writes), no struct dumps, all chunk sizes even (no RIFF padding garbage);
  getStateInformation snapshot determinism proven by fuzz canary c1.
- Blob version down-compat: v<2 (no FX), v1-4 (4 slot params, param5 zeroed),
  v3-only keepTails skip, v<6 (slots/name absent → popcount + empty name), v<7 (tuning block
  absent → raga byte authoritative, custom cleared) — all gated correctly.
- Undo: onPartSelect clears history outside undo/redo and arms the sweep flag
  (PluginProcessor.cpp:758-763); undoSafe/redoSafe sweep then replay (:773-793);
  loadPartIntoApvts writes the ValueTree with a NULL UndoManager (:884-886) so display dumps
  never become undo steps; restoringState_ RAII guard swallows re-entrant parameterChanged
  during replaceState (:1544-1546).
- TuningTables: no input path — static tables, id bounds-checked at dispatch
  (TuningTables.cpp:83-95); kTuningSilence (32767) round-trips verbatim through
  setPartTuningCustom (SynthEngine.cpp:1033-1035), engine blob, and .parvati
  (ParvatiPreset.cpp:876-884) — pinned by tuning_test + scala tests.
- Shadow-state contract (tests/shadow_state_test.cpp): defaults-multi load must zero-diff all
  9 polluted surfaces against a fresh engine — suite passes (log:
  last_run_parvati_shadow_state_test.log), and the comparator has per-category canaries.

# Coverage-gap notes (safe by inspection, untested)
- NRPN byte streams: midi_param_test covers value/stride/signed-INT8/unmapped CC+NRPN, but
  CC96/CC97 (data increment/decrement → nudgeValue, MidiParameterMap.cpp:299-310 — null-checked
  + clamped, safe) and CC99 MSB≠0 (part-space flag, :326-330) have zero test coverage; no NRPN
  fuzzing exists. Maps are bounds-guarded (midi_cc_map[controller] 0..127 via JUCE,
  midi_nrpn_map[address] 0..255 checked, firmware_parameters[paramIndex] gated by
  `paramIndex >= 58`).
- MUL/PRO chunk-header bytes and engine_state payload bytes are never fuzz-mutated (see
  F-state-3/F-state-6 for the concrete corpus extensions).
- Scala import has no fuzz corpus at all (only semantic tests) — F-state-2 is the concrete gap.

# Count summary
7 findings: 1 high (F-state-1), 1 medium (F-state-2), 5 low/latent
(F-state-3, F-state-4, F-state-5, F-state-6, F-state-7). 0 blockers in the verified-good
save/load/undo paths. 3 coverage-gap notes.
```

---

Output artifact: /Users/fuzboxz/parvati/.pi/subagents/artifacts/5bcb0170_reviewer_0_output.md
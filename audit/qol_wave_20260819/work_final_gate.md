# FINAL GATE — QoL Wave (custom scales removal / host params / a11y / render / editor)

## 1. Test fixes made

- **tests/host_state_test.cpp** — the v7 hard-code (`kV7PartStride` = 313) replaced by version-discovered stride math: new `capturePartStride()` reads the blob's version byte (v8 = 284; v7 = +29-byte tuning block; <6 → impossible size so the caller's check fails loudly) plus `kCurrentEngineBlobVersion = 8` sanity pins. Sections [4]/[5] (v1/v2 derivations) now derive offsets from the discovered stride; version assertions updated; misleading "v7 capture" comments corrected. **ALL CHECKS PASSED.**
- **tests/voice_slots_test.cpp [h]** — the v5-blob synthesis strip math is now version-discovered (v7Tuning = 4+25 only when capture version == 7), with a size sanity check guarding the cursor arithmetic before any memcpy. The old math read 6×27 bytes OOB on v8 captures (ASLR-dependent segv, 2/5). **ALL CHECKS PASSED × 5 consecutive runs (stable).**
- **tests/export_fallback_test.cpp** — (orchestrator-time fix, part of this gate) removed the obsolete D14 custom-tuning dialog case calling the deleted `MulExportDialog` ctor.

## 2. Render-lane verification verdict (contracts a–f)

- **(a) 8x never persists — PASS.** `setNonRealtime` boosts via `applyOversamplingFactor` (no `setUiOversampling`); `getStateInformation` snapshots `uiOversampling_` only. Pinned by render_quality_test: "user pref NOT bumped", "saved host state still carries 2x (no 8x leak)" (reads the actual XML).
- **(b) restore-on-exit + idempotency — PASS.** Exit branch restores saved factor; double-entry guarded by `offlineSavedOs_ >= 0`; `prepareToPlay` leak guard for hosts that skip `setNonRealtime(false)`; `setOversamplingFactor` mid-bounce re-targets the restore point. All four paths test-asserted.
- **(c) iOS excluded — PASS.** `#if ! JUCE_IOS` on both the boost block and the leak guard (compile-time; macOS build can't runtime-verify, code-read verified).
- **(d) oversized-block chunking — PASS (superset).** Full block tiled in `preparedMaxBlock_` slices; slice 0 byte-identical old path; later slices receive their window's MIDI rebased to [0,n) — strictly better than "MIDI-on-first-slice only" (events fire in their correct temporal window); transport clock advances on the full count. Test: 4×256 oversized block, all quarters non-silent.
- **(e) processBlockBypassed — PASS.** Clears every channel of the full buffer (all buses); no state flushes (un-bypass resumes); rationale for the debug-jassert override documented.
- **(f) delay tail math — PASS.** `feedbackTail = T·ln(1e-3)/ln(g)`; FV-1 Echo, **tempo-synced ClockedDelay** (T = 4/div·60/bpm, 1 s line clamp), CVerb 8483-sample tank; reverbs by decay/predelay; freeze→12 s cap; floor 0.2 s; cache recomputed on FX-dirty + tempo moves. Test table + processor-level cache asserted.

**parvati_render_quality_test: ALL CHECKS PASSED.**

## 3. Full suite (112 binaries + parvati_tests; screen/menu-shots + preset_stage dir skipped)

**112 PASS / 0 FAIL.** Two chase-downs during the gate:
1. **multigui/ui_mirror suite-order failures** — root-caused to a STALE `parvati_gen_templates` binary (`EXCLUDE_FROM_ALL`, so full builds never refresh it; it was left mid-lane by an earlier worker). Running it mutated repo `presets/TEMPLATES/` (voice_slots 4/Cyclic vs committed 1/Mono), breaking the template-mirror tests that ran after it. Restored `presets/` from HEAD; rebuilt the generator — current lane code reproduces committed templates byte-identically (self-verify 5/5). Proven: HEAD run of the stale-flow passes; the committed-template/generator divergence fix_verify reported was an artifact of the mid-lane binary, not real drift.
2. **parvati_fx_crackle_diag_test SIGBUS (rc 138, ~2/5 runs)** — proven **pre-existing on clean HEAD** (stash → rebuild → 4/5 SIGBUS → pop). Diagnostic binary, documented, not fixed per instructions.

Pre-existing tooling unchanged vs HEAD: check_translations 3 violations (identical set), check_async_this identical findings.

## 4. CHANGELOG

One consolidated entry added at the top of [Unreleased] covering all 9 items (scale removal + backcompat paths, value-to-text + groups, host context menus, preset stepping/shortcuts, offline 8x + guards, chunked render, bypass, dynamic tail incl. delay law, a11y) + new/updated tests + suite verdict. Style matches existing entries.

## 5. Tree state

48 files changed (+1778/−2718), 11 untracked (audit reports + 2 new test files). Nothing staged. No `presets/` modifications.

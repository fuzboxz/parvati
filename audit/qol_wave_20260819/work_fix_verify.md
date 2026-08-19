# PART A — host_param_text_test fix + PART B — full-suite gate

## Part A — env-time assertion fix (TEST was wrong, formatter is correct)

- Ground truth verified: `lut_res_env_portamento_increments[0] == 65535` (`resources_data.cpp:31`) — the LUT is **fastest-first** and never contains 0.
- With the exact constants (`kInternalSampleRate=39216`, `kAudioBlockSize=kControlRate=40`, `constants.h:29,53`): inc=65535 → t = (65536·40)/(65535·39216) = **1.0200 ms** → `envTimeToString` returns **`"1ms"`** (the `<1ms` branch is NOT taken). inc=1 (byte 127) → 66.846 s → `"66.8s"`.
- `ParameterLayout.cpp:812` wires `paramValueTextSynth` — the SAME formatter the UI readouts use — so the formatter is the UI ground truth; the test's ∞ expectation assumed byte 0 → inc 0, which the LUT cannot produce.
- Fix in `tests/host_param_text_test.cpp` (~197): expect `"1ms"` for value 0, added `"66.8s"` for 127, with a comment documenting the LUT direction. Formatter untouched (its inc==0 ∞ branch stays defensive). `ParameterLayout.cpp` untouched per constraints.

## Part B — suite results (105 binaries; `screen_shots`/`menu_shots` excluded, no-display)

**99 PASS**, including all Phase-1-relevant sets re-verified after the build settled: tuning, preset, ios_openin, patch_load, multitimbral, polyphony, patch_arrangement, mul_strategies, export_fallback, concurrency, apvts, editor, ui_mirror, preset, loader_fuzz, load_invariants, shadow_state, roundtrip_golden.

**2 invocation artifacts (PASS from repo root):** drum_kit + firmware_parity are CWD-relative (`tests/drum_kit_test.cpp:81`); run from `/` of the repo → both rc=0. scala_import_test binary is a stale artefact (target deleted by the scales lane).

**FAILURES:**

1. **host_state_test (5)** — `tests/host_state_test.cpp:44,328-368` hard-codes the capture format as v7 (`kV7PartStride`); captureState now emits **v8** (`SynthEngine.cpp:~497`). *Lane-caused (scales-lane blob v7→v8 bump; test not updated).*
2. **voice_slots_test (flaky: 2/5 segv SIGSEGV, else 1 check-fail)** — `tests/voice_slots_test.cpp:278,287` builds a "legacy v5" blob by stripping a 29-byte **v7 tuning block** per part from a fresh (now v8) capture → cursor overruns the capture buffer → **heap OOB read in the test's memcpy** (ASLR-dependent segv) or a corrupt blob → "legacy v5 blob restores" FAIL. *Lane-caused, same v8 class; this one is memory-unsafe in the test itself.* Both 1+2 are merge blockers for the scales lane; trivially fixed by making the stride/strip math v8-aware (or version-discovered).
3. **multigui_test (1 stable)** — `tests/editor_test.cpp:1090` "empty tree: step returns an invalid File", part of the **new [18] section (preset stepping + shortcuts + host-context menu) currently being added by the concurrent Phase-2 editor lane** (PresetBrowser.h + editor_test.cpp churn observed live; binaries vanished/relinked mid-gate). *In-flight lane work — re-verify when that lane completes; not Phase-1 damage.* Note editor_test standalone now passes; only multigui's aggregate run hits it.

**Incidental finding (pre-existing, not lane-caused):** running `parvati_gen_templates` at current code writes `voice_slots: 4` / `part_polyphony: 3 (Cyclic)` for Drum Kit parts where the committed templates say `1` / `Mono` (all 5 templates diverge). All generator inputs (gen_templates.cpp, PatchArrangement, engine slot/poly setters, serializer) are functionally unmodified by the lanes (diff-verified) → committed templates are **stale since dd99db6** (8 engine commits ago). The generator's self-verify covers only the 4 named templates, not Drum Kit. Recommend: do not regenerate templates until audited; add Drum Kit coverage to gen_templates verify. (I restored presets/TEMPLATES after my probe; tree clean.)

## Caveats
- Concurrent Phase-2 lanes edited/relinked the tree mid-gate (binaries transiently missing); final statuses above are post-settle re-runs.
- Side effects: none (presets/TEMPLATES restored; temp probe under /tmp only).

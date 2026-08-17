# Contributing to Parvati

Thanks for your interest in Parvati! This document covers how to build, test,
and contribute.

## Building

Parvati uses **CMake** as its only build system (the legacy Projucer `.jucer`
has been removed). You need CMake ≥ 3.22, a C++17 compiler, and a JUCE checkout
(default `$HOME/JUCE`, `%USERPROFILE%\JUCE` on native Windows, override with
`-DJUCE_GLOBAL_PATH=...`).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 8
```

On **Windows** use the Visual Studio 17 2022 generator (Developer PowerShell):
`cmake -S . -B build -G "Visual Studio 17 2022" -A x64` then `cmake --build
build --config Debug -j 8`. The sanitizer/`-Werror` CMake options are
GCC/Clang-only and no-op under MSVC.`

On **Linux** install JUCE's development packages first (see README for the
apt list), then use the plain CMake flow. Linux builds VST3, CLAP, and
Standalone — the classic AU is macOS-only. The CLAP format comes from
clap-juce-extensions (fetched and pinned automatically at configure time);
`-DPARVATI_BUILD_CLAP=OFF` disables it, and
`-DPARVATI_CLAP_EXTENSIONS_PATH=/path/to/clap-juce-extensions` builds against
a local checkout instead of the pin. Note the perf smoke test and the
`profile_ui.sh` gate are macOS-only (they drive the CoreFoundation run loop
and CGEvents).

We develop and test against **Debug builds only**. Do not rely on a Release
build for verification.

## Running the tests

All tests are built by default:

```bash
cmake --build build -j 8
for t in build/parvati_tests build/parvati_*_test; do [ -e "$t" ] && "$t"; done
```

(`parvati_tests` does not match the `parvati_*_test` glob — list it explicitly
or it is silently skipped.)

If you add a feature, add or extend a test under `tests/` and mirror an existing
CMake target. The editor coverage test (`tools/editor_test.cpp`) and the layout
sanity check are especially good guards for UI work.

## Performance regression testing

Two layers guard the UI against perf regressions:

1. **`parvati_perf_smoke_test`** (`tests/perf_smoke_test.cpp`) — headless,
   runs locally on demand. It constructs the full editor and pumps the JUCE
   message loop for 10 s against CPU-time and message-thread-congestion
   budgets. It catches the timer/message-storm class of regression (e.g. a
   ComboBox `clear()` arming a deferred `onChange` that re-enters its own
   rebuild — that one cost a full core at idle). Run it with
   `./build/parvati_perf_smoke_test`. Budgets live at the top of
   `tests/perf_smoke_test.cpp`; if a legitimate feature raises the floor,
   re-measure and update the constant together with the measurement comment.

2. **Local: `tools/profile_ui.sh`** — the full gate for work that touches
   paint paths, timers, or layout. It builds the Release standalone, then
   measures idle CPU, active CPU (while `tools/ui_drag_helper.swift` drags
   the mouse in circles), and jassert noise, against the budgets in its
   config block (override via `PROFILE_IDLE_MAX` / `PROFILE_ACTIVE_MAX` /
   `PROFILE_MAX_ASSERTS`). It needs a real GUI session: posting CGEvents
   requires Accessibility permission for the calling terminal. Run it after UI
   perf work:

   ```bash
   tools/profile_ui.sh                 # defaults to build_release
   ```

### Threading / concurrency

Most tests are single-threaded and **cannot** surface message↔audio data races
(the kind that crash the hosted plugin but stay green under ASAN). The
`parvati_concurrency_test` is the exception: it spins a real background AUDIO
thread (transport playing + a held note, so the arp / note-sequencer generate
notes) while the MESSAGE thread runs the full host surface (param edits, arp/seq,
part switches, `.MUL`/`.parvati` loads, host-state get/set, options, voice-mode).
Reuse `tests/mt_harness.h` (`parvati_test::runConcurrent`) to add two-thread
coverage to other tests.

**Always run it under ThreadSanitizer** — that is how these races are caught:

```bash
cmake -S . -B build_tsan -DPARVATI_ENABLE_TSAN=ON
cmake --build build_tsan -j 8 --target parvati_concurrency_test
for i in $(seq 5); do ./build_tsan/parvati_concurrency_test || echo RACE; done   # races are timing-dependent
```

`PARVATI_MT_MASK` (argv[1], hex) selects which message-thread op classes run, for
bisection (`./build_tsan/parvati_concurrency_test 0x2` = arp/seq only, etc.).

## Memory safety & static analysis

Before a non-trivial change, run the sanitizer suite and a clang-tidy pass:

```bash
cmake -S . -B build_asan -DPARVATI_ENABLE_ASAN=ON -DPARVATI_ENABLE_UBSAN=ON
cmake --build build_asan -j 8 && (cd build_asan && for t in parvati_*_test; do "$t"; done)
/opt/homebrew/opt/llvm/bin/run-clang-tidy -p build Source/*.cpp
```

`compile_commands.json` is generated automatically (CMAKE_EXPORT_COMPILE_COMMANDS).

## Code style

- Match the surrounding code. The dominant style is JUCE: 4-space indent, Allman
  braces, a space before the opening paren (`foo (x)`, `if (x)`), `type*` bound
  to the type on the left, ~100 columns.
- `.clang-format` is a **best-effort guide for new files** (`clang-format -i
  <new-file.cpp>`); the existing tree predates it and is **not** mass-reformatted.
  The faithful firmware-port files under `Source/dsp/` use their own compact
  style and are exempt.
- `.clang-tidy` is configured for bug-focused analysis. Prefer fixing a real
  finding; use a precise `// NOLINT(check-name)` with a reason only for genuine
  false positives (e.g. integer DSP arithmetic).

## The faithful DSP

`Source/dsp/` is a bit-exact port of the Ambika firmware. Preserve its integer
arithmetic and structure — do not "modernize" it into floating point. Any change
there should be validated against the existing DSP tests
(`parvati_lfo_*`, `parvati_filter_topology_test`, `parvati_tests`, …).

`Source/TuningTables.cpp` follows the same policy for the CONTROLLER side: the
32 scale ("raga") tables are vendored verbatim from the firmware's
`controller/resources.cc` — do not edit them by hand; re-vendor from upstream
if the firmware ever changes (see the file header and `NOTICES.md`). The Scala
converter (`Source/ScalaImport.cpp`) is Parvati-authored and unit-tested against
`parvati_scala_import_test`; per-part tuning behavior is covered by
`parvati_tuning_test` (plus the `[9]` tuning section of `parvati_multigui_test`
for the UI paths).

## Submitting changes

1. Open an issue describing the change for non-trivial work.
2. Keep diffs focused and minimal; don't reformat unrelated code.
3. Ensure `cmake --build build` and the full test suite pass (ideally also ASan).
4. Document user-visible changes in [`CHANGELOG.md`](CHANGELOG.md).

## Licensing

Parvati is GPL-3.0 (a derivative of the GPL-3.0 Ambika firmware). By
contributing you agree your changes are licensed under the GPL-3.0.

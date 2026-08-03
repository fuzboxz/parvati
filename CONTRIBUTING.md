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

## Submitting changes

1. Open an issue describing the change for non-trivial work.
2. Keep diffs focused and minimal; don't reformat unrelated code.
3. Ensure `cmake --build build` and the full test suite pass (ideally also ASan).
4. Document user-visible changes in [`CHANGELOG.md`](CHANGELOG.md).

## Licensing

Parvati is GPL-3.0 (a derivative of the GPL-3.0 Ambika firmware). By
contributing you agree your changes are licensed under the GPL-3.0.

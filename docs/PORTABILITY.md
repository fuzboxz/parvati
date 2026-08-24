# Parvati cross-platform notes

Date: 2026-08-23. Source: a portability audit of the build files, the shell
tools, the C++ tree and the test suite. This file records what the audit
verified, what this machine can test, and every item left open.

## Platform status

| Platform | Status | Notes |
|---|---|---|
| macOS | Green, tested here. | Full suite passes 123/123 (`tools/run_tests_parallel.sh`). Sanitizer trees and release flow work. |
| iOS | Configures and builds per the documented Xcode invocation. Not exercised in this pass. | The audit touched no iOS block. `build_ios` regenerates from source on demand. The suite keeps its iOS guards (`parvati_tests.cpp` SDK-level check). |
| Linux | Not compiled here. Build-file and test-source blockers fixed at guard level. | `-Wl,-dead_strip`, the narrowing flag spelling and the CoreMIDI tool guard now match Linux conventions. The test binary compiles from guarded sources: the runner falls back to in-process mode, `perf_smoke_test` compiles a skip stub, POSIX headers and calls sit behind guards. A Linux CI build must confirm the result. Headless runners need `xvfb-run` (see Known gaps). |
| Windows | Not compiled here. Build-file and test-source blockers fixed at guard level. | The test binary takes no Apple-only link flag under MSVC, and the ctest scanners resolve `python.exe` via `find_package(Python3)`. The runner uses `_putenv_s` and in-process execution, `layout_overlap_test` skips demangling, `build_policy_test` skips the exec-bit check, and `parvati_tests.cpp` probes tools with `where`. Per-test fork isolation stays lost on Windows (see Known gaps). JUCE lists Windows prerequisites in `README.md`. |

## Known gaps

- Linux and Windows never compiled in this pass. No cross toolchain exists
  here, so every fix is a spelling or guard change verified on macOS only.
- Windows runs the unified suite in-process. `fork()`/`waitpid()` have no
  MSVC equivalent, so `runTestIsolated` calls the in-process path there. A
  `CreateProcess`-based runner would restore per-test isolation; that work
  stays open.
- Headless Linux CI needs `xvfb-run`. Several tests create real desktop
  windows. They now print `SKIP: no display server` and return true when
  JUCE reports no display, instead of aborting on peer creation. The skip
  path is dormant on macOS and stays unverified here: no headless host
  exists in this pass.
- `perf_smoke_test` budgets stay machine-tuned. A loaded runner can exceed
  the wall-clock budgets without a code regression.
  `PARVATI_TEST_PERF_BUDGET_MULT` scales every budget on such hosts; the
  file header documents the knob.
- MSVC gets no dead-code link flag. MSVC drops unreferenced COMDAT functions
  by default; `/OPT:REF` stays untested, so the build omits it.
- Test-file header comments still name the purged per-test binaries. The
  build files no longer reference those targets; the comment sweep stays
  open for the tests scope.
- `tools/release/sign_and_notarize.sh` and `tools/profile_ui.sh` stay
  macOS-only by design. Both state the scope in their headers.
- AU or AUv3 in a `PARVATI_FORMATS` override on a non-Apple host relies on
  the JUCE configure-time `FATAL_ERROR`. `CMakeLists.txt` documents this as
  intended behavior.
- Vendored stmlib headers use GNU attributes and block an MSVC compile.
  `Source/dsp/clouds/stmlib/utils/dsp.h` puts `__attribute__((always_inline))`
  on twelve declarations, and `stmlib.h` keeps the `IN_RAM` section macro.
  Almost every FX translation unit reaches these headers. The repo rule
  forbids edits under `Source/dsp/clouds/**`, so this stays open. Wrap the
  attributes in a compiler guard at the next vendored-patch pass.
- The C++ tree passes include hygiene. Every quoted include matches the file
  tree with exact case. `Source/` calls no printf-family function and includes
  no platform header. `MulExport.h` now includes `<string>` directly; it
  declares `std::string` in its API.
- The test env helper carries one semantic edge: `setEnvVar` with an empty
  value removes the variable on POSIX and blanks it on Windows. No reader in
  the tree distinguishes the two states, so the difference stays harmless.

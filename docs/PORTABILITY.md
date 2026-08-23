# Parvati cross-platform notes

Date: 2026-08-23. Source: a portability audit of the build files, the shell
tools and the C++ tree. This file records what the audit verified, what this
machine can test, and every item left open.

## Platform status

| Platform | Status | Notes |
|---|---|---|
| macOS | Green, tested here. | Full suite passes 123/123 (`tools/run_tests_parallel.sh`). Sanitizer trees and release flow work. |
| iOS | Configures and builds per the documented Xcode invocation. Not exercised in this pass. | The audit touched no iOS block. `build_ios` regenerates from source on demand. |
| Linux | Not compiled here. Build-file blockers fixed; flag-level fixes only. | `-Wl,-dead_strip`, the narrowing flag spelling, the CoreMIDI tool guard, `python3` resolution and the shell-tool defaults now match Linux conventions. A Linux CI build must confirm the result. |
| Windows | Not compiled here. Build-file blockers fixed; flag-level fixes only. | The test binary takes no Apple-only link flag under MSVC, and the ctest scanners resolve `python.exe` via `find_package(Python3)`. JUCE itself lists Windows prerequisites in `README.md`. |

## Known gaps

- `parvati_check_translations` fails at HEAD. Fourteen `TRANS()` keys sit in
  no FR/DE table and no allowlist entry. The failure predates this pass and
  belongs to `Source/ui/Translations.cpp` scope.
- Linux and Windows never compiled in this pass. No cross toolchain exists
  here, so every fix is a spelling or guard change verified on macOS only.
- MSVC gets no dead-code link flag. MSVC drops unreferenced COMDAT functions
  by default; `/OPT:REF` stays untested, so the build omits it.
- Test-file header comments still name the purged per-test binaries. The
  tests scope owns that sweep; build files no longer reference those targets.
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

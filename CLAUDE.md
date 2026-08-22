# Parvati — agent & build policy

Parvati is a JUCE port of the Mutable Instruments Ambika polysynth (macOS/iOS).
Hard rules first; agents skim, so this file stays short on purpose.

## Test policy (hard rules — the suite is ONE binary)

- **NEVER create per-test `add_executable` targets.** All tests live in the single
  `parvati_unified_tests` binary. ~100 such targets once wasted ~3.5 GB of disk.
- **Add new tests ONLY via `tools/new_test.sh <name>`** — it scaffolds the file,
  registers it, and by construction cannot spawn a separate binary.
- **Every `tests/*.cpp` must be in the unified target's source list.** A
  configure-time orphan guard FAILS `cmake` if one is missing — do not work
  around it; fix the registration.
- Reuse `tests/test_utils.h` helpers (`setInt`/`setChoice`/`setParam`, etc.)
  instead of copying them into new files.
- Fork-per-test isolation: tests must be self-contained (no cross-test state,
  fresh processor per test). Run single tests in-process with
  `PARVATI_UNIFIED_INPROCESS=1` when debugging.
- Do not leave `.bak`/`.tmp`/backup files behind. Do not weaken assertions.

## Build & test commands

**ALL build dirs were purged 2026-08-22** (7 trees / ~20 GB → gone; don't
recreate the sprawl). The canonical dir is **`build_unified`** — one dir,
Debug, full plugin formats + the unified test binary:

```bash
cmake -B build_unified -DCMAKE_BUILD_TYPE=Debug   # canonical (only) build dir
cmake --build build_unified --target parvati_unified_tests -j8

./build_unified/parvati_unified_tests             # all 114 tests (~15-20 min)
./build_unified/parvati_unified_tests list        # list tests
./build_unified/parvati_unified_tests <name>      # one test
PARVATI_UNIFIED_INPROCESS=1 ./build_unified/parvati_unified_tests <name>

tools/run_sanitizers.sh                           # ASan+UBSan / TSan sweeps
```

- **Do NOT create ad-hoc `build_*` dirs.** Reuse `build_unified`; sanitizer
  dirs (`build_san_asan`/`build_san_tsan`) are created ON DEMAND by
  `tools/run_sanitizers.sh` as **tests-only** trees (`-DPARVATI_FORMATS=` —
  no plugin bundles, ~1.4 GB vs the old 9.6 GB full-instrument dirs).
- **`PARVATI_FORMATS`** (cache option): plugin formats to build. Default =
  the platform set (macOS: `Standalone;VST3;AU`, + CLAP via
  `PARVATI_BUILD_CLAP`). **Empty** = tests-only configure (no plugin
  targets) — use for sanitizer/CI test trees.
- **iOS** (dir was purged; regenerates from source): the documented
  invocation lives in `CMakeLists.txt` (~line 361) —
  `cmake -S . -B build_ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 -DPARVATI_IOS_DEVELOPMENT_TEAM=<team>`
  (simulator builds need no team id; with the Xcode generator pass
  `--config Release` BEFORE the `--` separator).

- `PARVATI_TEST_EXAMPLES=ON` (CMake option, default OFF) registers the 8
  example demo tests; `example_failing_test` deliberately fails — keep demos
  out of default runs.
- TSan note: `loader_fuzz_test` is OOM-killed under TSan (known; see
  `MIGRATION_STATUS.md` §Sanitizer validation). Zero races elsewhere.

## Repo map

- `Source/` — plugin + dsp port of Ambika. `ambika_reference/` is the **untracked**
  GPL3 firmware reference tree, required at configure time (see `NOTICES.md`).
- `tests/` — the unified suite (114 tests, single binary).
- `tools/` — scripts + `EXCLUDE_FROM_ALL` utilities (registered via `parvati_add_tool`).
- `docs/` + `MIGRATION_STATUS.md` — status source of truth (gitignored by
  root `*.md` policy). `CHANGELOG.md` must be updated for user-visible changes
  (`CONTRIBUTING.md` rule).

## Commit style

Conventional prefixes — `fix`/`feat`/`test`/`build`/`tools`/`chore`/`refactor` —
plus a scope, with the full reasoning in the body (see `git log`).

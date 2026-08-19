# Fix report — key `misc-ios` (iOS hunt wave, 2026-08-19)

Owns exactly: `tests/parvati_tests.cpp` (one guard), `audit/ios_device_checklist.md`
(new), `CHANGELOG.md` (one appended section). No other files touched.

## FIX 1 — F-ios-build-3: `std::system` blocks the iOS toolchain build of `parvati_tests`

- **Change:** `tests/parvati_tests.cpp` `toolAvailable()` (was :55) now returns
  `false` on iOS without invoking the shell, behind a new `PARVATI_TESTS_IOS` macro.
  The two call sites (:202-203) are unchanged — they only consume the bool.
- **Why not `#if ! JUCE_IOS`:** this harness is deliberately JUCE-free (pure
  `ambika::dsp` + std; verified: no JUCE header in its include chain), so `JUCE_IOS`
  is never defined in this TU — the literal guard from the task brief would have
  compiled `std::system` on iOS anyway. The guard instead detects iOS at the SDK
  level: `__IPHONE_OS_VERSION_MIN_REQUIRED` (defined for BOTH device and simulator
  and nothing else) plus a `TargetConditionals` `TARGET_OS_IPHONE` belt-and-braces.
- **Deterministic A/B (both run, no shared build dir used):**
  - PRE-FIX (guard forced off in a /tmp copy):
    `xcrun -sdk iphonesimulator clang++ -std=c++17 -I Source -fsyntax-only` →
    `error: 'system' is unavailable: not available on iOS` (the exact lane-reported
    error, reproduced).
  - POST-FIX (the real file): same command → **compiles clean**.
  - DESKTOP (build_release): `cmake --build build_release --target parvati_tests -j4`
    → builds; `./build_release/parvati_tests` → **ALL CHECKS PASSED (0 failures),
    EXIT=0** — behaviour identical to pre-fix (probe still reports avr-gcc/simavr
    missing on this machine, bit-exact diff SKIPPED, same as before the change).

## FIX 2 — `audit/ios_device_checklist.md` (new)

Seven on-device sections (D1–D7) covering every UNKNOWN-NEEDS-DEVICE item from the
five lanes, each with exact manual steps + expected result + FAIL criteria:
AUv3 instantiation in AUM/GB (AudioComponentBundle question), interruption/route
change post-fix, save-from-host Files visibility (both apps round-trip), open-in
from Files/AirDrop/Mail incl. the deterministic plutil build-config check AND the
explicitly-noted REMAINING integration (the app must still LOAD the opened file at
launch — JUCE Standalone delegate seam, scoped as follow-up), hardware-keyboard
host-return + combo-arrow + Tab-cycle behaviour, save-picker overwrite prompts per
provider, thermal at 2x OS with the measured CPU budget from the perf lane. Plus a
post-run results ledger convention.

## FIX 3 — `CHANGELOG.md`

One "iOS quality wave (2026-08-19)" entry appended at the top of `[Unreleased] ›
Changed` in the house style: hunt scope, the landed headline fixes (document
types/UTIs graft, the iOS test-suite guard, dead-bitcode removal, build-invocation
documentation), the checklist pointer, and the deferral of per-fix details to the
`fix_*.md` reports.

## Validation summary

- `./build_release/parvati_tests` — ALL CHECKS PASSED, EXIT 0 (desktop unchanged).
- iOS-simulator syntax compile of the fixed TU — clean; pre-fix copy reproduces
  the lane's exact error (A/B evidence above).
- `git status` — only the three owned files modified/added by this run (plus
  sibling-owned files being edited concurrently by other agents; nothing staged
  by this run).

## Residual risks

- The full iOS *link* of `parvati_tests` was not exercised here (building the whole
  Parvati target for iOS would contend with sibling agents' in-flight Source/ edits
  and the shared-build-dir rule); the TU-level fix is what removes the only reported
  iOS blocker, and `parvati_multi_load_test` already links clean for iOS sim, so the
  remaining risk is that another Source/ file gained an iOS-unavailable symbol in
  the meantime — covered by whichever lane wires iOS CI.
- Simulator `fsyntax-only` proves compilation, not the appex runtime; runtime items
  remain the checklist's domain by design.

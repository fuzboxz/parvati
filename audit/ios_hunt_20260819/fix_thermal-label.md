# Fix thermal-label — F-ios-perf-2 follow-up (the documented 3-line rider)

Lane: iOS hunt 2026-08-19 remediation, key `thermal-label`. Completed 08:05.

## Ownership / scope
- OWNED + EDITED: `Source/PluginEditor.cpp` (the pure helper + the 30 Hz
  timerCallback thermal block ONLY), `Source/PluginEditor.h` (the public
  enum + static helper declaration — REQUIRED by the task's own mandate that
  the transition helper be a testable `ParvatiEditor::` static; no sibling
  owns the header this wave), `tests/lifecycle_test.cpp` (section [5] only).
- OBSERVED, NOT TOUCHED: the sibling open-in worker's in-flight edits
  (`CMakeLists.txt`, `Source/PluginProcessor.cpp`, `Source/ui/IosOpenIn.*`,
  suite-run log refreshes). Nothing staged.

## What shipped

1. **Pure policy seam** (`ParvatiEditor::thermalStatusForTransition`,
   public static, header-declared): the full transition decision —
   escalations (0->1, 0->2, 1->2) return ShowHint / ShowStrong
   (distinguished), de-escalations (1->0, 2->0, 2->1) return Clear,
   same-level repeats return NoOp. Inputs are `ThermalAction` ints
   (0/1/2 — read from the processor enum), clamped defensively so a corrupt
   atomic can never invent an action outside the sampler's domain.
2. **30 Hz surfacing** (timerCallback, `#if JUCE_IOS`): ONE relaxed atomic
   read of `processorRef_.getThermalHint()` per tick; ONLY a transition arms
   `ParamControl::postTransientStatus` (the documented status-strip seam —
   `tickTransientStatus` drains it ~30 Hz into the strip). Placed BEFORE the
   single drain so an escalation is visible on the same tick.
   - Hint (Serious): `TRANS("Thermal: reduce Filter Quality")`, 90 frames (~3 s).
   - StrongHint (Critical): `TRANS("Thermal: lower Filter Quality now")`, 150 (~5 s).
   - Clear: the seam is frame-budget based with no explicit clear API —
     documented no-op (expiry handles it; advisory-only policy).
   - State is a file-scope static INSIDE the gate: matches the seam it drives
     (transient status is process-global; the hint is processor-global) and
     avoids a desktop-unused private field under `-Werror`.

## Deterministic test — lifecycle_test [5]
- All 9 matrix cells pinned with labels (3 escalations, 3 de-escalations,
  3 same-level no-ops incl. the idle 0->0 desktop tick) + 3 out-of-range
  clamp checks.
- **Mutation proof the pin bites**: temporarily changed same->same to return
  ShowHint (the real regression class — re-posting every tick, ~30 Hz status
  repaint churn) -> RED on exactly the 4 same->same checks, rc=1; reverted ->
  green. (An earlier mutation attempt was VACUOUS — `n == o` inside
  `if (n > o)` is unreachable — which is itself why the mutation run must be
  verified to actually go red, not assumed.)

## Validation
- `parvati_lifecycle_test`: ALL CHECKS PASSED (0 failures) — [1]-[5].
- `parvati_editor_test`: ALL CHECKS PASSED (0 failures) — timer path compiles,
  no desktop behavioral change.
- `parvati_param_thread_test`: ALL CHECKS PASSED (0 failures).
- Shared-build-dir guard used before every build; -j4; minimal targets.

## Residual
- The Obj-C++ shim half (application:openURL:) is the SIBLING's lane.
- On-device thermal timing (real NSProcessInfo transitions) remains on the
  device checklist — the pure matrix + gate are fully covered headlessly.

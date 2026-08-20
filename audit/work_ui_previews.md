# Preview-Display Update Regression — Fix Report

## Root cause (verified, not guessed)

All three previews (`OscPreviewDisplay`, `EnvelopeDisplay`, `FilterResponseDisplay`) poll the APVTS via a self-owned 30 Hz `juce::Timer` gated in `visibilityChanged()` by `isShowing()` — the F-ios-perf-3 battery gate added in commit **`2f68b5d`** (iOS quality wave, 2026-08-19).

The gate is broken by a JUCE semantics trap:

1. `juce::Component` is constructed **hidden** (`juce_Component.cpp:486 — componentFlags (0)`; `visibleFlag = 0`).
2. `addAndMakeVisible(child)` (JUCE source) calls `child.setVisible(true)` **before** `addChildComponent` — so the display's `visibilityChanged()` fires while it is still **unparented**.
3. Unparented/no-peer ⇒ `isShowing() == false` (`juce_Component.cpp:602-613`, final `return false`) ⇒ the callback called **`stopTimer()`**, killing the constructor's timer immediately.
4. `visibilityChanged` is sent **only** by `setVisible` on the component itself (`juce_Component.cpp:545-581`) — ancestor visibility changes, reparenting, and the editor gaining its desktop peer **never propagate it to descendants**. So nothing ever restarted the timer.

Net effect: every preview rendered its constructor-built initial graph once and froze — no reaction to shape dropdowns or any param change, in every format (Standalone included). Confirmed empirically with an instrumented probe: `visibilityChanged` fired exactly once per display with `isShowing=0 parent=0x0`, and the pre-fix regression test showed generations frozen (`osc gen 1→1`, `env gen 0→0`) even while pumping 700 ms of the message loop.

## The fix

`parentHierarchyChanged()` **does** recurse through all children on every hierarchy change — including `addToDesktop` when the editor gets its peer (`juce_Component.cpp:1627-1646`, `internalHierarchyChanged` iterates children) and page hosting/reparent swaps. All three displays now override **both** hooks, funnelling into one `updatePollTimer()` (`isShowing() ? startTimerHz(30) : stopTimer()`). The F-ios-perf-3 battery goal is preserved (polls stop when genuinely not showing — e.g. the whole editor headless/closed); the restart path is now correct.

## Test hook design

- `previewGeneration()` on all three displays (TEST-ONLY): `OscPreviewDisplay` increments in `rebuildCycle()` (every real cycle rebuild); the other two increment in `timerCallback` only when the eps-gated change actually fires a repaint. `isPollRunningForTest()` exposes timer liveness (`Timer` is a private base).
- `ParamPage::getGroupDecorationForTest/getGroupInlinePreviewForTest(name)` let tests reach the live displays through `allGeneratedPages()`.

## Test results

`tests/editor_test.cpp` **[19]** (target `parvati_multigui_test`): headless run pins components-found + polls-correctly-stopped; `--windowed` (real `addToDesktop` peer — the Standalone path) pins polls-running-after-peer-attach plus the four user-reported behaviors: **osc1 shape change → rebuilt (gen 1→2), osc1_param → rebuilt (2→3), env1_attack → ADSR refreshed, filter1_cutoff → response refreshed**. Pre-fix, these fail frozen (captured). Standalone rebuilt with the fix.

Commands (repo root): `cmake --build build -j8` (0 errors), `./build/parvati_multigui_test [--windowed]`, `parvati_ui_mirror_test`, `parvati_apvts_test`, `parvati_host_param_text_test`, `parvati_lifecycle_test`, `parvati_editor_test`, `parvati_ipad_hig_sizing_test` — **all green** (multigui/ui_mirror must run from the repo root: CWD-relative preset paths).

## Residual risks

- `parentHierarchyChanged` fires on any ancestor add/remove: `startTimerHz` on a running timer just re-arms the period (cheap); no behavioural risk observed.
- The windowed section needs a window server (fine locally; headless CI runs still pass — the windowed half is opt-in).
- Parameter *grouping* (QoL wave) was exonerated: getters bind by parameter ID, order-independent.

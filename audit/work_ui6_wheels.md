# Batch-6 lane: wheels wheel-scroll + envelope initial transient

## ITEM 1 — wheel-scroll offenders (audit across Source/ui/)

Offenders found (sliders NOT disabling the wheel):
1. **`WheelsComponent.cpp` pitch (SpringSlider) + mod (juce::Slider)** — plain juce::Sliders handle the wheel by default; a scroll over the bottom strip tweaked pitch/mod. **FIXED**: `setScrollWheelEnabled(false)` on both (the ParamControl idiom; unhandled wheel bubbles to the Viewport).
2. **`ModMatrixView.cpp:495 depthSlider_`** and **`FxMatrixView.cpp` depth slider** — plain members, never disabled. **NOT FIXED here** (owned by the sibling matrix lane; flagged for it).
Already clean (verified): ParamControl knobs (PluginEditor.cpp:430), PatchPage table cells (`setupKnob`), FxRoutingBar mix/EQ knobs, MidiKeyboardComponent (no wheel handler), NoteStepControl/SeqLengthStepper (inherit ParamControl's disable), CentralModBar/GroupPager (no value controls).

**Test**: `keyboard_view_test` new **[9]** — synthetic `MouseWheelDetails` (4 notches, deltaY 0.5) fed directly into each wheel's `mouseWheelMove`; asserts value AND the onPitch/onMod callbacks stay untouched. PASS.

## ITEM 2 — envelope initial transient

Root cause (`EnvelopeDisplay.cpp`): attack width was the raw proportional knob value `wA = a`; at a ≈ 0.02 (≈1–2 ms) the ramp occupied ~1–2% of the plot (invisible), and at a == 0 the trace started AT the peak — the 0 → 100% transient was not drawn at all.

**Fix**: attack floor `wA = jmax(a, 0.09 * (d + wS + r))` — a fast attack now renders as a near-vertical ramp at the left edge (always visible, always from 0); moderate attacks keep their exact proportional share. Curve math extracted to the pure `EnvelopeDisplay::adsrCurveLevel` (one definition shared by `paint()` and the new `adsrCurveLevelForTest` hook).

**Test**: editor_test `[19]` — a=0 and a=0.02 start < 5% and reach > 90% by 12% width; a=0.45 stays gradual (ramp mid-point below peak). All PASS (windowed run; the headless `[19]` block skips shape pins by design).

## Validation
`parvati_multigui_test --windowed` (all [19] pins green), `parvati_keyboard_view_test` (PASS incl. [9]), `parvati_lifecycle_test`, `parvati_layout_minwidth_test` — green. One pre-existing multigui failure `[23] tabStrip position` is the sibling PatchPage lane's in-flight state (their file), not this lane.

## Notes for the parent
- CHANGELOG untouched (ownership list forbids it) — the gate should fold both items into the batch-6 entry.
- Commit `b3b73d3` (NOT pushed): WheelsComponent.cpp, EnvelopeDisplay.{h,cpp}, keyboard_view_test.cpp, editor_test.cpp.

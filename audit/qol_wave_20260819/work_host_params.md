# Host Parameter Display — Implementation Report

## 1. Value-to-text (host automation display)
Every `AudioParameterInt` in `createParvatiParameterLayout` now carries
`withStringFromValueFunction` + `withValueFromStringFunction` (API verified:
`juce_AudioParameterInt.h` / `RangedAudioParameterAttributes`, JUCE 9.0.1).

**Formatters stayed where they were** — no move needed: `ui/SynthParamLabels.h/.cpp`
and `ui/FxSlotLabels.h/.cpp` are juce_core-only (their transitive includes are
`juce_core`/`juce_dsp`/pure C++; no `juce_gui`), so `ParameterLayout.cpp`
includes them directly. They remain pure LUT/math — callable from arbitrary
host threads.

Wiring by family (dispatch in `ParameterLayout.cpp`):
- **Synth ints** → `paramValueTextSynth(id, v)` (Hz / ms / ∞ / ct / % / note names; unmatched → raw int).
- **`fx{N}_param1..5`** → `paramValueText(FxType, idx, v)` via a **sibling lookup**: the lambda captures the slot's `fx{N}_type` `AudioParameterChoice*` (stashed at creation; `getIndex()` is an atomic load → thread-safe, display-stale-at-worst; both params share the APVTS lifetime).
- **`fx{N}_drywet`, `fx_mix`** → "NN%"; typed "100" → 127.
- **`fx_eq_low/mid/high`** → `fxEqLowToString`/`fxEqDbToString`, **hoisted from `FxRoutingBar.cpp`'s anonymous namespace into `FxSlotLabels`** (gui-free); `FxRoutingBar` now forwards — one implementation for UI knobs and host text. Typed "+6" → 96 (+6 dB).
- **`fx{N}_enabled`** → Off/On (+ On/off text entry).
- **`fxmod{M}_amount`** → ±NN%; typed "100" → 63.
- **`fx_order`: skipped by design** — internal chain-permutation index, no meaningful unit (stays raw).
- Choice params untouched (choice list carries text); `ParameterID {id, 1}` versioning unchanged.

## 2. Grouping → VST3 Units / AU lists
13 `AudioProcessorParameterGroup`s (osc/mix/filter/env/lfo/mod/modif/part/seq/arp/global/fx/fxmod), created lazily in **first-appearance order**, mirroring `sectionForId` prefix rules (incl. exact-id Global checks before the `filter` prefix; `fxmod` before `fx`, `modif` before `mod`; env-lfo split). Verified consumers: VST3 `setupParameters()` → Units; AU `addParameters()` → grouped lists.

## 3. Order stability (IMPORTANT)
Within-group order == descriptor-table order exactly. Two spans of the FLATTENED list permute (same members, same counts): the 23 env+lfo params (LFOs follow the 12 ADSR) and the 84-param part..global span (`part_select` joins Part; `vca_curve`/`filter_card`/`filter_drive` close Global). **Absolute indices are identical outside those spans** — through `modif4_op` (97) and from `fx1_type` (182) onward. All shipped wrappers reference params by string/hash ID (VST3 string ids; AU/AUv3 `hashCode` of id — verified in `juce_audio_plugin_client`), so saved automation + APVTS state are unaffected.

## 4. Validation
- `clang++ -fsyntax-only` with the exact `build_release` compile flags: PASS for all 4 touched sources (build runs were out of scope).
- New test `tests/host_param_text_test.cpp` (+ CMake target `parvati_host_param_text_test`): group structure/order, per-group descriptor order, flattened-order containment (prefix + fx-tail pinned positionally), ~25 text/parse assertions incl. the sibling-type switch (PitchShifter "+12.0" / Overdrive "16.0x" / ClockedDelay "1/16" / None "50%"), part_select still non-automatable. **Not executed** (no builds allowed): run `cmake --build build --target parvati_host_param_text_test && ./build/parvati_host_param_text_test`.

## 5. Risks
- Typed-entry percent scaling can wobble ±1 step at half-values (50% → 64 → displays 50%); matches UI rounding.
- Host text for `fx{N}_paramK` trails an in-flight type change by one param write (atomic read) — display-only.
- `filter1_cutoff` text is the base-cutoff approximation (same as the UI knob), not key-tracked.

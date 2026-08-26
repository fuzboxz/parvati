# Static Analysis & Audit

Hellcat is checked with three complementary tools. **No real defects were
found**. The only findings are style issues in the faithful
firmware/controller ports (exempt by policy) and a small set of documented
clang-tidy false positives (NOLINT'd in source with a reason).

## Baseline gate (2026-08-26)

`tools/run_static_analysis.sh` FAILS on any cppcheck finding whose signature
is absent from `tools/static_analysis_allowlist.txt`. The baseline holds the
historical debt: 30 signatures that cover 53 raw findings (repeated lines of
the same defect collapse into one line). Owners:

* `Source/dsp/oscillator.cpp` — 15 error-severity `uninitStructMember`
  (`phase.carry`). The firmware-faithful 24-bit phase struct leaves the carry
  uninitialised on purpose. Fix is separate work; do not touch it casually.
* Vendored trees — `Source/dsp/clouds/*`, `ambika_reference/*`, the clap
  helpers under `_deps`.
* `Source/dsp/fixed_math.h` — `shiftNegativeLHS` x2: int8 left-shift that
  matches the firmware arithmetic. Runtime-clean under ASan/UBSan sweeps.
* The rest — style/perf findings in `tests/*` and two Source style notes.

Rules:
* A NEW finding fails the gate. Fix the code, or extend the baseline
  consciously (review first).
* Line numbers are NOT part of a signature. Edits that shift lines keep the
  baseline valid.
* Regenerate signatures after an intentional re-baseline:
  `HELLCAT_STATIC_ALLOWLIST_REGEN=1 tools/run_static_analysis.sh`.

## How to reproduce

From a configured build dir (it produces `compile_commands.json`
automatically):

```bash
# 1. Sanitizers (whole build, incl. JUCE) — run the test suite under ASan+UBSan.
cmake -S . -B build_asan -DPARVATI_ENABLE_ASAN=ON -DPARVATI_ENABLE_UBSAN=ON
cmake --build build_asan -j 8
(cd build_asan && for t in hellcat_*_test; do ASAN_OPTIONS=detect_leaks=0 ./$t; done)

# 2. clang-tidy (bug-focused; see .clang-tidy for the curated check set).
/opt/homebrew/opt/llvm/bin/clang-tidy -p build <file>

# 3. cppcheck.
cppcheck --enable=warning,style,unusedFunction -DDEBUG -D_DEBUG \
         -I Source -I "$HOME/JUCE/modules" --std=c++17 Source
```

## Results (audit pass)

### Sanitizers (ASan + UBSan)
All headless tests run **clean** — no heap/stack buffer overflows, no
use-after-free, no undefined behaviour. (LeakSanitizer has no support on
Apple Silicon; Hellcat relies on JUCE's `JUCE_LEAK_DETECTOR` for leak checks
instead.)

### clang-tidy
The **JUCE-wrapper code** (`Source/PluginProcessor.*`, `SynthEngine.*`,
`AmbikaVoice.*`, `PluginEditor.*`, `Source/ui/*`, `ParameterLayout.*`,
`MidiParameterMap.*`, `PatchFile.*`) is **clean of bugprone-/cert- defects**
after the audit pass, which fixed:
- C-style casts → `static_cast` (`SynthEngine.h`).
- A `switch` on an `int` without a `default` → explicit `default: break`
  (`ui/ParamHelp.cpp`).

(Residual `modernize-avoid-c-style-cast` findings remain in the `.cpp` files
— e.g. `SynthEngine.cpp`, `PluginProcessor.cpp`, `ParameterLayout.cpp`.
These are all correct functional casts and are style-only, not defects. They
stay in place to keep the diffs minimal.)

Documented false positives (NOLINT'd in source with a reason):
- `bugprone-infinite-loop` — fires on `for (i=1; i<=6; ++i)` and
  `while (note>127) note-=12` style loops (finite; a known clang-tidy FP on
  unsigned counters).
- `bugprone-branch-clone` — fires on faithful controller-port if-chains
  whose branches are in fact distinct (`Arpeggiator`, `Sequencer`,
  `SynthEngine` transport start/stop).

The **faithful firmware/controller ports** (`Source/dsp/*`,
`Arpeggiator.*`, `Sequencer.*`) emit expected style findings (redundant
`inline`, enum base sizes, `auto`/`using` suggestions) and are **exempt** —
see `CONTRIBUTING.md`. `performance-enum-size` is deliberately excluded from
`.clang-tidy` because the `Patch` struct is serialized byte-for-byte into
`.PRO`/`.MUL` files. A change of enum base types would alter `sizeof(Patch)`
and corrupt the patch format.

### cppcheck
- `uninitStructMember: phase.carry` — false positive. `U24AddC` returns a
  fully initialised struct; `phase.integral/fractional` are seeded from the
  persistent member in `BEGIN_SAMPLE_LOOP`, and the code sets `carry` before
  it reads it.
- `arrayIndexOutOfBounds formant_amplitude[3]` — **intentional** firmware
  alias: `formant_amplitude[3]` overlays the adjacent `noise_modulation`
  member (the struct layout guarantees the adjacency), faithfully matching
  the firmware.
- `unusedFunction` (`setDirection`/`clockTick`/`start`) — false positives:
  cppcheck is translation-unit-local and does not see the cross-TU calls
  (`setDirection` is called from `PluginProcessor.cpp`, `clockTick` from
  `SynthEngine.cpp`).

## Goal of the user's "unwired / never-used variables" concern

A first audit pass reported "no dead/unwired code". An **independent**
(ASan/UBSan + clang-tidy + cross-TU grep) audit then found and **removed**
the following confirmed-dead code (each item was verified to have zero
callers across `Source/`, `tests/`, `tools/`):

- **Write-only state:** `SynthEngine` members `globalWheel_` /
  `globalBreath_` / `globalFoot_` (written in `handleController`, never read
  — the mod write goes through `applyGlobalModSource` on a separate path).
- **Dead accessors:** `ThemeManager` persistence/index API (`getNumThemes`,
  `getCurrentIndex`, `selectByIndex`, `toValueTree`, `fromValueTree`,
  `getCurrentThemeName` — superseded by the processor's `uiThemeName_`);
  `NoteStack::max_size`; `TransportClock::getBpm`/`getSamplesPerTick`;
  `KeyboardView::setBaseOctaveNote`; the four `getArp*Choices` wrappers in
  `ParameterLayout` (the descriptor table builds the lists inline via
  `makeArp*()`).
- **Unused field:** `ambika::dsp::Lfo::step_` (never written or read).
- **Dead computation / unused captures:** the `fourPole` local in
  `AnalogFilter::applyParams` (computed then discarded) and two unused
  `[this]` lambda captures in `PluginEditor.cpp`.

Every public engine/processor method, every APVTS parameter, and every GUI
control is now reachable (the latter two are enforced structurally — the GUI
is generated from the descriptor table, and `hellcat_editor_coverage_check`
asserts that every descriptor has a control + attachment).

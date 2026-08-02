# Static Analysis & Audit

Parvati is checked with three complementary tools. **No real defects were
found**; the only findings are style nits in the faithful firmware/controller
ports (exempt by policy) and a small set of documented clang-tidy false
positives (NOLINT'd in source with a reason).

## How to reproduce

From a configured build dir (produces `compile_commands.json` automatically):

```bash
# 1. Sanitizers (whole build, incl. JUCE) — run the test suite under ASan+UBSan.
cmake -S . -B build_asan -DPARVATI_ENABLE_ASAN=ON -DPARVATI_ENABLE_UBSAN=ON
cmake --build build_asan -j 8
(cd build_asan && for t in parvati_*_test; do ASAN_OPTIONS=detect_leaks=0 ./$t; done)

# 2. clang-tidy (bug-focused; see .clang-tidy for the curated check set).
/opt/homebrew/opt/llvm/bin/clang-tidy -p build <file>

# 3. cppcheck.
cppcheck --enable=warning,style,unusedFunction -DDEBUG -D_DEBUG \
         -I Source -I "$HOME/JUCE/modules" --std=c++17 Source
```

## Results (audit pass)

### Sanitizers (ASan + UBSan)
All headless tests run **clean** — no heap/stack buffer overflows, no
use-after-free, no undefined behaviour. (LeakSanitizer is unsupported on Apple
Silicon; Parvati relies on JUCE's `JUCE_LEAK_DETECTOR` for leak checks instead.)

### clang-tidy
The **JUCE-wrapper code** (`Source/PluginProcessor.*`, `SynthEngine.*`,
`AmbikaVoice.*`, `PluginEditor.*`, `Source/ui/*`, `ParameterLayout.*`,
`MidiParameterMap.*`, `PatchFile.*`) is **clean** after the audit pass, which
fixed:
- C-style casts → `static_cast` (`SynthEngine.h`).
- A `switch` on an `int` without a `default` → explicit `default: break`
  (`ui/ParamHelp.cpp`).

Documented false positives (NOLINT'd in source with a reason):
- `bugprone-infinite-loop` — fires on `for (i=1; i<=6; ++i)` and
  `while (note>127) note-=12` style loops (finite; a known clang-tidy FP on
  unsigned counters).
- `bugprone-branch-clone` — fires on faithful controller-port if-chains whose
  branches are in fact distinct (`Arpeggiator`, `Sequencer`, `SynthEngine`
  transport start/stop).

The **faithful firmware/controller ports** (`Source/dsp/*`, `Arpeggiator.*`,
`Sequencer.*`) emit expected style noise (redundant `inline`, enum base sizes,
`auto`/`using` suggestions) and are **exempt** — see `CONTRIBUTING.md`.
`performance-enum-size` is deliberately excluded from `.clang-tidy` because the
`Patch` struct is serialized byte-for-byte into `.PRO`/`.MUL` files; changing
enum base types would alter `sizeof(Patch)` and corrupt the patch format.

### cppcheck
- `uninitStructMember: phase.carry` — false positive. `U24AddC` returns a fully
  initialised struct; `phase.integral/fractional` are seeded from the persistent
  member in `BEGIN_SAMPLE_LOOP`, and `carry` is set before it is read.
- `arrayIndexOutOfBounds formant_amplitude[3]` — **intentional** firmware alias:
  `formant_amplitude[3]` overlays the adjacent `noise_modulation` member (the
  struct layout guarantees the adjacency), faithfully matching the firmware.
- `unusedFunction` (`setDirection`/`clockTick`/`start`) — false positives:
  cppcheck is translation-unit-local and does not see the cross-TU calls
  (`setDirection` is called from `PluginProcessor.cpp`, `clockTick` from
  `SynthEngine.cpp`).

## Goal of the user's "unwired / never-used variables" concern

The audit specifically looked for dead/unwired code. **None was found** in the
active code paths: every public engine/processor method, every APVTS parameter,
and every GUI control is reachable (the latter two are enforced structurally —
the GUI is generated from the descriptor table, and `parvati_editor_test`
asserts every descriptor has a control + attachment).

# Parvati

**Parvati** is a software synthesizer — a faithful, modern JUCE port of the
[Mutable Instruments **Ambika**](https://github.com/pichenettes/ambika)
hybrid polysynth. It recreates Ambika's 6-voicecard multitimbral engine
(oscillators, mixer, analog-modeled filters, 3 envelope/LFO units, modulation
matrix, step sequencer, arpeggiator) as a VST3 / AU / Standalone plugin, and
loads the original Ambika `.PRO` (program) and `.MUL` (multi) patch files.

> Parvati is a **derivative work** of the GPL-3.0 Ambika firmware and is
> therefore licensed under the **GNU GPL v3.0**. See [`LICENSE`](LICENSE) and
> [`NOTICES.md`](NOTICES.md). The bundled factory presets are the GPL-3.0 Ambika
> "goldencard" banks by Emilie Gillet (see [`presets/ATTRIBUTION.md`](presets/ATTRIBUTION.md)).

---

## Features

- **Faithful DSP** — integer/bit-exact port of the Ambika voicecard engine
  (`Source/dsp/`), including the 3 selectable filter topologies (4-pole LM13700,
  4-pole SSM2164, 2-pole SVF) and the fixed 39216 Hz internal rate with
  Lagrange-resampled output.
- **6-part multitimbral** — up to 6 Parts, each with its own MIDI channel, key
  zone, voice allocation, patch, arpeggiator and step sequencer.
- **Ambika patch compatibility** — load/save `.PRO` programs and `.MUL` multis;
  round-trip verified (see `tests/roundtrip_test.cpp`).
- **Controller extras** — host-tempo arpeggiator, step sequencer, MPE, multi-output
  buses (6 individual voicecard outs + main mix), undo/redo, themes, localizations,
  tooltips, parameter smoothing, optional filter oversampling.
- **Factory presets** — 128 GPL-3.0 programs + 2 multis ship embedded and are
  extracted on first run.

## Status

Experimental / beta. The audio engine and patch I/O are feature-complete and
covered by a headless test suite (17 executables). Treat it as a work in progress.

## Requirements

- **CMake** ≥ 3.22
- A C++17 compiler (clang ≥ 14 / gcc ≥ 11 / MSVC 2022 recommended)
- **JUCE** 8 or 9 (default lookup: `$HOME/JUCE`, or `%USERPROFILE%\JUCE`
  on native Windows; override with `-DJUCE_GLOBAL_PATH=/path/to/JUCE`)

## Building

Debug build (the only build this project is developed/tested with):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 8
```

The plugin artefacts land in `build/Parvati_artefacts/Debug/`
(`Parvati.vst3`, `Parvati.component`, `Parvati.app`). The headless test
executables land in `build/`.

> The **AU** format is macOS-only. On Windows and Linux, JUCE builds the
> **Standalone** app and **VST3** only (no `.component`).

### Windows

The same CMake commands work with Visual Studio 2022 (Developer PowerShell or
the *x64 Native Tools* prompt). The default JUCE lookup falls back to
`%USERPROFILE%\JUCE` when `HOME` is unset:

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DJUCE_GLOBAL_PATH=C:/path/to/JUCE
cmake --build build --config Debug -j 8
```

(The sanitizer and `-Werror` options are GCC/Clang-only; they no-op under MSVC.)

## Running the tests

All tests build by default alongside the plugin:

```bash
cmake --build build -j 8
# run the whole suite:
for t in build/parvati_*_test; do "$t"; done
```

Key tests: `parvati_roundtrip_test` (patch save/load), `parvati_idle_silence_test`
(startup-rumble regression), `parvati_editor_test` (GUI coverage + layout sanity),
`parvati_headless_test` (audio produces sound).

## Memory safety / static analysis (opt-in)

```bash
# Address + UndefinedBehavior sanitizer (whole build, incl. JUCE):
cmake -S . -B build_asan -DPARVATI_ENABLE_ASAN=ON -DPARVATI_ENABLE_UBSAN=ON
cmake --build build_asan -j 8 && (cd build_asan && for t in parvati_*_test; do "$t"; done)

# clang-tidy (needs compile_commands.json, generated automatically):
/opt/homebrew/opt/llvm/bin/run-clang-tidy -p build Source/*.cpp
```

Other CMake options: `PARVATI_ENABLE_TSAN`, `PARVATI_WARNINGS_AS_ERRORS`.

## Documentation

- [`docs/VOICE_DESIGN.md`](docs/VOICE_DESIGN.md) — *The Design of the Ambika Digital Voice*: a deep,
  illustrated walkthrough of the hybrid voice architecture (in the spirit of
  *The Design of the Juno DCO*)
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — codebase map & developer guide
- [`docs/DSP_PORT_SPEC.md`](docs/DSP_PORT_SPEC.md) — the firmware→JUCE port spec
- [`docs/UI_MODERNIZATION_PLAN.md`](docs/UI_MODERNIZATION_PLAN.md) — UI design notes
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — build, test & style conventions

## Credits

- **Ambika** original design, firmware & sound design: **Emilie Gillet** (Mutable
  Instruments) — [github.com/pichenettes/ambika](https://github.com/pichenettes/ambika)
- **Parvati** port: 805LABS

## License

Copyright © 2024 805LABS. Licensed under the **GNU GPL v3.0**. See
[`LICENSE`](LICENSE). Third-party notices in [`NOTICES.md`](NOTICES.md).

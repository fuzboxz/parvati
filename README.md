# Parvati

**Parvati** is a software synthesizer — a plugin port of the
[Mutable Instruments **Ambika**](https://github.com/pichenettes/ambika)
hybrid polysynth, recreated as a modern VST3 / AU / Standalone plugin.

## Goal

A modernized Ambika that runs natively in your DAW, supporting every feature
of the original hardware and more — without the need for the original
microcontroller / voicecard hardware.

## Tech stack

- **JUCE 9** framework (VST3 / AU / Standalone targets)
- C++17, CMake build system
- Integer/bit-exact port of the Ambika voicecard DSP

## Features

- Faithful 6-voicecard multitimbral engine (oscillators, mixer, filters,
  3× envelope/LFO, modulation matrix, step sequencer, arpeggiator)
- All 3 Ambika filter topologies (4-pole LM13700, 4-pole SSM2164, 2-pole SVF)
- Loads/saves original Ambika `.PRO` (program) and `.MUL` (multi) patch files
- 6-part multitimbral with per-part MIDI channel, key zone & voice allocation
- Host-tempo sync, MPE, multi-output buses (6 individual outs + main mix)
- Factory preset banks bundled (GPL-3.0 "goldencard" banks)

## Build & install (macOS)

### Requirements

- **CMake** ≥ 3.22 (`brew install cmake`)
- Xcode command-line tools (`xcode-select --install`)
- **JUCE 9** checked out at `~/JUCE` (clone JUCE 9.0.0:
  `git clone --branch 9.0.0 https://github.com/juce-framework/JUCE ~/JUCE`)

### Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Artifacts land in `build/Parvati_artefacts/Release/`:
`Parvati.vst3`, `Parvati.component`, and `Parvati.app`.

### Install into your DAW

```bash
# VST3
cp -R build/Parvati_artefacts/Release/VST3/Parvati.vst3 ~/Library/Audio/Plug-Ins/VST3/

# AU (Audio Unit)
cp -R build/Parvati_artefacts/Release/AU/Parvati.component ~/Library/Audio/Plug-Ins/Components/

# Standalone app (optional)
cp -R build/Parvati_artefacts/Release/Standalone/Parvati.app /Applications/
```

Restart your DAW and rescan plugins. To run the AU for the first time you may
need to clear the quarantine attribute:

```bash
xattr -cr ~/Library/Audio/Plug-Ins/Components/Parvati.component
```

---

Parvati is licensed under the **AGPL-3.0** (own code) and is a derivative work
of the GPL-3.0 Ambika firmware (the DSP under `Source/dsp/` retains GPL-3.0).
See [`LICENSE`](LICENSE) and [`NOTICES.md`](NOTICES.md).

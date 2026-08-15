# Parvati

**Parvati** is a software synthesizer — a plugin port of the
[Mutable Instruments **Ambika**](https://github.com/pichenettes/ambika)
hybrid polysynth, recreated as a modern VST3 / AU / CLAP / Standalone plugin.

## Goal

A modernized Ambika that runs natively in your DAW, supporting every feature
of the original hardware and more — without the need for the original
microcontroller / voicecard hardware.

## Tech stack

- **JUCE 9** framework (VST3 / AU / CLAP / Standalone targets)
- C++17, CMake build system
- Integer/bit-exact port of the Ambika voicecard DSP

## Features

- Faithful 6-voicecard multitimbral engine (oscillators, mixer, filters,
  3× envelope/LFO, modulation matrix, step sequencer, arpeggiator)
- All 3 Ambika filter topologies (4-pole LM13700, 4-pole SSM2164, 2-pole SVF)
- Loads/saves original Ambika `.PRO` (program) and `.MUL` (multi) patch files
- 6-part multitimbral with per-part MIDI channel, key zone & voice allocation
- **Per-part microtonal tuning** — the 32 firmware scale presets, custom
  12-entry per-note-class tables, and Scala `.scl`/`.kbm` import
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
`Parvati.vst3`, `Parvati.component`, `Parvati.clap`, and `Parvati.app`.

### Install into your DAW

```bash
# VST3
cp -R build/Parvati_artefacts/Release/VST3/Parvati.vst3 ~/Library/Audio/Plug-Ins/VST3/

# AU (Audio Unit)
cp -R build/Parvati_artefacts/Release/AU/Parvati.component ~/Library/Audio/Plug-Ins/Components/

# CLAP
cp -R build/Parvati_artefacts/Release/CLAP/Parvati.clap ~/Library/Audio/Plug-Ins/CLAP/

# Standalone app (optional)
cp -R build/Parvati_artefacts/Release/Standalone/Parvati.app /Applications/
```

(On macOS the one-command `cmake --build build_release --target deploy` builds
all formats, renders `./screens`, and installs VST3 + AU + CLAP for you.)

Restart your DAW and rescan plugins. To run the AU for the first time you may
need to clear the quarantine attribute:

```bash
xattr -cr ~/Library/Audio/Plug-Ins/Components/Parvati.component
```

## Build (Linux)

Same CMake flow, with JUCE's Linux development packages installed (X11/XInput, ALSA, JACK, freetype/fontconfig, OpenGL):

```bash
sudo apt install libasound2-dev libx11-dev libxcomposite-dev libxcursor-dev \
  libxext-dev libxi-dev libxinerama-dev libxrandr-dev libxrender-dev \
  libfreetype6-dev libfontconfig1-dev libglu1-mesa-dev libjack-jackd2-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target Parvati_VST3 Parvati_CLAP Parvati_Standalone -j
```

Linux builds VST3, CLAP, and Standalone (the classic AU is macOS-only).
User-level install targets are `~/.vst3` and `~/.clap`; the standalone binary
lands in `build/Parvati_artefacts/Release/Standalone/Parvati`.

## Microtonal tuning

Every part has its own tuning (the **Tune** column on the Patch page):

- **12-EDO** (default) or one of the **32 firmware scale presets** — the
  Ambika's own "raga" tables, restored verbatim (a dropped-by-the-port
  firmware feature). Presets round-trip `.PRO`/`.MUL` files, so they also
  play back on the hardware. Scale-muted note classes (some presets) are
  refused exactly like the hardware.
- **Custom…** opens a per-part editor: twelve note-class offsets in steps of
  1 unit = 1/128 semitone (≈ 0.78 ¢ — the oscillator's actual resolution;
  the readout never promises finer), double-click a row to reset it, and
  **Import .scl/.kbm…** converts a Scala tuning file into the table.
- Scala import stays honest to the hardware rather than approximating:
  12-key octave-repeating mappings only (other sizes, non-octave periods
  such as Bohlen-Pierce, and over-long keymaps are rejected with the
  reason), offsets are clamped to ±127 with a per-class warning, unmapped
  classes become the firmware's muted-note behaviour, and a 432 Hz-style
  reference pitch folds in exactly.
- Caveats: custom tables (unlike presets) do not export to `.MUL`/`.PRO`
  (the formats have no field for them — the export falls back to the part's
  Scale preset byte or 12-EDO, and the export dialog says so); arpeggiator
  octave shifts transpose by one scale period; scale offsets also shift
  filter key tracking and the NOTE modulation source (the offsets apply to
  the triggered pitch, as on the hardware).

---

Parvati is licensed under the **AGPL-3.0** (own code) and is a derivative work
of the GPL-3.0 Ambika firmware (the DSP under `Source/dsp/` retains GPL-3.0).
See [`LICENSE`](LICENSE) and [`NOTICES.md`](NOTICES.md).

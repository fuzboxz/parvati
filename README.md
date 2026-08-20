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

- Faithful Ambika multitimbral engine (oscillators, mixer, filters,
  3× envelope/LFO, modulation matrix, step sequencer, arpeggiator) with
  **per-part voices 1..16 from a 96-voice pool** — Mono / Poly / Unison /
  Multitimbral / Drum Kit voice/part configuration presets (0-voice parts
  are first-class; anything else reads back as Custom). The hardware's 6
  voicecards are a derived internal detail (individual outputs + `.MUL`
  export), not a user-facing cap.
- All 3 Ambika filter topologies (4-pole LM13700, 4-pole SSM2164, 2-pole SVF)
- Loads/saves original Ambika `.PRO` (program) and `.MUL` (multi) patch files
- 6-part multitimbral with per-part MIDI channel, key zone & voice count
- **Per-part microtonal tuning** — the 32 firmware "raga" scale presets
  (plus 12-EDO), restored verbatim from the Ambika controller and
  round-tripping `.PRO`/`.MUL`
- Host-tempo sync, MPE, multi-output buses (6 individual outs + main mix)
- Factory preset banks bundled (GPL-3.0 "goldencard" banks)
- **iOS 14+ AUv3 + Standalone** build (iPad-only, landscape) sharing the same
  engine and presets container as the desktop plugin

## Requirements

- **macOS 13 (Ventura) or newer, Apple Silicon (arm64)**. Intel/x86_64 is not
  shipped (Rosetta is being phased out by Apple); a universal build is still
  possible from source with `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`.
- **iOS 14+** for the AUv3 app-extension + Standalone build (iPad-only,
  landscape-only — the dense editor layout has no portrait form). The iOS
  build is not yet App-Store shipped; see `audit/release_readiness.md`.
- Build-from-source additionally needs **JUCE 9** (9.0.1 was the version
  developed against) at `~/JUCE` and CMake ≥ 3.22. Linux builds VST3 + CLAP
  + Standalone (no AU).

## Build & install (macOS)

### Build requirements

- **CMake** ≥ 3.22 (`brew install cmake`)
- Xcode command-line tools (`xcode-select --install`)
- **JUCE 9** checked out at `~/JUCE` (clone JUCE 9.0.1:
  `git clone --branch 9.0.1 https://github.com/juce-framework/JUCE ~/JUCE`;
  override with `-DJUCE_GLOBAL_PATH=/path/to/JUCE`)

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
all formats, renders `./screens`, and installs VST3 + AU + CLAP into
`~/Library/Audio/Plug-Ins` **and the Standalone into `/Applications`** for you.)

Restart your DAW and rescan plugins.

Binaries built locally with `deploy` are ad-hoc signed; macOS Gatekeeper
may quarantine files downloaded/copied around, in which case clear the
attribute once:

```bash
xattr -cr ~/Library/Audio/Plug-Ins/Components/Parvati.component
```

For distribution, use [`tools/release/sign_and_notarize.sh`](tools/release/)
instead — Developer-ID signing + notarization + stapling, so end users never
see a Gatekeeper prompt.

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
- Arpeggiator octave shifts transpose by one scale period; scale offsets
  also shift filter key tracking and the NOTE modulation source (the
  offsets apply to the triggered pitch, as on the hardware).

> **Custom / Scala tuning was removed** (2026-08): an earlier build offered
> custom 12-entry per-note-class tables with `.scl`/`.kbm` Scala import.
> It was removed to keep tuning hardware-faithful to the Ambika. Old presets,
> host states, and `.parvati` files that carried a custom table still load —
> they fall back to 12-EDO (or the part's scale-preset byte).

---

Parvati is licensed under the **AGPL-3.0** (own code) and is a derivative work
of the GPL-3.0 Ambika firmware (the DSP under `Source/dsp/` retains GPL-3.0).
See [`LICENSE`](LICENSE) and [`NOTICES.md`](NOTICES.md).

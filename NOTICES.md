# Third-Party Notices

This project includes or depends on the following third-party software.

## Parvati (this project)

Copyright © 2026 Jozsef Ottucsak.

Licensed under the **GNU Affero General Public License v3.0** (AGPL-3.0). The
full text is in [`LICENSE`](LICENSE). Parvati's own code is AGPL-3.0; the
combined work (including the GPL-3.0 Ambika-derived DSP and factory banks
below) is distributed under AGPL-3.0, which is one-way compatible with GPL-3.0.

> Note: AGPL-3.0 adds a network-use clause (§13) on top of GPL-3.0 — if you let
> users interact with Parvati over a network, you must offer them the
> corresponding source.

## Ambika firmware (Mutable Instruments)

Parvati is a software port of the **Ambika** synthesizer by
**Emilie Gillet / Mutable Instruments**.

- Project: <https://github.com/pichenettes/ambika>
- License: **GPL-3.0** (firmware) / CC-BY-SA 3.0 (PCB, schematics, docs)
- Author: Emilie Gillet (emilie.o.gillet@gmail.com)

The DSP engine under `Source/dsp/` is a faithful C++17 port of the Ambika
voicecard firmware and **retains its GPL-3.0 license** (upstream-derived, not
relicensed). GPL-3.0 is compatible with this project's AGPL-3.0. The
`ambika_reference/` tree (kept locally, not part of the tracked source) is the
original GPL-3.0 firmware used as the porting reference.

Integrity pin: `tools/check_reference_integrity.sh` checks a SHA-256 manifest
of every content file in this tree. The manifest lives at
`tools/reference_integrity.sha256`; the ctest `parvati_check_reference_integrity`
runs the check in every configured tree. Re-pinning is deliberate. Run
`PARVATI_PIN=1 tools/check_reference_integrity.sh` only for an intended
upstream sync. Re-pinning after an accidental edit destroys the parity oracle.

The controller-side scale ("raga") tuning tables in `Source/TuningTables.cpp`
are likewise vendored verbatim from the Ambika controller firmware
(`controller/resources.cc`, GPL-3.0, upstream-derived) — a mechanical
transcription of the 30 scale tables + the 32-entry dispatch used by Parvati's
per-part microtonal tuning.

## Mutable Instruments Eurorack DSP (clouds / rings / warps / stmlib, MIT)

The per-part FX chain embeds DSP vendored from four **Emilie Gillet /
Mutable Instruments** Eurorack module firmwares, under
`Source/dsp/clouds/`:

| Upstream repo | Used for |
|---|---|
| <https://github.com/pichenettes/clouds> | Diffuser, Pitch Shifter, Looping Delay, WSOLA Stretch, Spectral (phase vocoder), Clouds reverb (CVerb) |
| <https://github.com/pichenettes/rings> | Resonator (modal resonator + limiter) |
| <https://github.com/pichenettes/warps> | Wavefolder / Ring Modulator transfer curves + the 6× polyphase sample-rate converters |
| <https://github.com/pichenettes/stmlib> | Shared support DSP (filters, interpolators, FFT, LUTs, buffers) |

- Copyright © 2012–2015 Emilie Gillet (emilie.o.gillet@gmail.com)
- License: **MIT** — the full permission notice is retained in every vendored
  file header (e.g. `Source/dsp/clouds/stmlib/stmlib.h`). MIT is compatible
  with this project's AGPL-3.0.

The sources are kept verbatim for upstream sync, with a small number of
localised portability patches documented in `CMakeLists.txt` and inline
(e.g. the `stmlib/dsp/dsp.h` portability patch; the Warps
`sample_rate_converter.h` circular fast-path removal and the
`quadrature_oscillator.h` phase-clamp; the Clouds
`looping_sample_player.h` `frac16FromQ12` signed-overflow fix).

## Ambika factory presets ("goldencard")

The bundled factory patches under `presets/Factory/*.PRO` and
`presets/FactoryMulti/*.MUL` are the Ambika "goldencard" factory banks by
Emilie Gillet / Mutable Instruments, redistributed unmodified under the GPL-3.0.
See [`presets/ATTRIBUTION.md`](presets/ATTRIBUTION.md).

## JUCE

Parvati is built with the **JUCE 9** framework.

- Project: <https://github.com/juce-framework/JUCE>
- License: the JUCE framework modules are **dual-licensed under the
  [AGPLv3](https://www.gnu.org/licenses/agpl-3.0.en.html) and the commercial
  [JUCE 9 licence](https://juce.com/legal/juce-9-licence/)** (see JUCE's
  `LICENSE.md` for the authoritative terms and its bundled-dependency list —
  ISC/BSD/MIT/zlib apply to individual bundled dependencies and the JUCE
  *examples*, not to the framework modules as a whole).

Parvati consumes the JUCE modules under the **AGPLv3** terms: Parvati itself
is AGPL-3.0, so the combination is license-compatible, and binary
distributions of Parvati must continue to satisfy AGPL-3.0's
corresponding-source obligations (§6/§13). Building or distributing Parvati
under the commercial JUCE licence instead is a choice for downstream
redistributors, on their own account.

## clap-juce-extensions (CLAP format)

The CLAP plugin format is built via **clap-juce-extensions** (plus its
vendored CLAP SDK and clap-helpers).

- Project: <https://github.com/free-audio/clap-juce-extensions>
- License: **MIT** (Copyright 2019–2020 Paul Walker; the bundled CLAP SDK is
  MIT) — compatible with AGPL-3.0. Pinned to a specific upstream commit in
  `CMakeLists.txt` (see the `PARVATI_CLAP_EXTENSIONS_PATH` / fetch block).

## GNU Unifont (embedded font)

The UI previously embedded a **subset** of **GNU Unifont** (ASCII + Latin-1
+ a few symbols; ~18 KB of the full ~12 MB face) as `fonts/unifont_data.h`
under a "Console" font mode. That mode was removed (the UI now uses the
system default sans-serif), so shipped binaries **no longer embed the
font**; the subset and the original `fonts/unifont.ttf` remain in-tree for
reference.

- Project: <https://unifoundry.com/unifont/> / <https://github.com/multitheftauto/unifont>
- License: **GPL+** (GPL with font exception) — compatible with AGPL-3.0.
- The subset was produced with FontTools `pyftsubset` from the upstream TTF.

## Other libraries

Beyond the JUCE modules (and their bundled dependencies, per JUCE's
`LICENSE.md`) and the items above, Parvati uses only the C++17 standard
library. No further third-party libraries are linked.

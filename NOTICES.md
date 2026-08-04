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

## Ambika factory presets ("goldencard")

The bundled factory patches under `presets/Factory/*.PRO` and
`presets/FactoryMulti/*.MUL` are the Ambika "goldencard" factory banks by
Emilie Gillet / Mutable Instruments, redistributed unmodified under the GPL-3.0.
See [`presets/ATTRIBUTION.md`](presets/ATTRIBUTION.md).

## JUCE

Parvati is built with the **JUCE** framework.

- Project: <https://github.com/juce-framework/JUCE>
- License: **JUCE 8/9** — the core modules are available under the
  **ISC License** (permissive) for most uses; the closed-source / "AGPL/Commercial"
  components (Pro plugins, etc.) are not used here. See JUCE's `LICENSE.md` for
  the authoritative terms.

> If you distribute a binary built against JUCE, ensure you comply with JUCE's
> own license terms in addition to Parvati's AGPL-3.0. (Parvati's AGPL-3.0 code
> dynamically links JUCE's ISC-licensed modules; the combined work remains
> AGPL-3.0 for Parvati's source. Consult your own counsel for distribution.)

## GNU Unifont (embedded font)

The "Console" UI font mode uses a **subset** of **GNU Unifont** (ASCII + Latin-1
+ a few symbols; ~18 KB of the full ~12 MB face) embedded in
`fonts/unifont_data.h`.

- Project: <https://unifoundry.com/unifont/> / <https://github.com/multitheftauto/unifont>
- License: **GPL+** (GPL with font exception) — compatible with AGPL-3.0.
- The subset was produced with FontTools `pyftsubset` from the upstream TTF.

## Other libraries

Parvati uses only JUCE modules + the C++17 standard library. No further
third-party libraries are linked.

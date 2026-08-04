# Third-Party Notices

This project includes or depends on the following third-party software.

## Parvati (this project)

Copyright © 2026 Jozsef Ottucsak.

Licensed under the **GNU General Public License v3.0** (GPL-3.0). The full text
is in [`LICENSE`](LICENSE). Parvati is a derivative work of the Ambika firmware
(also GPL-3.0), which requires this project to be distributed under the same
license.

## Ambika firmware (Mutable Instruments)

Parvati is a software port of the **Ambika** synthesizer by
**Emilie Gillet / Mutable Instruments**.

- Project: <https://github.com/pichenettes/ambika>
- License: **GPL-3.0** (firmware) / CC-BY-SA 3.0 (PCB, schematics, docs)
- Author: Emilie Gillet (emilie.o.gillet@gmail.com)

The DSP engine under `Source/dsp/` is a faithful C++17 port of the Ambika
voicecard firmware. The `ambika_reference/` tree (kept locally, not part of the
tracked source) is the original GPL-3.0 firmware used as the porting reference.

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
> own license terms in addition to Parvati's GPL-3.0. (Parvati's GPL-3.0 code
> dynamically links JUCE's ISC-licensed modules; the combined work remains
> GPL-3.0 for Parvati's source. Consult your own counsel for distribution.)

## Other libraries

Parvati uses only JUCE modules + the C++17 standard library. No further
third-party libraries are linked.

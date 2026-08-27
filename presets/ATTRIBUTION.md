# Factory Presets — Attribution

Hellcat ships two factory content sets:

1. **The original Hellcat bank** (`presets/HFACTORY/*.yml`) — 64 single-part
   presets written for Hellcat. These are ORIGINAL Hellcat content under the
   project AGPL-3.0 license (see below). They share no data with the Ambika firmware.
2. **The Ambika "goldencard" factory banks** (`presets/AFACTORY/<BANK>/*.PRO`
   and `presets/AFACTORY_MULTI/*.MUL`) by **Emilie Gillet / Mutable
   Instruments**, taken from the Ambika firmware tree
   (`controller/data/goldencard/`).

## The Hellcat bank (HFACTORY)

64 single-part patches in the Hellcat-native `.yml` format, numbered 01-64 and
named "Category - Cryptic":

| Slots  | Category       | Count | Notes                                   |
|--------|----------------|-------|-----------------------------------------|
| 01-12  | Bass           | 12    | 1 voice; 08 is poly (8 voices); a few carry Overdrive with a dry master |
| 13-26  | Keys           | 14    | 16-voice poly; several carry an FX slot  |
| 27-38  | Lead (flat)    | 12    | 1 voice, mono, no unison stack           |
| 39-48  | Unison         | 10    | 8-voice mono unison stack plus part spread |
| 49-58  | Pad            | 10    | 16-voice poly; slow envelopes, chorus/reverb space |
| 59-64  | FX             |  6    | 8-voice; risers, sweeps, glitch, ring modulation |

Each patch asks for its own polyphony (voice count) via the `voice_slots:`
field, applied on load: bass and flat leads use one mono voice; keys and
pads max the part at 16 voices; unison leads use an 8-voice mono unison
stack; the FX presets use 8. The FX mod matrix is used across the bank so
patches stay evolving (LFO-driven FX params) and interactive
(velocity-driven FX wet/dry, and harder bass hits bring more Overdrive).

Copyright (c) 2026 805Labs Kft. These presets are part of
Hellcat and carry the project license (AGPL-3.0). They live in the repo as
the CANONICAL bank — they are hand-editable (the format carries
choice-label comments on every value). `tests/factory_bank_test.cpp` guards
them: every file must load through the real patch path, round-trip its params
byte-exactly, keep its amp routing and category polyphony and voice count,
carry FX and a reverb-or-delay outside bass, and sit in its spectral band.
Bass stays free of reverb/delay; a dirt bass may carry one Overdrive/LUT slot
with a relatively dry master (to tame the 32 kHz FV-1 dirt). To edit a preset,
change the .yml and run the test; to add one, copy a file, keep the
`NN Category - Name` numbering, and extend the test's count pin. The
installer content-syncs the bank to installed trees on every launch (edits
propagate without a version bump) and sweeps files no longer present.

## The Ambika banks — License

These presets are part of the Ambika firmware, released under the
**GNU General Public License v3.0** (GPL-3.0) by Emilie Gillet.

Hellcat itself is licensed under the **GNU Affero General Public License
v3.0** (AGPL-3.0) — see the project root [`LICENSE`](../LICENSE) and
[`NOTICES.md`](../NOTICES.md). The two are compatible in the direction used
here: GPL-3.0 material (these preset banks, and the Ambika-derived DSP) may
be combined with and distributed under AGPL-3.0, since AGPL-3.0 §13 adds
further obligations (network-use source offer) on top of GPL-3.0 without
removing any. Both are Free Software licenses; redistributing these presets
unmodified carries the GPL-3.0 notice obligation (this file), and
distributing the combined Hellcat binary/source carries the AGPL-3.0
corresponding-source obligation.

## Contents

The presets are organized into Ambika's original program banks (A/B/F/S) plus a
multi bank. A `USER/` folder is provided (empty) for your own saved presets.

| Directory             | Source                                        | Count |
|-----------------------|-----------------------------------------------|-------|
| `AFACTORY/A/*.PRO`    | `goldencard/PROGRAM/BANK/A/` (programs 000–127) | 128   |
| `AFACTORY/B/*.PRO`    | `goldencard/PROGRAM/BANK/B/` (programs 000–021) |  22   |
| `AFACTORY/F/*.PRO`    | `goldencard/PROGRAM/BANK/F/` (programs 000–079) |  80   |
| `AFACTORY/S/*.PRO`    | `goldencard/PROGRAM/BANK/S/` (programs 000–127) | 128   |
| `AFACTORY_MULTI/*.MUL`| `goldencard/MULTI/BANK/A/` (multis 000, 001)    |   2   |
| `HFACTORY/*.yml`      | *(original Hellcat content — see above)*      |  64   |
| `USER/`               | *(empty — for user-saved presets)*            |   0   |

> The Ambika firmware ships only these 2 multis (vs 486 single programs): a
> `.MUL` is a complete 6-part multitimbral *setup* (one patch + routing per
> part), not an individual sound, so the factory provided just two example
> setups. To get more, author your own (Hellcat can save `.MUL`).

At build time every factory file is embedded into the plugin binary and
extracted to the user app-data directory on first run: the Ambika banks under
`AFACTORY/` and `AFACTORY_MULTI/`, the Hellcat bank under its own `HFACTORY/`
root. `USER/` is created empty on first run for your own presets.

## Credit

- **Original sound design & firmware:** Emilie Gillet (emilie.o.gillet@gmail.com)
- **Ambika project:** https://github.com/pichenettes/ambika

These presets are redistributed unmodified from the original Ambika factory
banks, in accordance with the GPL-3.0 terms.

# Factory Presets — Attribution

The factory patches shipped with Hellcat (`presets/FACTORY/<BANK>/*.PRO` and
`presets/FACTORY_MULTI/*.MUL`) are the **Ambika "goldencard" factory banks** by
**Emilie Gillet / Mutable Instruments**, taken from the Ambika firmware tree
(`controller/data/goldencard/`).

## License

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
| `FACTORY/A/*.PRO`     | `goldencard/PROGRAM/BANK/A/` (programs 000–127) | 128   |
| `FACTORY/B/*.PRO`     | `goldencard/PROGRAM/BANK/B/` (programs 000–021) |  22   |
| `FACTORY/F/*.PRO`     | `goldencard/PROGRAM/BANK/F/` (programs 000–079) |  80   |
| `FACTORY/S/*.PRO`     | `goldencard/PROGRAM/BANK/S/` (programs 000–127) | 128   |
| `FACTORY_MULTI/*.MUL` | `goldencard/MULTI/BANK/A/` (multis 000, 001)    |   2   |
| `USER/`               | *(empty — for user-saved presets)*            |   0   |

> The Ambika firmware ships only these 2 multis (vs 486 single programs): a
> `.MUL` is a complete 6-part multitimbral *setup* (one patch + routing per
> part), not an individual sound, so the factory provided just two example
> setups. To get more, author your own (Hellcat can save `.MUL`).

At build time all `FACTORY*` files are embedded into the plugin binary and
extracted to the user app-data directory on first run
(`~/Library/Hellcat/FACTORY/...`, etc.). `USER/` is created empty on first run
for your own presets.

## Credit

- **Original sound design & firmware:** Emilie Gillet (emilie.o.gillet@gmail.com)
- **Ambika project:** https://github.com/pichenettes/ambika

These presets are redistributed unmodified from the original Ambika factory
banks, in accordance with the GPL-3.0 terms.

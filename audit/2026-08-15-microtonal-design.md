# Per-part microtonal tuning — design & feasibility audit (2026-08-15)

**Scope:** note→frequency path at HEAD; hook point for per-part tuning tables; interaction with MPE bend, arp/seq transposition, preset/state/hardware formats; UI surface; Scala vs cents-table; architecture sketch, risks, phased plan.
**Method:** read-only inspection of `Source/` (SynthEngine, AmbikaVoice, dsp/*, ParameterLayout, PatchFile, ParvatiPreset, MulExport, ui/PatchPage), the vendored firmware (`ambika_reference/`), its table generator (`controller/resources/lookup_tables.py`), and `CONTRIBUTING.md` (bit-exactness policy, line 126-130). Every claim below cites file:line. Items marked **[H]** are hypotheses/estimates, not verified facts.

## 1. The complete note→frequency path today (verified)

```
MIDI in / UI keyboard (raw note numbers)
  PluginProcessor.cpp:216 (UI midi merge), :220 (processTransport), :229 (renderNextBlock)
    │
    ▼ channel+keyzone routing
SynthEngine::noteOn/noteOff  SynthEngine.cpp:1045-1065  →  findPartForNote  SynthEngine.cpp:807-825
    │  (arp active → notes held in Part.arp; arp/seq generated notes → triggerNoteInPart
    │   via callbacks SynthEngine.cpp:46-65; direct notes → triggerNoteInPart SynthEngine.cpp:1054)
    ▼ SINGLE FUNNEL: SynthEngine::triggerNoteInPart  SynthEngine.cpp:866-955
    │   (polyphony modes, spread drift; voice-keyed by RAW note number: PolyAllocator
    │    pool_[i]&0x7f, monoStack, arp previousNote_)
    ▼
AmbikaVoice::startNote  AmbikaVoice.cpp:157-182   ← THE tuning choke point
    │   baseNote = jlimit(0,127, midiNoteNumber + partOctave_*12)          :169
    │   note14   = jlimit(0, kHighestNote, baseNote*128 + partTuning_
    │                                + spreadDrift14_)                     :170-171
    ▼
dsp::Voice::Trigger(note14) → pitch_target_/portamento  dsp/voice.cpp:174-192
    ▼ per block
dsp::Voice::RenderOscillators  dsp/voice.cpp:404-471
    │   base_pitch = glide + coarse/fine mod dst (:410-412)
    │              + pitch_bend_offset_  (live bend/MPE)  :418
    │   per-osc: range ×128, detune, mod dst ±16 st :419-427; clamp 120*128 :429-431
    ▼
lut_res_oscillator_increments[(pitch−116*128)>>1] + octave fold (num_shifts)
    dsp/voice.cpp:433-449  → 24-bit increment → f = inc/65536 × 39216 Hz
    (12-EDO baked in: lookup_tables.py:71-80, A4=440, 2^((n−69·128)/(128·12)))
```

Pitch bend / MPE: `SynthEngine::handlePitchWheel` (SynthEngine.cpp:1077-1089) converts wheel→semitones with a **fixed** ±2 st range (`bendRangeSemitones_`, SynthEngine.h:686; no MPE RPN handling), routes to active voices per channel; `AmbikaVoice::applyMpeToVoice` (AmbikaVoice.cpp:214-231) converts to 1/128-st units (`lround(semis*128)`, :219) → `Voice::set_pitch_bend_offset` (dsp/voice.h:111) added at dsp/voice.cpp:418.

## 2. Findings

- **F1 (important, fidelity gap + opportunity):** The firmware already has per-part microtonal tuning: `PartData.raga` (byte 4, ambika_reference/controller/part.h:89) selects one of ~40 scale tables (12×int16 shifts in 1/128-st units; `LUT_RES_SCALE_JUST…`, resources.h:463+; generator lookup_tables.py:151-176; 32767 = "silence this note" sentinel). `Part::TuneNote` (part.cc:634-647) applies `octave → raga[midi%12] → tuning` and `AcceptNote` (part.cc:649-659) rejects out-of-scale notes. **Parvati ports only octave+tuning**: `AmbikaVoice::startNote` (AmbikaVoice.cpp:167-173) and `setPartByte` caches only offsets 1/2/3 (AmbikaVoice.h:86-97); byte 4 is not exposed (ParameterLayout.cpp:335-341 has no `part_raga`) and no scale tables exist in `Source/dsp/resources` (resources.h:118-123, resources_data.cpp:1366-1372 list only 6 lookup tables). Verified by grep for `raga|SCALE_JUST` across `Source/` (no hits).
- **F2 (verified fact, low integration cost):** There is a **single choke point** for note-number→pitch: `AmbikaVoice::startNote` (AmbikaVoice.cpp:169-171). All sources (direct MIDI, UI keyboard via midiCollector PluginProcessor.cpp:216, arp, seq, MPE) funnel through `triggerNoteInPart` (SynthEngine.cpp:866) → `triggerVoice/retriggerVoice` (SynthEngine.cpp:849-864) → `startVoice/retriggerNote` → `startNote`. Both note-on and note-off key voices by the **raw** note number upstream, so a tuning table that only adds fine offsets (never changes the integer note) cannot break allocator/monoStack/arp matching.
- **F3 (important, hard DSP constraint):** Effective oscillator pitch resolution is **1/64 semitone (~1.56 cents)**: the 14-bit pitch is in 1/128-st units but the increment lookup drops the LSB (`ref_pitch >> 1`, dsp/voice.cpp:445; table steps of 2 units, lookup_tables.py:74 `arange(116*128, 128*128, 2)`). A tuning table cannot be more accurate than ~1.56 ct; storing 1/128-st units is fine but the last bit is inaudible. No change to `Source/dsp/` is required or recommended for tuning (keeps CONTRIBUTING.md:126-130 bit-exactness intact — the firmware itself did tuning entirely in the 14-bit note domain).
- **F4 (verified, already correct):** Order of operations tuning→bend is already right. Tuning is baked into `note14` at trigger (becomes portamento target); bend/MPE is a live additive offset applied after all note-derived pitch (dsp/voice.cpp:418). This is the correct semantics: bend stays in **absolute cents** (2^(N/12)), not scale steps; a continuous bend never re-quantizes to the scale.
- **F5 (note, existing gap on the bend path):** A voice triggered while the wheel is off-center does **not** inherit the standing bend: `handlePitchWheel` only updates `isVoiceActive()` voices (SynthEngine.cpp:1081-1088) and `startNote` ignores `currentPitchWheelPosition` (AmbikaVoice.h:60, AmbikaVoice.cpp:158); no per-channel wheel latch exists in SynthEngine. Mostly invisible on standard MIDI (wheel usually centered at note-on) but audible with MPE/latched wheels. Worth fixing alongside tuning work (store last wheel value per channel; apply in `triggerVoice`).
- **F6 (verified):** Arp/seq transposition happens in **note-number space, before the tuning hook** — arp `note += 12 * arpOctave_` with wrap `while (note>127) note -= 12` (Arpeggiator.cpp:77-80); note-seq `note = n.note + heldNote − 60` clamped (Sequencer.cpp:31-33). With an octave-periodic table indexed by `note % 12` (the raga design), "+12" automatically means "+1 scale period" — transposition in scale space **for free, no arp/seq changes**. Constraint: v1 scales must be octave-repeating; non-octave periods (Bohlen-Pierce) would need arp changes and are out of scope.
- **F7 (verified):** `partTuning_` today is a global ±~1 semitone offset in 1/128-st units (ParameterLayout.cpp:338 `-127..127`; displayed in cents at SynthParamLabels.cpp:222-229). A per-part *table* is a different beast: per-note-class offsets. They compose additively in `note14` exactly as firmware `TuneNote` did.
- **F8 (verified, format compat):** `.PRO`/`.MUL` round-trip the full 84-byte PartData (PatchFile.h:20-26, 58-66; writer PatchFile.cpp:279+), so the **raga byte already survives hardware export today** (semantically ignored). Arbitrary custom tables have **no** representation in the hardware formats (MultiData is 56 fixed bytes; MulExport only remaps voices/polyphony — MulExport.h:40-47). The `.parvati` YAML multi already carries Parvati-only per-part extensions with backward-compatible `hasProperty` guards (`voice_slots`, `name` — ParvatiPreset.cpp:586-590 write, :655-660 load). Engine host-state blob is version 6 (SynthEngine.cpp:397 write, :461-463 strict-accept 1..6) with the length-prefixed-appendable FX-block precedent (SynthEngine.cpp:398-401).
- **F9 (verified):** Side effects of tuning-in-note-space: filter keytracking and `MOD_SRC_NOTE` follow the tuned pitch (`dst_[MOD_DST_FILTER_CUTOFF] = … + pitch_value_ − 8192`, dsp/voice.cpp:263-264; `MOD_SRC_NOTE = pitch_value_>>6`, dsp/voice.cpp:209) — firmware-consistent (raga shifts were inside TuneNote), arguably desirable, must be documented. Bend does **not** move keytracking (offset added after, dsp/voice.cpp:418) — existing behavior, unchanged.
- **F10 (verified):** UI precedent for per-part arrangement settings: the PatchPage part row (Cards / Voices / Ch / Zone Low / Zone High / Poly + name) — layout PatchPage.cpp:170-215, refresh :217-231, `voicesCombo_` extension pattern :383-388. Part *sound* params (`part_octave`, `part_tuning`) live in the SynthWorkspace param grid (ParamHelp.cpp:132-139).

## 3. Scala/kbm vs simple cents table — recommendation

**v1: 12-entry cents table per part (per note class, octave-repeating), plus the firmware raga presets. Not Scala.**
Rationale: (a) the DSP consumes exactly this shape — 12 offsets in 1/128-st units applied at `note%12`, resolution-capped at 1/64 st (F3), so .scl's higher precision buys nothing audible; (b) .scl with ≠12 degrees requires a key-mapping policy (.kbm or naive) — the classic source of subtle mapping bugs — pure scope creep for v1; (c) porting the ~40 firmware raga tables is simultaneously a **fidelity restoration** (F1) and instant content. Ship a `.scl` **importer** in a later phase as a *converter into* the 12-entry custom table (period assumed 2/1, degrees mapped in order, documented limits), never as the storage format. **[H]** the 12-entry model covers the owner's "different scales per part" need; if non-octave scales are a hard requirement, revisit before phase 1.

## 4. Architecture sketch

**Components**
1. `TuningTables` (new, `Source/dsp/resources` or `Source/`): the vendored firmware raga tables (12×int16 each, from ambika_reference/controller/resources.cc) + a "12-EDO" zero table. Read-only, shared, no thread issues.
2. `Part::TuningState` (SynthEngine.h, in `struct Part`): `AtomicByteArray<24>` custom offsets (12×int16 LE), `std::atomic<uint8_t> tuningMode` (0=12-EDO, 1..N=raga preset, N+1=custom), `std::atomic<bool> tuningDirty_`. Mirrors the `frameDirty_`/`fxDirty_` staging pattern (SynthEngine.h:137-141, 168-172).
3. `AmbikaVoice`: `int16_t tuneOffsets_[12] {}` + `void setTuningOffsets(const int16_t*)` — called from the audio-thread `tuningDirty_` service (extend the `frameDirty_` service loop in `processTransport`, SynthEngine.cpp:1187+; or fold into `pushPartBytesToVoices` SynthEngine.cpp:760-791). Read once in `startNote`.
4. **The hook** (single line region, AmbikaVoice.cpp:169-171):
   ```cpp
   const int baseNote = juce::jlimit (0, 127, midiNoteNumber + partOctave_ * 12);
   const int note14   = juce::jlimit (0, (int) ambika::dsp::kHighestNote,
                                      baseNote * 128 + tuneOffsets_[midiNoteNumber & 11]
                                                    + partTuning_ + spreadDrift14_);
   ```
   (Index on the **raw** incoming note like firmware `TuneNote`, part.cc:642; identical result after octave shift since `%12` is shift-invariant.) `dsp/` untouched → bit-exactness preserved; `partTuning_`, spread, bend, portamento all compose unchanged.
5. APVTS param `part_raga` (byteOffset 4, isPart, choices = preset names) — gives automation, `.parvati` params serialization, `.PRO`/`.MUL` round-trip **and hardware playback** of preset tunings for free. Custom table is engine-level (like `voiceSlots`).

**Data flow:** MT setter (`setPartTuning(part, mode, offsets)`) → `Part::TuningState` + `tuningDirty_` → AT service copies resolved 12 offsets into each voice of the part → consumed at next `startNote`. Sounding voices keep their triggered pitch (matches partOctave_/partTuning_ semantics today — change applies to new notes; document, or optionally retrigger).

**File-format fields & migration**
- `.PRO`/`.MUL`: nothing new — raga rides PartData byte 4 (already round-tripped, F8). Custom tables are dropped on export: surface a warning line in `MulExportDialog` (ui/MulExportDialog.h:22+) / `afterMultiSaved` ("Part 2 custom tuning not representable on hardware; export uses its raga byte or 12-EDO"). Policy: if a preset raga is also selected, byte 4 exports it; custom-only → export byte 4 = 0. **[H]** users accept preset-only hardware fidelity.
- `.parvati` multi: per-parts[] entry, after `name:` (ParvatiPreset.cpp:588-590):
  ```yaml
      tuning_mode: 3          # 0=12-EDO, 1..N=raga preset, N+1=custom
      tuning_offsets: [0, 12, -34, ...]   # 12 ints, 1/128-semitone units (custom only)
  ```
  Load with `hasProperty` guards (voice_slots precedent, ParvatiPreset.cpp:655-660) → old files load as 12-EDO; new files load in old builds as no-op (unknown keys ignored — ParvatiPreset.h:50-51). Patch format: `part_raga` rides `params:` automatically; custom table optionally mirrored top-level later.
- Engine state blob: bump to **v7**; append per-part length-prefixed block `{u8 mode; i16 offsets[12]}` after the name block (pattern: FX block SynthEngine.cpp:398-401 + v6 gate :461-463/:560-575). `restoreState` accepts 1..7; a v6 blob restores with 12-EDO defaults. Old plugin builds reject v7 and fall back to legacy APVTS restore — same accepted tradeoff as v5→v6 (documented in the captureState header comment, SynthEngine.cpp:372-379). Update CHANGELOG.

## 5. Ranked risks

1. **Silent tuning loss on hardware export** (custom tables unrepresentable, F8) — important; mitigate with export warning + preset-first UX.
2. **State-version fallback regression** (v7 blob in an older host loses full multitimbral state) — moderate; documented precedent, CHANGELOG entry.
3. **MT→AT staging correctness** — low if the existing dirty-flag patterns are followed verbatim; do not write voice state from the message thread (see SynthEngine.cpp:181-186 rationale).
4. **Resolution disappointment** (1.56 ct steps, F3) — document; reject UI values implying finer precision, show quantized cents.
5. **Non-octave scale expectations** (arp octave = 12 notes, F6) — v1 constraint, stated in UI copy.
6. **Keytracking/`MOD_SRC_NOTE` shift with tuning** (F9) — behavior change vs today's 12-EDO default; firmware-consistent, document in ParamHelp.
7. **Standing-bend pickup gap** (F5) — pre-existing; fix opportunistically (per-channel wheel latch applied in `triggerVoice`) so microtonal+MPE users don't blame tuning. **[H]** fix cost ~0.5 day.

## 6. Phased plan (effort **[H]**, single dev)

- **Phase 0 — fidelity restore (raga):** vendor scale tables; `part_raga` APVTS param; apply in `startNote` via resolved offsets; ParamHelp/labels; tests (table application, .MUL round-trip of byte 4, arp-period behavior). ~1–1.5 d.
- **Phase 1 — custom per-part table:** `TuningState` + AT staging + voice hook; engine-state v7; `.parvati` multi fields; `tests/tuning_test.cpp` (startNote mapping via a test-only pitch readout on AmbikaVoice — outside `dsp/`; plus one spectral spot-check following the mod_audio_test harness pattern). ~2–3 d.
- **Phase 2 — UI:** PatchPage part-row "Tune" combo (preset/custom) following `voicesCombo_` (PatchPage.cpp:383-388) + a 12-slot cents popover editor (NoteStepControl-style, 44 pt targets per the iPad HIG work); MulExport warning. ~2–3 d.
- **Phase 3 — Scala import (optional):** `.scl` parser → fills the custom table (degrees in order, period 2/1, cents→1/128-st rounding); import errors surface in-editor; tests with canonical .scl corpus. ~2–3 d.
- **Phase 4 — polish:** standing-bend pickup (F5), retune-live-notes option, non-octave period study. Deferred.

Total v1 (phases 0-2): ~5-8 days. No changes under `Source/dsp/` in any phase.

## Verification method
All path/behavior claims above were verified by direct code reading with the cited file:line evidence, cross-checked against `ambika_reference/` (part.cc TuneNote/AcceptNote, part.h PartData, lookup_tables.py, resources.h). Format claims verified against PatchFile.h/.cpp, ParvatiPreset.cpp, SynthEngine.cpp capture/restore, MulExport.h. Effort estimates, UX placement, and user-acceptance items are hypotheses as marked. No files were modified; no builds/tests were run (read-only lane).

---

## Executive summary (≤40 lines)

- **Feasibility: high.** A per-part tuning table needs **one hook** — `AmbikaVoice::startNote` (AmbikaVoice.cpp:169-171), the single funnel where every note source (direct MIDI, on-screen keyboard, arp, seq, MPE) becomes the 14-bit `note14`. Note-off/allocator matching keys on raw note numbers upstream, so fine-offset-only tables cannot break voice management. No `Source/dsp/` changes required — preserves the bit-exactness policy (CONTRIBUTING.md:128).
- **Firmware precedent found:** Ambika already had per-part microtuning — `PartData.raga` byte 4 + ~40 scale tables applied in `Part::TuneNote` (part.cc:634-647). Parvati dropped it (only octave+tuning ported). Restoring raga is both fidelity fix and instant v1 content; the byte already round-trips .PRO/.MUL.
- **Hard constraint:** oscillator pitch resolution is 1/64 semitone (~1.56 ct) — the increment LUT drops the 14-bit pitch LSB (voice.cpp:445; table steps of 2 units). Design the UI/store around that; don't promise finer.
- **Order of operations is already correct:** tuning at note-on (baked into portamento target), MPE/bend as a live additive offset afterwards (voice.cpp:418) — bend stays in absolute cents, never re-quantized. Arp (+12/octave) and note-seq transposition happen in note-number space **before** the hook, so they transpose by scale period automatically for octave-repeating scales (v1 constraint: no non-octave periods).
- **Formats:** raga preset rides PartData byte 4 (hardware-exportable today); custom 12-entry tables live only in `.parvati` multi (voice_slots-style guarded fields) + engine state **v7** (length-prefixed append, FX-block pattern); .MUL export of custom tables is lossy → warn in MulExportDialog.
- **Recommendation:** v1 = firmware raga presets + simple 12-entry cents table per part; **not** Scala (parser/mapping scope, zero audible gain at 1.56-ct resolution). Scala import as a later *converter* only.
- **Findings:** no blockers. Important: F1 un-ported raga (fidelity), F3 resolution cap, F8 hardware-export loss. Note: F5 standing-bend not picked up by newly triggered voices (pre-existing, worth fixing alongside).
- **Plan:** Phase 0 raga restore (~1-1.5 d) → Phase 1 custom table + state v7 + .parvati + tests (~2-3 d) → Phase 2 UI (PatchPage row combo + cents editor) + export warning (~2-3 d). v1 total ≈ 5-8 days. Full report: audit/2026-08-15-microtonal-design.md (returned in-response; no write tool in this session).

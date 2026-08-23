# Parvati architecture refactor

Goal: replace the fragile ad-hoc plumbing with a clean, modern layer. The
Ambika DSP and the sound stay **byte-identical**. The DSP core
(`Source/dsp/*` — `Voice`, filters, oscillators, envelopes, LFOs — and
`AmbikaVoice::renderNextBlock`/`fillInternalBlock`) stays **untouched**. Only
the state-management / threading / parameter-application layer above it
changes.

## The Ambika model (preserved)
Controller (6 Parts, each a Patch frame + PartData + routing + arp/seq) →
voicecards (6 voices, each a `Voice` rendering one patch frame). The plugin
uses the same design: `SynthEngine` = controller, `AmbikaVoice` = voicecard.
The controller sends a full patch frame to a voicecard. The voicecard loads
it. This transfer is now clean and atomic.

**Voice model (slots-truth):** the user-facing polyphony knob is each Part's
`voiceSlots` (1..16, from a fixed 96-voice pool = 6 Parts x 16). A Part is
enabled if and only if its count is >= 1. An arrangement preset or a loaded
multi is the only way to disable a Part. On hardware, a voice IS a voicecard.
The firmware 6-voicecard bitmask is therefore **derived, not user state**.
`mul_export::deriveMasks` (the pure, tested solver in `Source/MulExport.h`)
gives each active Part a contiguous proportional share of the 6 cards. This
is the single source of truth for the engine and the `.MUL` export
strategies, so the two cannot drift. The derived masks keep only their
legacy functions: individual-output (aux) routing and `.MUL`/hardware
export. Legacy loads (`.MUL`, old host-state blobs) seed slots from the
stored mask's popcount (0 -> disabled).

## Current problems (from the two read-only audits)
1. **Ad-hoc message↔audio handoffs** — three special flags
   (`allocationDirty_`, `killGeneratedNotes_`, `resetAllVoicesPending_`) plus
   direct message-thread writes to audio-read state. Each one is a possible
   race.
2. **Plain cross-thread byte writes** — `applyPatchByte`/`applyPartByte`
   write `voice_.patch_`/`part_` from the message thread while the audio
   thread renders them (torn reads). `setVcaExponential`/`setFilterDrive`/
   `setSmoothing` do the same. The arp/seq object setters do it too.
3. **Dual state representation** — APVTS (normalized floats, current-part
   only) vs engine bytes (all parts). *Multiple* apply paths bridge the two
   and diverge (Part-0 clobber, `refreshApvtsFromCurrentPart` workaround,
   options via the current-part bridge).
4. **Fragile voice lifecycle** — `Kill` zeroes state that only
   `Init`/`reprime` restores.
5. **F1 (fixed)** — `pushPartBytesToVoices` wrote 84 part-bytes into the
   7-byte `dsp::Part` (an out-of-bounds corruption on every switch).

## Target architecture
- **Authoritative engine state.** `SynthEngine::Part` is the single source of
  truth per part (patch frame + voicecard part fields + routing + polyphony +
  arp/seq). Arp/seq state lives with authority in its own objects. PartData
  bytes are *derived* at serialize time, not a stale second copy.
  `EngineOptions` (`vca_curve`/`filter_card`/`filter_drive`/`voice_mode`) are
  explicit engine-owned globals. They do not pass through the current-part
  APVTS bridge.
- **One atomic message→audio transfer.** The message thread changes a part's
  state and increases a per-part version counter. At the block top, the audio
  thread takes a snapshot of the changed parts and pushes the full frame to
  that part's voices (the firmware's "ship a patch frame" model). This
  replaces all three flags and every plain per-voice write.
- **One unified apply.** `engine.applyPartState(part, …)` is used by live
  edits *and* by every file load (`.PRO`/`.MUL`/`.parvati`). The APVTS
  becomes a host-facing adapter that reads and writes the authoritative
  state. It is not the source of truth.
- **Clean voice lifecycle.** Voices reload a frame atomically on the audio
  thread. The Kill/reprime workarounds are removed or collapsed into the
  frame reload.
- **DSP untouched.** `Source/dsp/*` and the Ambika render path stay as-is.

## Phases (test-gated; commit when tests pass; playable between phases)
- **Phase 0 — F1 OOB fix** ✅
- **Phase 1 — state ownership cleanup.** (Skipped as a standalone phase. The
  state is already well consolidated in `SynthEngine::Part`. Phase 2/final
  addressed the arp/seq dual-source concern — arp/seq writes are now
  deferred.)
- **Phase 2 — single atomic transfer (patch/part bytes)** ✅. Per-Part
  `frameDirty_`; `applyPatchByte`/`applyPartByte` stage + defer;
  `processTransport` services on the audio thread.
  `tests/concurrency_test.cpp` guards the race.
- **Phase 3 — unified apply + engine-storage-authoritative after
  multi-load** ✅. Multi-loads no longer call `syncAllParamsToEngine` (that
  call clobbered Part 0). `refreshApvtsFromCurrentPart` is removed.
- **Phase 4 (final) — finish threading: global options + arp/seq +
  voiceCount** ✅. Global option setters (`setVcaExponential`/
  `setParameterSmoothing`/`setFilterDrive`) stage via `optionsDirty_`.
  Arp/seq setters stage via per-Part `configDirty_` (including the
  active→inactive transition on the audio thread). The `voiceIndices.size()`
  snapshot passes through per-Part `voiceCount_` (written in
  `rebuildVoiceAllocation`, read by the editor). The concurrency test was
  broadened.
- **Phase 5 — concurrency test harness + sweep** ✅ (merged into Phase 4).

## Post-sweep hardening (after the arch series)
A read-only 5-agent sweep (GUI / patches / OS-threading / DSP / integration)
ran against `813bd85`. It found and fixed these issues:
- **P0 — crush stack-use-after-scope** (`AmbikaVoice::fillInternalBlock`):
  the `crushed[]` hold buffer was declared inside the `if (crush>1)` block.
  The `out = crushed` shadow therefore dangled after the block. Every
  downstream `out[i]` read (1x default/smoothing + the oversampled raw fill)
  was undefined behaviour — an ASAN abort (shadow byte `f8`) reachable from
  the Crush knob / `mix_crush`. The buffer was moved to function scope. The
  numerics are unchanged (bit-identical).
- **P1 — arp/seq ownership made consistent.** Phase 4 deferred *live* arp/seq
  edits to `pendingConfig_` + `configDirty_`. But the **load paths**
  (`loadMultiFile`, `applyParvatiMulti`) still wrote the live
  `Arpeggiator`/`Sequencer` objects directly (TSAN data races vs the
  audio-thread clock loop). The **serialize paths** (`saveMultiFile`,
  `partRaw`, `loadPartIntoApvts`) still read the live objects. Those objects
  lag `pendingConfig_` until the audio thread services `configDirty_`.
  Edits were therefore lost in headless runs or raced in production. A
  latent clobber also existed: a load left `pendingConfig_` stale, so the
  next live edit re-applied stale defaults. Fix: `pendingConfig_` is now the
  **MT-authoritative** arp/seq config. Loads stage through it
  (`stageArpSeqFromPartBytes`) and the constructor seeds it. Serialize and
  refresh read it. The audio thread stays the sole writer of the live
  objects (`servicePendingConfig`). `configDirty_` is set **once after a
  load**, not per param. The audio thread then always services a complete
  snapshot.
- **P2 — `controller_mod_test` threshold** was halved by the post-test
  `-6 dB` main-bus headroom (`kMainMixHeadroomGain`). The routing is intact.
  The threshold was relaxed from `0.01` to `0.005`.
- **P2 — realtime-safety hardening**: `voiceIndices.reserve(kNumVoices)`
  prevents a heap allocation on the audio thread at a Hardware→Extended
  switch. The per-voice `osFactorDirty_`/`topologyDirty_` service now uses
  `exchange(acq_rel)`. This closes a lost-update window vs `load()` +
  `store(false)`.

## Phase 6 — close the last message↔audio data races (TSAN-clean)
The plain-payload-behind-a-dirty-flag pattern (Phase 2/4) left the byte
arrays and two scalars as plain `uint8_t`/`int` behind `frameDirty_` /
`allocationDirty_`. TSAN flagged them under rapid automation / mid-run
loads. The fix makes every cross-thread state atomic:
- **`patchBytes`/`partBytes`** → `AtomicByteArray<N>` — a fixed-size array
  of `std::atomic<uint8_t>` with read/write element proxies. Existing
  `arr[i] = v` / `uint8_t x = arr[i]` sites compile unchanged. Whole-array
  operations use `loadFrom`/`fill`/`operator=`/`copyTo`. The atomics are
  per-byte relaxed. The per-Part `frameDirty_` release/acquire still orders
  the publish of a whole frame.
- **`voiceAllocation`** and **`voiceMode_`** → `std::atomic` (both are
  written by the message thread and read by the audio thread behind
  `allocationDirty_`).
Result: `tests/concurrency_test.cpp` (a background audio thread that drives
  patch/part/arp/seq/polyphony rapidly + a mid-run preset load) is now
  **TSAN-clean** (0 races over repeated runs). `polyphonyMode` stays plain
  (audio-thread-only).

## Per-part tuning staging (microtonal)
Per-part tuning follows the same MT-stage → AT-service pattern as the byte
frames:
- **Preset mode** uses PartData byte 4 (the firmware "raga"). It is written
  via the normal `applyPartByte`/`part_raga` param path and published by
  `frameDirty_`.
- **Custom mode** (mode 33) is engine-side only: `Part::customTuning`
  (`AtomicByteArray<24>`, 12 × int16 LE in 1/128-semitone units) + a
  `customTuningActive` flag. A dedicated `tuningDirty_` release/acquire
  publishes it, serviced next to the `frameDirty_` loop.
  `pushPartBytesToVoices` also ends with a tuning push, so preset picks and
  custom edits share one code path. `setPartTuningCustom` keeps byte 4 at 0
  while custom is active. The resolution rule
  `byte4 ? byte4 : customFlag ? 33 : 0` then never hides user edits. The
  function passes the 32767 mute sentinel through verbatim.
- **Consumption** happens once, at the single note→pitch point
  (`AmbikaVoice::startNote`: `note14 = baseNote*128 + table[note & 11] + …`).
  It happens after the sentinel gates in `SynthEngine::noteOn`/
  `triggerNoteInPart` refuse muted note classes (firmware `AcceptNote`
  semantics).
- **Persistence**: the host-state blob carries a per-part length-prefixed
  `{mode; offsets[12]}` block (state **v7**). `.parvati` multis carry
  `tuning_mode`/`tuning_offsets` behind `hasProperty` guards. `.PRO`/`.MUL`
  carry the preset byte unchanged (custom tables do not export — the dialog
  gives a warning). `tests/tuning_test.cpp` guards this.

## Host plugin state (full 6-Part persistence)
`getStateInformation`/`setStateInformation` previously persisted only the
APVTS (current Part) + UI prefs. A DAW project reload therefore lost Parts
1-5 (patch/part/arp/seq/routing). The functions now also embed a versioned,
base64 binary blob (`engine_state`). The blob captures every Part's patch
bytes, PartData (arp/seq overlaid from the authoritative `pendingConfig_`),
MIDI routing, voice counts (`voiceSlots`; the loader re-derives the
voicecard bitmask — see the voice model above), polyphony mode and the
current Part. `SynthEngine::captureState`/`restoreState` own the format
(magic `PVST`; it is byte-oriented and therefore endian-independent).
Backward compatible: a state without `engine_state` (or a short/foreign
blob) falls back to the legacy current-Part APVTS restore.
`tests/host_state_test.cpp` guards the round-trip + the legacy fallback.

## Hard constraints
- Do not modify `Source/dsp/*` DSP behaviour or the Ambika render path.
- Do not change the APVTS parameter IDs / byte offsets (`ParameterLayout.cpp`
  descriptors) — the verified byte-bridge.
- Keep the Ambika-faithful structures: 6 parts, the Patch/PartData
  byte layout, multitimbral routing, the controller/voicecard split. The
  6-voicecard bitmask stays a DERIVED representation (see the voice model
  above). It never becomes independent user state.
- Every phase keeps the full test suite passing.

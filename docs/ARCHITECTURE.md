# Parvati architecture refactor

Goal: replace the fragile ad-hoc plumbing with a clean, modern layer **while
keeping the Ambika DSP and sound byte-identical**. The DSP core
(`Source/dsp/*` — `Voice`, filters, oscillators, envelopes, LFOs — and
`AmbikaVoice::renderNextBlock`/`fillInternalBlock`) is **untouched**. Only the
state-management / threading / parameter-application layer above it changes.

## The Ambika model (preserved)
Controller (6 Parts, each a Patch frame + PartData + routing + arp/seq) →
voicecards (6 voices, each a `Voice` rendering one patch frame). The plugin
mirrors this: `SynthEngine` = controller, `AmbikaVoice` = voicecard. The
controller ships a full patch frame to a voicecard; the voicecard loads it. We
make that transfer clean and atomic.

**Voice model (slots-truth):** the user-facing polyphony knob is each Part's
`voiceSlots` (1..16, from a fixed 96-voice pool = 6 Parts x 16 — a Part is
enabled iff its count is >= 1; a Part is disabled only by an arrangement
preset or a loaded multi). On hardware a voice IS a voicecard, so the firmware
6-voicecard bitmask is **derived, not user state**: `mul_export::deriveMasks`
(the pure, tested solver in `Source/MulExport.h`) gives each active Part a
contiguous proportional share of the 6 cards — the single source of truth
shared by the engine and the `.MUL` export strategies, so they cannot drift.
The derived masks keep only their legacy jobs: individual-output (aux) routing
and `.MUL`/hardware export. Legacy loads (`.MUL`, old host-state blobs) seed
slots from the stored mask's popcount (0 -> disabled).

## Current problems (from the two read-only audits)
1. **Ad-hoc message↔audio handoffs** — three bespoke flags
   (`allocationDirty_`, `killGeneratedNotes_`, `resetAllVoicesPending_`) plus
   direct message-thread writes to audio-read state. Each is a race chance.
2. **Plain cross-thread byte writes** — `applyPatchByte`/`applyPartByte` write
   `voice_.patch_`/`part_` from the message thread while the audio thread
   renders them (torn reads); `setVcaExponential`/`setFilterDrive`/`setSmoothing`
   likewise. Arp/seq object setters too.
3. **Dual state representation** — APVTS (normalized floats, current-part only)
   vs engine bytes (all parts), bridged by *multiple* apply paths that diverge
   (Part-0 clobber, `refreshApvtsFromCurrentPart` workaround, options via the
   current-part bridge).
4. **Fragile voice lifecycle** — `Kill` zeroes state only `Init`/`reprime`
   restores.
5. **F1 (fixed)** — `pushPartBytesToVoices` wrote 84 part-bytes into the 7-byte
   `dsp::Part` (OOB corruption on every switch).

## Target architecture
- **Authoritative engine state.** `SynthEngine::Part` is the single source of
  truth per part (patch frame + voicecard part fields + routing + polyphony +
  arp/seq). Arp/seq live authoritatively in their objects; PartData bytes are
  *derived* on serialize (not a stale second copy). `EngineOptions`
  (`vca_curve`/`filter_card`/`filter_drive`/`voice_mode`) are explicit
  engine-owned globals — not threaded through the current-part APVTS bridge.
- **One atomic message→audio transfer.** The message thread mutates a part's
  state and bumps a per-part version counter; the audio thread, at the block
  top, snapshots the changed parts and pushes the full frame to that part's
  voices (the firmware's "ship a patch frame"). Replaces all three flags and
  every plain per-voice write.
- **One unified apply.** `engine.applyPartState(part, …)` is used by live edits
  *and* every file load (`.PRO`/`.MUL`/`.parvati`). The APVTS becomes a
  host-facing adapter that reads/writes the authoritative state — not the source
  of truth.
- **Clean voice lifecycle.** Voices reload a frame atomically on the audio
  thread; the Kill/reprime hacks are removed or collapsed into the frame reload.
- **DSP untouched.** `Source/dsp/*` and the Ambika render path stay as-is.

## Phases (test-gated; commit when green; playable between phases)
- **Phase 0 — F1 OOB fix** ✅
- **Phase 1 — state ownership cleanup.** (Skipped as standalone; the state is
  already reasonably consolidated in `SynthEngine::Part`. The arp/seq dual-source
  concern was addressed by Phase 2/final — arp/seq writes are now deferred.)
- **Phase 2 — single atomic transfer (patch/part bytes)** ✅. Per-Part
  `frameDirty_`; `applyPatchByte`/`applyPartByte` stage + defer; `processTransport`
  services on the audio thread. `tests/concurrency_test.cpp` guards the race.
- **Phase 3 — unified apply + engine-storage-authoritative after multi-load** ✅.
  Multi-loads no longer `syncAllParamsToEngine` (which clobbered Part 0);
  `refreshApvtsFromCurrentPart` removed.
- **Phase 4 (final) — finish threading: global options + arp/seq + voiceCount** ✅.
  Global option setters (`setVcaExponential`/`setParameterSmoothing`/
  `setFilterDrive`) stage via `optionsDirty_`; arp/seq setters stage via per-Part
  `configDirty_` (incl. the active→inactive transition on the audio thread);
  `voiceIndices.size()` snapshot via per-Part `voiceCount_` (written in
  `rebuildVoiceAllocation`, read by the editor). Concurrency test broadened.
- **Phase 5 — concurrency test harness + sweep** ✅ (merged into Phase 4).

## Post-sweep hardening (after the arch series)
A read-only 5-agent sweep (GUI / patches / OS-threading / DSP / integration)
against `813bd85` found and fixed:
- **P0 — crush stack-use-after-scope** (`AmbikaVoice::fillInternalBlock`): the
  `crushed[]` hold buffer was declared inside the `if (crush>1)` block, so the
  `out = crushed` shadow dangled after the block and every downstream `out[i]`
  read (1x default/smoothing + the oversampled raw fill) was UB — an ASAN abort
  (shadow byte `f8`) reachable from the Crush knob / `mix_crush`. Hoisted the
  buffer to function scope; numerics unchanged (bit-identical).
- **P1 — arp/seq ownership made consistent.** Phase 4 deferred *live* arp/seq
  edits to `pendingConfig_` + `configDirty_`, but the **load paths**
  (`loadMultiFile`, `applyParvatiMulti`) still wrote the live `Arpeggiator`/
  `Sequencer` objects directly (TSAN data races vs the audio-thread clock loop),
  and the **serialize paths** (`saveMultiFile`, `partRaw`, `loadPartIntoApvts`)
  read the live objects — which lag `pendingConfig_` until the audio thread
  services `configDirty_`, so edits were lost in headless / raced in production.
  There was also a latent clobber: a load left `pendingConfig_` stale, so the
  next live edit re-applied stale defaults. Fix: `pendingConfig_` is now the
  **MT-authoritative** arp/seq config — loads stage through it
  (`stageArpSeqFromPartBytes`), the constructor seeds it, serialize/refresh
  read it, and the audio thread remains the sole writer of the live objects
  (`servicePendingConfig`). `configDirty_` is set **once after a load** (not per
  param) so the audio thread only ever services a complete snapshot.
- **P2 — `controller_mod_test` threshold** was halved by the post-test `-6 dB`
  main-bus headroom (`kMainMixHeadroomGain`); the routing is intact, threshold
  relaxed `0.01 → 0.005`.
- **P2 — realtime-safety hardening**: `voiceIndices.reserve(kNumVoices)` (no
  heap alloc on the audio thread at a Hardware→Extended switch); the per-voice
  `osFactorDirty_`/`topologyDirty_` service now uses `exchange(acq_rel)` (closes
  a lost-update window vs `load()`+`store(false)`).

## Phase 6 — close the last message↔audio data races (TSAN-clean)
The plain-payload-behind-a-dirty-flag pattern (Phase 2/4) left the byte arrays
and a couple of scalars as plain `uint8_t`/`int` behind `frameDirty_` /
`allocationDirty_`, which TSAN flagged under rapid automation / mid-run loads.
Closed by making every cross-thread state atomic:
- **`patchBytes`/`partBytes`** → `AtomicByteArray<N>` (a fixed-size array of
  `std::atomic<uint8_t>` with read/write element proxies, so existing
  `arr[i] = v` / `uint8_t x = arr[i]` sites compile unchanged; whole-array ops
  use `loadFrom`/`fill`/`operator=`/`copyTo`). Per-byte relaxed atomics; the
  per-Part `frameDirty_` release/acquire still orders a whole frame's publish.
- **`voiceAllocation`** and **`voiceMode_`** → `std::atomic` (both are
  message-thread-written, audio-thread-read behind `allocationDirty_`).
Result: `tests/concurrency_test.cpp` (a background audio thread hammering
  patch/part/arp/seq/polyphony + a mid-run preset load) is now **TSAN-clean**
  (0 races over repeated runs); `polyphonyMode` stays plain (audio-thread-only).

## Per-part tuning staging (microtonal)
Per-part tuning follows the same MT-stage → AT-service shape as the byte frames:
- **Preset mode** rides PartData byte 4 (the firmware "raga") — written via the
  normal `applyPartByte`/`part_raga` param path, published by `frameDirty_`.
- **Custom mode** (mode 33) is engine-side only: `Part::customTuning`
  (`AtomicByteArray<24>`, 12 × int16 LE in 1/128-semitone units) + a
  `customTuningActive` flag, published by a dedicated `tuningDirty_` release/
  acquire serviced next to the `frameDirty_` loop (`pushPartBytesToVoices` also
  ends with a tuning push, so preset picks and custom edits share one funnel).
  `setPartTuningCustom` keeps byte 4 at 0 while custom is active (the resolution
  rule `byte4 ? byte4 : customFlag ? 33 : 0` then never shadows user edits) and
  passes the 32767 mute sentinel through verbatim.
- **Consumption** happens once, at the single note→pitch choke point
  (`AmbikaVoice::startNote`: `note14 = baseNote*128 + table[note & 11] + …`),
  after the sentinel gates in `SynthEngine::noteOn`/`triggerNoteInPart` refuse
  muted note classes (firmware `AcceptNote` semantics).
- **Persistence**: the host-state blob carries a per-part length-prefixed
  `{mode; offsets[12]}` block (state **v7**); `.parvati` multis carry
  `tuning_mode`/`tuning_offsets` behind `hasProperty` guards; `.PRO`/`.MUL`
  carry the preset byte unchanged (custom tables do not export — the dialog
  warns). Guarded by `tests/tuning_test.cpp`.

## Host plugin state (full 6-Part persistence)
`getStateInformation`/`setStateInformation` previously persisted only the APVTS
(current Part) + UI prefs, so a DAW project reload lost Parts 1-5 (patch/part/
arp/seq/routing). They now also embed a versioned, base64 binary blob
(`engine_state`) capturing every Part's patch bytes, PartData (arp/seq overlaid
from the authoritative `pendingConfig_`), MIDI routing, voice counts
(`voiceSlots`; the voicecard bitmask is re-derived on load — see the voice
model above), polyphony mode and the current Part. `SynthEngine::
captureState`/`restoreState` own the format (magic `PVST`, byte-oriented →
endian-independent). Backward compatible: a state without `engine_state` (or a
short/foreign blob) falls back to the legacy current-Part APVTS restore.
`tests/host_state_test.cpp` guards the round-trip + the legacy fallback.

## Hard constraints
- Do not modify `Source/dsp/*` DSP behaviour or the Ambika render path.
- Do not change the APVTS parameter IDs / byte offsets (`ParameterLayout.cpp`
  descriptors) — the verified byte-bridge.
- Keep the Ambika-faithful structures: 6 parts, the Patch/PartData
  byte layout, multitimbral routing, the controller/voicecard split. The
  6-voicecard bitmask stays a DERIVED representation (see the voice model
  above) — never independent user state.
- Every phase keeps the full test suite green.

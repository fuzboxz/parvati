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
- **Phase 1 — state ownership cleanup.** Make arp/seq authoritative (derive
  PartData arp/seq bytes on serialize, drop the stale mirror); move global
  options to explicit engine ownership. Behavior-identical.
- **Phase 2 — single atomic transfer.** Per-part versioned state snapshots
  applied on the audio thread; delete the three flags + the plain per-voice
  writes (patch/part bytes, vca/smoothing/drive, arp/seq).
- **Phase 3 — unified apply + APVTS-as-adapter.** Live edits + all loads route
  through one apply; remove `refreshApvtsFromCurrentPart`, the dual sources, the
  options-via-current-part bridge.
- **Phase 4 — voice lifecycle.** Atomic frame reload on the audio thread; drop
  reprime hacks.
- **Phase 5 — concurrency test harness + full thread-safety sweep.**

## Hard constraints
- Do not modify `Source/dsp/*` DSP behaviour or the Ambika render path.
- Do not change the APVTS parameter IDs / byte offsets (`ParameterLayout.cpp`
  descriptors) — the verified byte-bridge.
- Keep the Ambika-faithful structures: 6 voicecards / 6 parts, the Patch/PartData
  byte layout, multitimbral routing, the controller/voicecard split.
- Every phase keeps the full test suite green.

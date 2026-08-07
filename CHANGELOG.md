# Changelog

All notable changes to Parvati. Dates are approximate (local dev chronology).

## [Unreleased]

### Added
- **Per-part FX section (Parvati-exclusive).** Each of the 6 Parts now has its
  own stereo FX chain: 3 reorderable FX slots (Gain+Pan / Delay / Reverb /
  Chorus placeholders), a Series/Parallel topology + slot-order control, and a
  separate 16-slot FX mod matrix that shares the synth's modulation sources.
  FX runs post-voice-render on the main mix only (per-part stereo), while the
  per-voicecard aux buses stay dry. 71 new `isFx` APVTS params drive it, written
  on the message thread and applied on the audio thread via the same
  dirty-flag staging pattern as the rest of the engine. Audibly-identical to
  the pre-FX dry mix when all slots are disabled (chain bypass = dry copy).
- **FX persistence.** Per-part FX state round-trips through the Parvati-native
  `.parvati` format (both multi and single-part patch) and the DAW host state
  (binary blob bumped to **version 2**, length-prefixed FX block per Part).
  Backward compatible: older `.parvati` files and v1 host blobs load with FX at
  defaults. The Ambika `.PRO` / `.MUL` byte formats are unchanged and **drop FX
  entirely** (FX never touches Ambika patch/part bytes), so Ambika interchange
  stays byte-faithful. Note: DAW projects saved on v2 won't fully restore on an
  older (v1-only) Parvati build — the engine blob is rejected and it falls back
  to the legacy APVTS restore.

### Changed
- **Font: Console by default + Serif / Sans Serif options + live switching.**
  The Settings "Font" combo now defaults to **Console** (embedded GNU Unifont)
  and adds system-default **Serif** and **Sans Serif** choices. Switching the
  font now updates *every* text surface live — combo drop-down lists, tab labels,
  and group-component panel titles previously stayed in the old font; all stock
  text now resolves through the LookAndFeel font getters. The PARVATI header
  logotype is also slightly smaller.
- **License: AGPL-3.0.** Parvati's own code is now licensed under the GNU
  Affero General Public License v3.0 (was GPL-3.0). The Ambika-derived DSP under
  `Source/dsp/` and the factory preset banks retain their GPL-3.0 license
  (upstream-derived, compatible with AGPL-3.0). See `LICENSE` and `NOTICES.md`.

### Added
- **GNU Unifont "Console" font mode.** The Settings "Font" combo's Console
  mode now uses an embedded subset of GNU Unifont (ASCII + Latin-1, ~18 KB) for
  a true DOS/retro look, instead of the system monospace. (GPL+ with font
  exception, AGPL-3.0-compatible.)

### Fixed (post-architecture deep sweep)
- **P0 — crush stack-use-after-scope** (`AmbikaVoice::fillInternalBlock`): the
  `crushed[]` sample-and-hold buffer was block-scoped, so the `out = crushed`
  shadow dangled after the `if (crush>1)` block and every downstream `out[i]`
  read was UB (ASAN abort, reachable from the Crush knob). Hoisted to function
  scope; numerics unchanged.
- **P1 — arp/seq ownership consistency.** File loads (`loadMultiFile`,
  `applyParvatiMulti`) and serialize/refresh paths (`saveMultiFile`, `partRaw`,
  `loadPartIntoApvts`) now go through `pendingConfig_` + `configDirty_` like the
  live setters, instead of reading/writing the live `Arpeggiator`/`Sequencer`
  objects directly. This removes the load-path TSAN data races with the
  audio-thread clock loop, fixes arp/seq edits being lost on save in headless /
  racing in production, and fixes a latent clobber where a load left
  `pendingConfig_` stale so the next edit re-applied defaults. Added
  `SynthEngine::stageArpSeqFromPartBytes`.
- **Phase 6 — message↔audio data races closed (TSAN-clean).** The plain byte
  arrays / scalars behind the `frameDirty_` / `allocationDirty_` latches are now
  atomic: `patchBytes`/`partBytes` → `AtomicByteArray<N>` (element proxies keep
  `arr[i] = v` / `uint8_t x = arr[i]` sites unchanged; whole-array ops via
  `loadFrom`/`fill`/`operator=`/`copyTo`); `voiceAllocation` + `voiceMode_` →
  `std::atomic`. `concurrency_test` is now TSAN-clean (0 races).
- **Crash on the note-sequencer (TekDrums multi) — root cause + fix.** The hosted
  `SIGBUS`/PAC-fail in `Sequencer::internalNoteOn/Off` (a corrupted
  `std::function` invoker) was a **memory-corruption cascade from `NoteStack`**.
  The `NoteStack` default constructor left its pool at `note == 0`, but the
  free-slot search looks for `kFreeSlot (255)` — and the `Arpeggiator`'s
  `pressedKeys_` is **never `clear()`-ed**, so every `noteOn` found no free slot,
  wrote the `pool_[0]` dummy sentinel, and inflated `size_`. That desynced the
  linked list from the sorted array, producing out-of-range `pool_`/`sorted_ptr_`
  indices that wrote ~1 KB past the NoteStack — straight into the adjacent
  `Sequencer`'s `std::function`, corrupting its invoker. Fixes: `NoteStack()` now
  runs `clear()` (proper init, the root fix); `noteOn` bails on `free_slot == 0`
  (defense, never clobber the sentinel); `pendingConfig_` is now seqlock-guarded
  (MT writer / AT reader); `pendingTopology_` / `pendingOsFactor_` are now
  `std::atomic`. Surfaced by a new two-thread test (see below).
- **Two-thread test harness.** `tests/mt_harness.h` + a rewritten
  `parvati_concurrency_test` model the real plugin threading: a background AUDIO
  thread loops `processBlock` with the transport playing + a held note (so the
  arp / note-sequencer actually generate notes) while the MESSAGE thread runs the
  full host surface (param edits, arp/seq, part switches, `.MUL`/`.parvati` loads,
  host-state get/set, options, voice-mode). `PARVATI_MT_MASK` (argv, hex) selects
  op classes for bisection. Run under TSAN to catch message↔audio races.
- **Crash on the note-sequencer (TekDrums multi) — `pendingConfig_` data race.**
  The arp/seq config staging struct was a plain `PendingConfig` written by the
  message thread (param edits / `.parvati`-multi + host-state loads) and read by
  the audio thread (`servicePendingConfig`, every block) — a TSAN-confirmed data
  race. UB in the realtime path manifested as a hard `SIGBUS`/PAC-fail crash in
  the hosted plugin (calling the sequencer's note callback via a corrupted
  `std::function` invoker) while sanitizer builds stayed green — exactly why the
  regression suite did not catch it. Fixed with a **seqlock** (`pendingSeq_`):
  the message thread is the sole writer (`writePendingConfig`), the audio thread
  the sole reader (`readPendingConfig`, retry-on-write) — the textbook SPSC case.
  All arp/seq setters, `stageArpSeqFromPartBytes`, `applyParvatiMulti`, and the
  serialize/refresh readers route through it. TSAN now reports 0 races on the
  note-sequencer path.
- **Host plugin state now persists the full multi.** `getStateInformation`/
  `setStateInformation` embed a versioned binary blob (`engine_state`) with all 6
  Parts (patch/part bytes, arp/seq, routing, voice allocation/mode, current
  part) via `SynthEngine::captureState`/`restoreState`, so a DAW reload preserves
  the whole multitimbral setup. Backward compatible (legacy states fall back to
  the current-Part APVTS restore). Guarded by `parvati_host_state_test`.
- **P2 — `controller_mod_test`** threshold relaxed `0.01 → 0.005` (the post-test
  `-6 dB` main-bus headroom exactly halves the controller diffs; routing intact).
- **P2 — realtime safety**: `voiceIndices.reserve(kNumVoices)` (no audio-thread
  heap alloc on Hardware→Extended switch); per-voice `osFactorDirty_`/
  `topologyDirty_` service now `exchange(acq_rel)` (closes a lost-update window).

### Added
- **Factory presets** — the GPL-3.0 Ambika "goldencard" banks (128 programs + 2
  multis) are bundled embedded and extracted to the user app-data dir on first
  run; the Patch combo is populated out of the box.
- **`.MUL` (multi) writer** — `PatchFile::writeAmbikaMultiFile` +
  `ParvatiAudioProcessor::saveMultiFile`; full 6-Part state can now be saved and
  reloaded.
- **Patch save/load round-trip tests** (`parvati_roundtrip_test`) — unit
  (parse→write→parse) and end-to-end (load→save→load) for both `.PRO` and `.MUL`.
- **Startup-rumble regression test** (`parvati_idle_silence_test`).
- **Memory-safety / static-analysis tooling** — CMake `PARVATI_ENABLE_ASAN` /
  `UBSAN` / `TSAN` / `WARNINGS_AS_ERRORS` options, `compile_commands.json`
  export, `.clang-tidy`, `.clang-format`, `.editorconfig`, `.gitattributes`.
- **Flexible-width grouping grid** — parameter panels now row-fill to the window
  width (clean grid); `PageInfo::cols` honored as a panels-per-row cap.
  Layout-sanity assertions guard the grid in `parvati_editor_test`.
- **Themed Settings panel** — SidePanel chrome follows the active theme; the
  panel is right-docked so it never covers its launcher button (toggle feedback).
- **Tooltips** — per-parameter help now actually shows (set on the interactive
  child controls; was unreachable via the bare cell's `TooltipClient`).
- **Master DC blocker** (15 Hz) on the main bus.
- **Real-time / thread-safety hardening** — filter-card topology change and the
  arp/seq note-kill are now staged and serviced on the audio thread (mirroring
  the oversampling-factor defer); `processTransport` reuses a member `MidiBuffer`
  (no per-block audio-thread alloc); the per-voice FIFO is reserved from the
  actual worst-case demand; `TransportClock` clamps BPM + floors the tick step
  (no runaway ticking).
- **Controller-modulation regression test** (`parvati_controller_mod_test`) —
  mod wheel (CC1) / breath (CC2) / foot pedal (CC4) / channel pressure are
  proven wired to their mod-matrix sources and audible.
- **Windows support** — CMake JUCE path falls back to `%USERPROFILE%\JUCE`;
  sanitizer / `-Werror` flags are MSVC-guarded (no-op under MSVC); CI builds on
  `windows-latest` with the VS generator. README/CONTRIBUTING document Windows.
- OSS docs: `README.md`, `CONTRIBUTING.md`, `docs/ARCHITECTURE.md`, `LICENSE`
  (GPL-3.0), `NOTICES.md`.

### Fixed
- **Startup low-frequency rumble** — idle voices no longer render (the
  multiplicative ENV→VCA modulation could leave an idle voice's VCA open for
  patches with modulation amount < 63, leaking a sub-audio oscillator tone). Idle
  voices self-gate on `isVoiceActive()`; `Envelope::Init()` now parks the
  generator in DEAD; `Voice::Init()` primes envelope increments so a gated idle
  voice is still trigger-ready.
- **`.MUL` arp round-trip** — arp settings (stored in the Arpeggiator object, not
  a descriptor byte) are now written from the live Arpeggiator on save, so a
  saved `.MUL` reloads with identical arp settings.
- **`.MUL` arp/seq data loss on non-current parts** — `saveMultiFile` now
  serializes arp + sequencer state from the live per-part objects for ALL parts
  (not just the current one); edits survived a part-switch but were lost on save.
- **Stuck released voice** — a released voice could stay active forever if no
  ENV→VCA routing drove `vca()<2`; now also freed when all envelopes reach DEAD.
- **All 6 parts audible at init** — `prepare` now seeds every Part with the
  controller init patch (osc1=Saw) + firmware init `PartData`, mirrored into the
  live arp/seq objects; Parts 1–5 were previously silent until visited.
- **`loadProgramFromBytes`** now also guards a null `part84`.
- **CI silently skipped `parvati_tests`** — the test glob `parvati_*_test` did not
  match `parvati_tests`; now listed explicitly (cross-OS `.exe` handling).

### Removed
- Legacy Projucer `NewProject.jucer` (CMake is the canonical build).
- Dead / unwired code surfaced by a deep static-analysis audit: the write-only
  `globalWheel_/globalBreath_/globalFoot_` engine state; the inert `Filter 2`
  control group (engine only reads `filter[0]`); the dead `ThemeManager`
  persistence/index API, `getArp*Choices` wrappers, `Lfo::step_`, `NoteStack::
  max_size`, `TransportClock` accessors, `KeyboardView::setBaseOctaveNote`; the
  unused `bendRangeSemitones`/`setMpeBendRangeSemitones` accessors,
  `setSequencerMode`, and the superseded `forceInit`/`copyPatchBytes` pair; a
  dead `fourPole` local and unused lambda captures.

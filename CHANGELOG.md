# Changelog

All notable changes to Parvati. Dates are approximate (local dev chronology).

## [Unreleased]

### Added
- **Warps Wavefolder FX.** A per-slot **Wavefolder** ported from the Mutable
  Instruments Warps bipolar wavefolder (memoryless LUT waveshaper) — `FxType`
  value `Wavefolder` (7), append-only so existing presets keep their effect
  assignments. Unlike the Clouds FX it runs NATIVELY at the host sample rate (no
  32 kHz resampling). Knobs: Fold + Bias (asymmetric fold). Anti-aliases via the
  Warps hardware's OWN 6× polyphase-FIR oversampling
  (SampleRateConverter<SRC_UP/DOWN,6,48>, kOversampling=6): the fold runs at 6×
  the host rate internally (native base rate), so the sharp fold corners alias
  exactly as little as the hardware (~8 samples of group delay). Frequency
  Shifter + Ring Modulator (the other two Warps effects) follow.
- **Warps Frequency Shifter + Ring Modulator FX.** Two more per-slot effects
  ported from the Mutable Instruments Warps DSP — **Frequency Shifter** (the
  quadrature/Hilbert "easter-egg" shifter: true single-sideband frequency
  shifting, not pitch scaling; `FxType` 8) and **Ring Modulator** (the Warps
  analog diode-model ring mod against an internal sine/harmonics/buzzy carrier;
  `FxType` 9). Both run NATIVELY at the host sample rate (the Hilbert allpass
  network is normalized-frequency so its ~90° band scales with the host rate;
  the carrier oscillators init at the host rate — no 32 kHz resampling, which
  would move further from the 96 kHz design point). New values are append-only.
  Freq Shifter knobs: Shift / Feedback / Spread (right-channel sideband blend for
  stereo width); Ring Mod knobs: Carrier / Shape / Amount. Ring Mod anti-aliases
  via the same Warps 6× polyphase-FIR oversampling (kOversampling=6): the internal
  carrier renders at the 6× rate and the signal is upsampled 6×, so the diode
  product (signal ± carrier) happens entirely in the oversampled domain (mirrors
  upstream src_up_[0]=carrier, src_up_[1]=modulator). The Freq Shifter is linear
  and needs no oversampling.
- **Rings Resonator FX.** A per-slot **Resonator** ported from the Mutable
  Instruments Rings modal resonator (a bank of up to 64 resonant band-pass SVFs
  tuned to harmonic/inharmonic partials) — `FxType` value `Resonator` (10),
  append-only so existing presets keep their effect assignments. Runs NATIVELY at
  the host sample rate (the SVF coefficients are computed from the normalized
  frequency `freqHz / sampleRate` each block, so they track the host rate — no
  resampler or oversampling needed, unlike Clouds/Warps). Rings-faithful stereo:
  ONE resonator processes a mono sum (0.5*(L+R)); its out (odd modes) -> L and aux
  (even modes) -> R, matching Rings' mono path (part.cc). Position rebalances
  odd vs even (pickup position + stereo width); structure is fixed at the Rings
  default (0.25, slightly inharmonic). Knobs: Pitch (base pitch C1–C7) / Decay
  (ring time) / Bright (brightness) / Position (odd/even mode balance).
  latency()==0 (LTI filter group delay is the effect's sound, not processing
  latency).
- **Clouds FX modules.** Three new per-slot FX algorithms ported from the
  Mutable Instruments Clouds `dsp/fx` chain — **Diffuser** (AP diffusion
  network), **Pitch Shifter** (dual-tap, ±12 st), and **Reverb**
  (Griesinger/Dattorro tank) — selectable alongside the existing Gain+Pan /
  Delay / Reverb / Chorus placeholders. The vendored Clouds + stmlib DSP runs at
  a fixed 32 kHz and is linear-resampled at the FX boundary, so its tuning is
  bit-faithful to upstream at any host rate. New `FxType` values are append-only
  (`Diffuser`/`PitchShifter`/`Reverb` = 1/2/3), so existing presets keep
  their effect assignments. A `parvati_clouds_fx_test` covers build, finite
  output and audible wet output for each module.

- **Clouds "mode" FX (buffer-based).** Three more per-slot effects ported from
  the Mutable Instruments Clouds playback modes — **Looping Delay**, **WSOLA
  Stretch** (time/pitch via waveform-similarity overlap-add), and **Spectral**
  (4096-point phase vocoder / STFT) — selectable alongside the Clouds
  `dsp/fx` chain. Unlike the pure in→out FX these are buffer-based (they record
  the dry input and play back from the recorded past, so they re-texture the
  sound constantly) except Spectral, which runs an in-place FFT pipeline. Each
  runs the vendored DSP at the fixed 32 kHz (HostRateBridge) chunked at ≤32
  samples, and runs the firmware's per-block "background tick" inline (the WSOLA
  correlator splice-point search and the phase-vocoder frame drain) since Parvati
  FX slots have no background thread. New `FxType` values are append-only
  (`LoopingDelay`/`WSOLAStretch`/`Spectral` = 4/5/6), each 3–4 knobs (Pitch /
  Position / Size / Warp / Blur / Freeze); existing presets keep their
  assignments. `parvati_clouds_fx_test` extended to cover all six Clouds modules.
  A latent upstream bug in `window.h` (`Window::Start()` never cleared `done_`,
  silencing WSOLA) was patched faithfully (`// PARVATI PATCH`).

- **Unified Patch page (merges Multi/Setup + Global).** A single **Patch**
  page replaces the separate Multi/Setup and Global pages. A high-level
  **Arrangement** selector — Single / Stack / Split 2 / Layer 2 / Multi 6 —
  auto-configures voice allocation, MIDI channels, key zones and polyphony
  across all 6 parts in one click; each part is then fine-tuned through a card
  **count** (not a voicecard bitmask), MIDI channel, key zone and polyphony. A
  live **"Cards X / 6"** readout and dynamic per-row combo capping make the
  fixed 6-voicecard budget self-evident and impossible to exceed (the GUI never
  offers a count it would reject). The arrangement is **inferred** from engine
  state on load and never stored, so Ambika `.PRO` / `.MUL` interchange stays
  byte-exact; engine internals, file formats and the audio thread are unchanged
  (one additive `const` polyphony getter aside).
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

### Fixed
- **FX mod-matrix: synth-voice modulation sources now couple correctly + track
  the latest note.** Two fixes to how per-voice modulation sources reach the FX
  section:
  - **AC/DC coupling mirrors the synth voice path.** The FX mod matrix now
    treats the same sources as bipolar (AC-coupled, 128 = neutral) as the synth
    mod matrix (`voice.cpp`): `LFO_1..4`, `PITCH_BEND`, `NOTE`. Previously
    every source was read as unipolar 0..1, so an LFO / pitch-bend / note at
    rest (value 128) injected a static offset (~+0.126 at amount 63) instead of
    zero modulation — e.g. an LFO routed to an FX dry/wet sat above its base
    and only used its upper half. Now a centered LFO contributes nothing at rest
    and swings symmetrically (true tremolo). All other sources (`VELOCITY`,
    `AFTERTOUCH`, `WHEEL`, …) keep their existing unipolar depth.
  - **Representative voice tracks the most-recently-triggered note, with a
    de-click crossfade.** The FX stage is per-part but sources are per-voice, so
    it samples one voice per part. It now follows the latest note-on (not an
    arbitrary first-active voice), and on any voice change a ~5 ms crossfade
    bridges the old voice's last source values to the new one — so per-voice
    sources (`VELOCITY`, `NOTE`, per-note MPE bend/pressure/slide) glide
    instead of jumping/clicking when the tracked voice rotates. Global / part-
    global sources are identical across voices so the crossfade is a no-op
    there; tails keep modulating on the last held values when all voices release.
- **FX effect-param smoothing.** The four per-slot effect params (Pitch, Decay,
  Fold, etc.) are now one-pole-smoothed at BLOCK rate (8 ms tau) in `FxChain`
  before being passed to each processor, so fast FX-mod-matrix modulation / host
  automation on a retuning param (Resonator Pitch, Freq Shifter, Pitch Shifter,
  Ring Mod carrier) no longer zippers or clicks. The sole gate param
  (LoopingDelay Freeze) is SNAPPED, not smoothed, so it still engages in the next
  block. (Supersedes the earlier "per-effect param smoothing not included yet"
  note — it now lives centrally in FxChain, benefiting all 10 FX types.)
- **Resonator output limiter + stereo fix.** The Rings modal resonator now ports
  upstream's output `Limiter` (drive = 1.4, the modal `model_gains_`), bounding
  the sustained on-resonance build-up to ~0.8 peak (SoftLimit toward ~1.0) —
  previously the unbounded ×3 makeup hard-clipped at ~16× (defaults) to ~4850×
  (extreme Decay/Bright). Stereo routing corrected to Rings-faithful Option A:
  ONE resonator processes a mono sum `0.5*(L+R)`; its out (odd modes) → L and aux
  (even modes) → R (the two genuinely-different hardware outputs, not a fake
  stereo pair). At Position ≈0.5 the even-mode (R) channel vanishes — the
  center-pluck node (textbook modal physics, identical to hardware Rings); the
  default 0.25 keeps both channels active.
- **FX tails & clicks.** The per-part FX chain now keeps effect tails by
  default and is click-free across all state transitions. Previously
  bypassing/engaging a slot or changing its type hard-cut the wet signal
  (audible pop) and truncated reverb/delay tails, and every dry/wet +
  master-mix value stepped once per block (zipper noise on knob moves and
  FX-mod-matrix modulation). Fixes: (1) always-on per-sample one-pole tail
  fades in `FxChain` (≈0.30 s fade-out on bypass so tails ring out, ≈5 ms
  fade-in on engage/type-swap so the new effect doesn't slam in); (2)
  per-sample one-pole smoothing of per-slot dry/wet and the global FX mix
  (20 ms tau); (3) `FxChain::prepare()` no longer zeroes tail fades or snaps
  smoothers, so a host sample-rate/buffer-size change mid-session no longer
  truncates ringing tails or dips enabled effects. Per-effect param smoothing
  (gain/feedback/etc. inside each effect) is intentionally not included yet.

### Removed
- **`fx_keep_tails` parameter.** Tail retention is now always on, so the
  per-part "Keep FX Tails on Bypass" toggle — and its APVTS param, engine
  field, serialization, and routing-bar UI control — is removed. APVTS
  descriptor count 257 → 256. The DAW host-state blob bumps to **version 4**
  (the keepTails byte is dropped); v1/v2/v3 blobs still load (a v3 blob's
  legacy keepTails byte is consumed and discarded). Older `.parvati` files
  are unaffected.

### Changed
- **FX page: 4-column synth-style top row + routing/slot overhaul.** The
  FX page's top row is a single 4-column row `[ ROUTING | FX1 | FX2 | FX3 ]`, so
  every FX card gets the full top-row height and its knobs reach their 52px
  synth-parity dial. The cards + routing column are borderless sibling panels
  (`containerFill`, 7px corners) matching the synth `GroupComponent` cards, with
  a 14px bold uppercase header. The **ROUTING** column centres an in→out
  signal-flow block diagram (`IN ▶ [FX1] ▶ [FX2] ▶ [FX3] ▶ OUT`) that redraws
  for the Series / Parallel-1+2→3 / Parallel-1→2+3 topologies — topology is
  changed via ◀ ▶ steppers (the FLOW dropdown is gone); the slot blocks are
  bright `FXn` pills and `IN`/`OUT` are smaller muted endpoints, evenly spaced
  in Series. Below sit a `Mix` knob, the 3-band master **EQ** (Low/Mid/High,
  wired to the existing `fx_eq_*` biquads), and a modern pill **Keep FX Tails**
  switch. The **FX-slot cards** lay their knobs in a Mixer-style grid (Mix last;
  Chorus / Gain-Pan drop to 2 columns for a 2-row look), use a fit-to-text type
  combo, and a compact ~80px visualizer (the Chorus graphic is now static). The
  standalone `FxMasterEqCurve` component is removed. **Global / Synth / FX /
  Multi** are unified into four peer top-level pages (Global and Multi are no
  longer overlays).
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

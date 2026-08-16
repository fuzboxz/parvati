# Changelog

All notable changes to Parvati. Dates are approximate (local dev chronology).

## [Unreleased]

### Changed
- **Smaller mod pills + [MOD] header toggle.** The central mod-pill bar's
  pills are compacted (72pt → 56pt, bar 92pt → 78pt — still ≥ the 44pt HIG
  touch minimum), reclaiming a seam of vertical space for the content rows.
  The flanking `<` / `>` scroll buttons are quiet CHROME now: 30x30 glyphs
  centred on the pill band (was 56pt-tall pill-styled tiles with accent
  bands) — borderless at rest, dim glyph that lights on hover/press only. A
  [MOD] toggle (the bar seam; the tap-to-assign toggle is [MAP] now) sits in
  the header between [MAP] and [KBD] and
  collapses/expands the bar seam in BOTH workspaces (hiding hands the bar's
  height back to the content rows; the bar is hidden, not torn down, so
  scroll position and the active-generator highlight survive). Default ON
  (the historical look).
- **FX knobs no longer shrink when the window is resized.** The FX-slot
  cards' 3x2 param grid now has a FIXED cell height (kCellH, full-size 48px
  dials — synth-page parity) instead of deriving the cell height from the
  card body; only the compact visualizer band is elastic (down to 0). The
  FX top row's natural-height scroll floor (kTopRowNaturalH, was 190 for
  the routing bar alone) is raised to 264 so a shorter frame SCROLLS the
  slot row instead of starving the cards — the same pattern the synth
  pages already use. Verified: dial bounds are 48x48 at every window size
  from the 1024x500 minimum to 1800x1100.
- **Theme: the audio-family colour is the theme's brand accent in EVERY
  theme — no amber knob rings/previews anywhere.** The STRICT mod-source
  family palette is unchanged in every theme (Env=teal, LFO=magenta,
  Seq/Arp=mint, Perf=amber, Util=orange, Mod=purple, Const=indigo), but
  `catAudio` (Osc/Sub-Osc/Noise/Filter/Mixer knob rings, osc/filter
  previews, audio section headers) no longer carries the family amber:
  Carbon keeps its cyan brand (a8b3cb2), Midnight adopts steel blue (its
  primary teal would collide with the Env teal rings), Obsidian violet,
  Paper blue (its primary is amber), Crimson crimson. The multigui theme
  guard now pins the exact category hues of ALL six themes.

### Fixed
- **FX effect dropdowns can be reopened after a selection.** FxTypeCombo's
  custom showPopup finished with a nullptr completion callback, so JUCE's
  private ComboBox `menuActive` flag stayed latched TRUE after the popup
  dismissed — every later click bailed inside showPopupIfNotActive() and the
  effect could be picked exactly once per card. The popup now finishes with
  a hidePopup() callback (destruction-safe via SafePointer; the per-item
  action is SafePointer-guarded too). Regression-tested by driving the real
  mouseDown → popup → dismiss path headlessly (parvati_editor_test).
- **Engine correctness/threading fixes (audit 2026-08-15, section A).**
  - **Critical — per-part FX input routing** (`SynthEngine::renderPartFx`):
    the per-part FX-chain input summed `voiceCardBuffers_` indexed by POOL
    voice indices (0..95), which only coincide with card indices (0..5) in
    the default single-part layout. With per-part voice slots or custom
    card bitmasks this cross-bled OTHER parts' cards into a part's FX
    (audible on the main bus, which sums every part's FX output) and left
    parts whose pool slice starts at ≥ 6 with a SILENT FX input — even the
    firmware factory multi bitmasks (0x15/0x2a) were affected. The sum now
    runs over each part's OWNED-card bitmask, persisted by
    `rebuildVoiceAllocation` (`partCardMask_`). Regression test:
    `parvati_part_fx_routing_test` (disjoint-card/slots≠cards layout,
    factory bitmasks, default-path sample-exactness).
  - **Critical — `part_select` automation ran ValueTree/UndoManager writes on
    the render thread**: automating it executed `onPartSelect` →
    `loadPartIntoApvts` (~250 `getParameterAsValue` writes bound to the
    UndoManager, racing the APVTS 50 Hz flush timer — UB-class data race
    plus a hard RT violation). `part_select` is now created
    non-automatable (`AudioParameterIntAttributes().withAutomatable(false)`;
    part switching is a UI action, not a sound parameter).
  - **Critical — `parameterChanged` audio-thread dispatch broke the engine's
    single-writer invariants**: JUCE 9 delivers host automation (AUv3 render
    events / VST3 `process()`) and the CC/NRPN map (`midiParamMap_.handleBuffer`
    inside `processBlock`) synchronously ON THE CALLING THREAD, so arp/seq
    parameter edits reached the engine setters from the audio thread — a
    SECOND writer for the `pendingConfig_` seqlock (torn configs: the
    historical SIGBUS class). Audio-thread-origin arp/seq/part_select writes
    are now funneled through a fixed-capacity (64) latest-wins
    `DeferredParamRing` drained by a 60 Hz message-thread timer (≤ 16.7 ms
    added latency for those edits only; GUI-path edits stay synchronous).
    Debug jasserts on `applyArpParameter`/`applySequencerParameter`/
    `onPartSelect` enforce the restored message-thread invariant. False
    "timer-routed" dispatch comments corrected (PluginProcessor.h,
    ui/FxSlotCard.cpp, SynthEngine.h, tests/mt_harness.h). Concurrency test
    extended: CC102-106 + host-style writes from a non-message thread vs
    GUI-style edits, drop/convergence/last-value-wins assertions.
  - **`loadMultiFile` desynced `part_select`**: a `.MUL` load moved the
    engine to Part 0 but left the part combo showing the previously selected
    part (edits still routed correctly, but the UI lied). The load now writes
    `part_select` (mirroring the `.parvati` path); `partstate_test` gains a
    `assertPartSelectInSync` check.
  - **`setStateInformation` rested on a false `replaceState` assumption**:
    JUCE 9's `replaceState` DOES fire `parameterChanged` per changed
    parameter (valueTreeRedirected → setDenormalisedValue →
    setValueNotifyingHost). A `restoringState_` guard now suppresses those
    mid-restore callbacks (they re-applied stale/partial values to the
    engine and drove the part-load machinery re-entrantly);
    `currentPart_` is tracked explicitly on both the engine-blob and legacy
    restore paths. Host-state test hardened to a full ~259-parameter compare
    (legacy state saved on Part 3 restores part_select==4, engine part 3,
    and the saved bytes land on Part 3).
  - **`readPendingConfig` spun unbounded on the audio thread**: the seqlock
    reader now retries a bounded 64 times, falls back to the audio thread's
    last-good config snapshot, and re-marks `configDirty_` so the next block
    retries (documented: only the audio thread can exhaust — the message
    thread IS the sole writer).
  - **`AnalogFilter` per-sample overhead**: the 4-pole ladder path built a
    1-sample `AudioBlock` + `ProcessContextReplacing` and called
    `LadderFilter::process()` PER SAMPLE (~3.8M wrapper constructions/s at
    96-voice polyphony). A `LadderTap` subclass now exposes the protected
    `processSample`/`updateSmoothers` hooks and reproduces JUCE's exact
    per-sample sequence — BIT-IDENTICAL output (pinned by
    `parvati_analog_filter_batch_test` across cutoff/resonance/drive regimes
    and input classes).
  - Dead/stale surface removed: unused `getArp()`/`getSequencer()` (returned
    audio-thread-owned objects; zero call sites), dead
    `setNoteStealingEnabled(true)` (JUCE's steal path is unreachable —
    noteOn/noteOff are overridden and stealing lives in the per-part
    PolyAllocator), and four stale comments (`setFxMix` "not consumed yet",
    "16 AmbikaVoice instances", `killGeneratedNotes_` flagged-by, seqlock
    sole-writer note).
- **Deferred (recorded, not implemented — separate lanes):** god-object
  extraction (PolyAllocator/PartFxRack/EngineStateCodec), sample-accurate
  arp/seq timing, PartData byte-map unification, clouds::Reverb `lp_decay`
  init (vendored-verbatim policy), audio-thread allocation pre-construction
  for OS/topology/FX-type changes, UndoManager transaction bracketing,
  `.parvati` version check, part-combo repaint guard, SharedContainer
  diagnostics, relaxed-snapshot acquire pairing.

### Added
- **Per-part microtonal tuning (firmware raga restore + custom tables +
  Scala import).**
  - **Fidelity restore:** the Ambika controller firmware already had per-part
    microtonal tuning — `PartData.raga` (byte 4) selecting one of 32 scale
    tables applied by firmware `Part::TuneNote` — which the original Parvati
    port had dropped (only octave + fine tuning were ported). The 30 scale
    tables + the 32-entry dispatch (bageshree→kafi, rasia→yaman aliases) are
    now vendored verbatim into `Source/TuningTables.cpp` (GPL-3.0 provenance,
    NOTICES.md; do-not-edit policy like `dsp/resources`). Exposed as the
    `part_raga` ("Scale") choice parameter — index == raga byte (0..32,
    file-faithful superset of the hardware UI's 0..31). The firmware does NOT
    map raga over CC/NRPN (UI-only there), so the parameter carries no MIDI
    mapping. Restoring raga also restores the firmware AcceptNote behaviour:
    note classes muted by the scale (32767 sentinel) are REFUSED, not voiced
    as garbage pitch (a deliberate, documented deviation from the firmware's
    clamped arithmetic).
  - **Custom tables:** per-part 12-entry tables (±127 in 1/128-semitone
    units) staged message-thread → audio-thread (`tuningDirty_`, the
    frameDirty_/fxDirty_ pattern) and applied at the single note→pitch
    choke point (`AmbikaVoice::startNote`, raw-note class indexing like the
    firmware). Mode resolution: 0 = 12-EDO, 1..32 = raga preset (== byte 4,
    which also round-trips .MUL/.PRO), 33 = custom (byte 4 kept 0 while
    active — `setPartTuningCustom` enforces this, so a leftover preset can
    never shadow the user's table and hardware export stays clean). Custom
    tables may carry the mute sentinel themselves (Scala imports of keymaps
    with unmapped classes): it survives storage verbatim and refuses those
    note classes exactly like a raga preset. No `Source/dsp/` changes — the
    bit-exactness policy is intact; resolution is capped at the hardware's
    1/128-semitone (~0.78 ¢) oscillator quantum by construction.
  - **Scala import:** `parvati::importScala` converts `.scl` (+ optional
    `.kbm`) into the custom table, enforcing the hardware contract
    ACCURATELY rather than approximating: 12-key mappings only (S≠12
    rejected), octave-repeating periods only (the formal octave must be
    1200 ¢ within the 1/128-st quantum — non-octave scales like
    Bohlen-Pierce are rejected, not mangled), kbm degrees beyond the formal
    octave rejected, short keymaps rejected (deterministic rule), offsets
    clamped to ±127 with per-class warnings, unmapped classes become the
    firmware mute sentinel, reference-frequency deviations (e.g. A = 432 Hz)
    fold in exactly, single final rounding from full double precision, and a
    C-locale parser (no locale-dependent numbers). Warning taxonomy: always
    -on resolution-cap notice, clamped classes, muted classes, subset /
    duplication notes.
  - **State v7 + persistence:** the engine-state blob gains a per-part
    length-prefixed tuning block (`{mode; offsets[12]}`); `restoreState`
    accepts v1..v7 (a v6 blob restores presets from its raga byte, customs
    cleared). Migration tradeoff (same accepted class as v5→v6): an OLDER
    Parvati build strictly rejects a v7 blob and falls back to the legacy
    single-part APVTS restore. `.parvati` multis carry `tuning_mode` +
    `tuning_offsets` per part behind `hasProperty` guards (old files load
    unchanged; new files load in old builds as no-ops). The raga byte rides
    `.PRO`/`.MUL` unchanged (custom tables are Parvati-only: they do not
    export to hardware formats).
  - **Standing-bend pickup fix (pre-existing gap, fixed alongside):** voices
    triggered while a pitch wheel is off-centre now inherit the standing
    bend from a per-channel latch (`lastWheel_`, wheel-centre default);
    previously a new note started un-bent until the next wheel event —
    audible with MPE and latched wheels. Per-channel: other channels are
    unaffected.
  - Tests: `parvati_tuning_test` (vendored-table fidelity incl. the 7 muted
    classes of kaushik todi, hook mapping/composition, sentinel gates, both
    staging paths, v7 round-trip + hand-crafted v6 view, `.parvati` + `.MUL`
    round-trips, standing bend) and `parvati_scala_import_test` (full
    expected-table fixture corpus — 12tet / ji12 / penta+kbm / edo19+kbm /
    bohlen / root62 / x432 — plus grammar edges and the malformed-input
    taxonomy). Scala fixture expectations were hand-derived independently
    from the documented math and cross-checked digit-by-digit.
  - **UI surface (Patch page):** a per-part **Tune** column — "12-EDO" + the
    32 firmware presets + "Custom…", which opens the per-part tuning popover
    (`Source/ui/TuningEditor.{h,cpp}`): twelve note-class rows with a
    1-unit drag/step control (±127), a quantized-cents readout (the honest
    hardware resolution, never a finer promise), double-click-to-zero, an
    inline warning/error band, [Import .scl/.kbm…] (the FileChooser path
    over `parvati::importScala`, warnings surfaced inline — mobile-friendly,
    no alert popups), [Clear] and [Done]. Applying is live (every edit lands
    via `setPartTuningCustom`; closing without [Done] loses nothing), rows
    prefill from the currently resolved table (tweak-a-preset-into-custom
    flow), and the popover scrolls in short AUv3 panes (the MulExportDialog
    pattern). The .MUL export-fallback dialog now also warns when any part
    uses a custom tuning (naming the parts — .MUL cannot carry custom
    tables). ParamHelp gains the `part_raga` entry (incl. the keytracking /
    NOTE-mod-source side effect) and a `part_tuning` resolution note;
    SynthParamLabels decodes the raga byte (pure regression entry). Row
    columns re-budgeted (Cards/Voices/Ch/Zone/Tune) against measured caption
    and preset-name text widths; HIG 44pt bands preserved. Test additions:
    multigui [9] (Tune combo path, editor prefill/live-apply/Clear, Scala
    import incl. sentinel + rejected-import-untouched), export-fallback [l]
    (custom-tuning warning + part naming), synth-paramtext (part_raga
    decode), tuning (custom sentinel round-trip).
- **CLAP plugin format + Linux build support.**
  - CLAP via clap-juce-extensions (JUCE 9.0.0 has no native CLAP `FORMATS`
    value; native CLAP is on the JUCE roadmap). The extension is fetched at
    configure time from a pinned main commit (no tagged release contains the
    JUCE 9 support); `-DPARVATI_CLAP_EXTENSIONS_PATH` swaps in a local
    checkout. `-DPARVATI_BUILD_CLAP=OFF` disables the format (default ON on
    desktop, forced OFF on iOS). `CLAP_ID` is `com.805labs.parvati` (aligned
    with the AU/VST3 bundle id and stable for host identification);
    features `instrument synthesizer stereo`.
  - Desktop formats now split by platform: macOS keeps VST3 + AU (+CLAP);
    Linux/Windows build VST3 + Standalone (+CLAP) — the classic AU is
    macOS-only (a non-Apple configure with AU is a juce_add_plugin fatal
    error). iOS is unchanged (AUv3 + Standalone).
  - Linux configure support: `parvati_perf_smoke_test` is APPLE-gated (its
    pump drives the CoreFoundation run loop and `#error`s off Apple); the
    macOS-only `deploy` target is now gated `APPLE AND NOT PARVATI_IOS` and
    also builds + installs the CLAP bundle alongside VST3/AU
    (`~/Library/Audio/Plug-Ins/CLAP`).
  - README/CONTRIBUTING document the CLAP format, the Linux build, and the
    new CMake options.
- **Per-part voice slots + part names + Drum Kit template (voice-card
  polyphony extension).**
  - Engine: each Part has a `voiceSlots` setting (0 = **Auto**: one voice
    per allocated voicecard — the faithful 6-voice hardware behaviour;
    1..16 = a fixed count) drawn from a fixed 96-voice pool, so every Part
    can be maxed out simultaneously with no cross-Part stealing. The
    voicecard bitmask keeps ownership / aux-out routing / `.MUL` export: a
    Part's voices are tagged round-robin onto its own cards, and a
    card-less Part stays disabled. Idle pool voices are self-gated (no DSP
    cost until played). Voices that drop out of a Part's set on a rebuild
    now get a graceful tail-off release (previously they could sustain
    forever when a card/slot was removed mid-note — latent bug).
  - Mono is **per-card**: MONO fires exactly one voice per allocated card,
    so the unison size and CPU are invariant under the slot setting, and
    MONO + 1 card is true single-voice mono.
  - Part names/aliases ("Kick", "Lead"): editable on the Patch page rows,
    shown in the top-bar Part selector, carried by the `.parvati` multi
    format and the host engine state (v6; v1..v5 blobs restore as unnamed).
    The Ambika `.MUL`/`.PRO` formats carry no name/slot bytes — hardware
    export keeps the faithful bitmask representation.
  - New **Drum Kit (GM)** template: a 6-part drum multi with one GM note
    per part (Kick 36 / Snare 38 / Clap 39 / Closed Hat 42 / Open Hat 46 /
    Tom 45), Omni channel, 4 slots + CYCLIC round-robin per drum, named
    parts, and short percussive patches (noise hats, pitch-dropped
    sine kick/tom).
  - Tests: `voice_slots_test`, `drum_kit_test`; multitimbral/multi_load/
    host_state updated to the pool-model behavioural contract.
  - **`.MUL` export fallback dialog** (Save > Ambika Multi (.MUL) — the
    entry itself is new). When the setup requests more voices than the
    6 hardware voicecards can express, a strategy dialog previews and picks
    the voice-to-card mapping: **Proportional** (largest-remainder split by
    request, default), **Priority** (first-wins), **Even Split**, **Mono
    Fold** (constrained parts fold to MONO unison), **Chain Split** (writes
    extra "-2.MUL" unit files for physically chained Ambikas; heads get
    CHAIN mode and forward overflow on the same channel/key zone), and
    **As-Is** (legacy bitmasks, slots ignored). Pure solver in
    `Source/MulExport.*`; `.parvati` exports never degrade.
  - **Byte-exact hardware files (found + fixed by the new byte-level
    test):** the .PRO/.MUL name chunk now ends with the firmware's
    NUL terminator at byte 15 (string + space pad + NUL, verified
    against three reference hardware files). Re-written exports are
    now byte-for-byte identical to real Ambika files. New
    `export_bytes_test` memcmps full files against hardware
    references, walks the raw RIFF chunk layout (tags/sizes/type
    prefixes/interleaving), and verifies the export strategies at the
    raw-byte level (only MultiData[i*4+3] + folded PartData[15]
    change; patches/routing stay untouched).
  - **Unicode-safe names.** The .PRO/.MUL 16-byte name chunk now
    truncates at a UTF-8 code-point boundary (a raw 16-byte copy
    could split a multi-byte character, writing invalid UTF-8 into
    the hardware file) and drops control characters. The .parvati
    parser now unescapes the serializer's `\"`/`\\` escapes (a
    part name containing a quote round-tripped with literal
    backslashes). `setPartName` strips control characters (a
    newline in a name would corrupt the line-based .parvati
    document on save); the host-state restore path sanitizes too.
    New `mul_strategies_test`: a scenario-matrix x strategy
    invariant suite (card conservation, mask contiguity/disjointness,
    min-1-per-active-part, proportional monotonicity, chain segment
    conservation + CHAIN heads), end-to-end playable-polyphony
    checks per strategy, and the unicode/name battery (accented /
    CJK / emoji / quote / newline names across .parvati, host state
    and the hardware name chunk). ASCII hardware files remain
    byte-identical.

### Fixed
- **iOS Info.plist plumbing (T1/T2/T5/T16 of the iPadOS audit — see
  audit/IPAD_TOUCH_TODO.md).**
  - The AUv3 app-extension shipped with a `CFBundleIdentifier` of
    `com.805labs.parvati.parvatiAUv3` (juceaide hard-codes
    `<app-id>.<last-component>AUv3`) while `PRODUCT_BUNDLE_IDENTIFIER` and
    the entitlements `application-identifier` both said `com.805labs.parvati.AUv3`
    — a code-sign/App-Store validation failure waiting for the first
    real-device/.ipa build. The plist literal now agrees with both
    (PlistBuddy `Set` on the juceaide-generated source plist).
  - The Standalone plist was patched at configure time behind a silent
    `if(EXISTS …)` + `ERROR_QUIET` guard that could no-op and drop keys.
    Nearly all app-level keys (device family, orientation arrays,
    `UIRequiresFullScreen`, background audio, file sharing) are now passed
    NATIVELY via `juce_add_plugin` keyword arguments, so juceaide generates
    them into the plist at configure time with no custom patch step at all;
    the one remaining patched key (`LSSupportsOpeningDocumentsInPlace`)
    fails the configure loudly instead of silently skipping.
  - Added `UIRequiresFullScreen true` — fixes the ITMS-90474 App-Store
    rejection (landscape-only orientations + Slide Over opt-in are
    incompatible) and disables Split View, as intended for the layout.
  - `TARGETED_DEVICE_FAMILY` is now iPad-only (`2`): the 1024pt editor width
    floor cannot fit iPhone AUv3 panes; revisit when a compact layout exists.
  - Deleted the dead `JUCE_XCODE_EXTRA_PLIST_ENTRIES ""` no-op.
- **FV-1 FX family: denormal (subnormal) flush in the RateBridge BW-limit
  biquads.** The 4th-order Butterworth input/output filters shared by all
  five FV-1 effects (Clocked Delay / Ensemble / Plate Reverb / Vinyl
  Compressor / Phaser) are Direct-Form-II-Transposed IIRs whose `z1`/`z2`
  state decays toward 0 but never reaches it in finite steps when fed silence
  (a paused track, the gap between notes, or a reverb/delay tail). The state
  passed through the subnormal range (~1e-38..1e-45), where x86 CPUs stall
  ~50× — a real-time audio-thread killer. Surfaced by
  `parvati_fx_param_coverage_test` (VinylCompressor emitted ~200 subnormals on
  a silence tail). Fix: flush `z1`/`z2` to 0.0f below `FLT_MIN` — inaudible
  (~280 dB below the 15 kHz target) and removes the stall.

### Added
- **Comprehensive test coverage for every synth parameter and every FX
  module/parameter**, consolidated into two ~6.3 MB binaries (instead of
  ~35 separate ~6.5 MB binaries ≈ 220 MB):
  - `parvati_synth_param_coverage_test` — sweeps all 181 synth parameters
    (a generic byte-routing net over all 105 patch/part params + targeted
    audio checks per family: oscillators, mixer, filter, envelopes, LFOs, mod
    matrix, part, sequencer, arp, options). 48 checks.
  - `parvati_fx_param_coverage_test` — all 16 FxType values; every effect's
    5 slot params swept (active moves, inactive exactly inert); 3×6 topology/
    order routing; master mix + 3-band EQ; **all 18 FX mod-matrix destinations
    proven to reach the DSP at full depth**; condition-dependent Clouds params
    (looper Size/Pitch under freeze; Spectral Position under freeze) verified.
    292 checks. See `tests/COVERAGE_SPEC.md` (intended outcomes) and
    `tests/COVERAGE_FINDINGS.md` (bugs fixed + verified behaviours).

### Added
- **Five FX effects gained new parameters, reordered by signal-path flow.**
  Each effect's generic slot params were re-mapped left→right to follow the
  audio path (input → processing → output), and new controls were added by
  either un-hardcoding a constant the engine already accepts (Tier-1) or a
  one-pole Tone/HP filter (Tier-2) — no effect's core character changes
  unexpectedly:
  - **Wavefolder** (2→4): **Drive** (1×–4× pre-gain into the fold) → Fold →
    Bias → **Tone** (post-fold one-pole LP that tames the harsh upper harmonics
    the fold generates; full-bright = bit-identical to before).
  - **Frequency Shifter** (3→4): Shift → **Shape** (carrier wavetable timbre —
    pure un-hardcode of the `0.0` sine argument already passed to
    `QuadratureOscillator::Render`; 0 = sine = original) → Feedback → Spread.
  - **Reverb** (3→5): **Predelay** (0–200 ms host-rate ring before the tank; a
    musical delay, not latency-compensated) → Diffusion → Time → Tone →
    **Low-Cut** (post-tank one-pole HP, 15–450 Hz, to shed mud).
  - **Pitch Shifter** (2→3): Pitch → Size → **Spread** (offsets the right
    channel's read taps for chorusy stereo width; 0 = mono = bit-identical).
  - **WSOLA Stretch** (3→5): Pitch → Position → Size → **Freeze** (Tier-1:
    gates the record-buffer write, identical to the looper — holds the loop) →
    **Tone** (post one-pole LP). _A dedicated time-stretch **ratio** was
    deferred: the vendored WSOLA engine exposes no independent stretch rate, so
    a true Stretch knob would need invasive correlator/window-scheduler surgery
    (the crackle-prone area in `FX_CRACKLE_INVESTIGATION.md`); Freeze + Tone give
    real musical value at zero risk._
  Per-type engagement defaults (`fxTypeDefaults`) and the slot visualizer's
  per-type param mapping were updated to the new signal-path order. Reordering
  is an index remap of the generic `fx{N}_param1..5` slots (no new parameters
  were added to the APVTS — they were already 5/slot); no saved Parvati presets
  reference FX params yet, so there is no preset-remap impact.

### Added
- **Per-slot FX now expose up to 5 params + a fixed Dry/Wet.** Each FX slot's
  generic param count rose from 4 to 5 (`kNumFxSlotParams` 4→5; a new
  `fx{N}_param5` parameter + `FX_DST_FX{N}_P5` mod-matrix destination per slot).
  Two effects gained a meaningful 5th control by un-hardcoding previously fixed
  DSP constants: **Spectral** adds **Freeze** (was hardcoded off — holds the
  current spectral frame) and **Resonator** adds **Structure** (was fixed at the
  Rings default 0.25 — modal inharmonicity/layout). The card's knob grid is now a
  FIXED 3-column × 2-row layout, so the **Dry/Wet knob is anchored in the
  bottom-right corner consistently** regardless of effect type, and it is
  **hidden entirely when the slot type is None** (a None slot has no mix). The
  FX mod-matrix offset table grew accordingly (`kNumFxSlots * 5` →
  `kNumFxSlots * (kNumFxSlotParams + 1)`).

### Changed
- **Host state format v5.** The per-part FX engine-state blob is re-versioned
  4 → 5 to carry the 5th param per slot (78 bytes/Part, was 75). Restore is
  back-compatible: a v4 (or earlier) blob loads with the 5th param at its neutral
  default, version-gated so legacy field offsets are read correctly. Parallel FX
  routing is **already true-parallel** (both branches process the full stereo
  signal and are summed — NOT a left/right split); a headless measurement test
  (`tests/parvati_fx_stereo_balance_test.cpp`) confirms the Spectral effect has
  no systematic L/R bias (only a stochastic, by-design texture-freeze variance at
  blur < 0.5), so no DSP change was needed for stereo.
- **Ambika patch/multi load resets the FX section to a clean slate.** Loading a
  legacy Ambika `.PRO` (single program) or `.MUL` (multi) — which carry no FX
  information — now resets every affected Part's FX to the defaults (all slots
  `None` / bypassed / dry, Series topology, flat master EQ, cleared FX mod
  matrix) instead of leaving the previously-loaded patch's FX active. The
  `.parvati` native formats are unaffected (they carry their own FX).

### Added
- **User-friendly synth readouts.** The SYNTH-page knob readouts (OSC / MIX /
  FILTER / ENV / LFO / MOD / SEQ / ARP / GLOBAL) now show human-readable units
  instead of raw 0–127 numbers — e.g. an LFO rate shows `"2.4 Hz"` (free-running)
  or `"1/16"` (tempo-synced); an envelope time shows `"173 ms"` / `"1.50 s"`;
  filter cutoff shows Hz; oscillator range/detune show semitones/cents; mod
  amounts, sustain, resonance and mix levels show `%`; sequencer note-steps show
  note names (+ gate glyph) and the patch-page key-zone knobs show note names
  instead of MIDI numbers. Choice params and the FX section were already
  friendly and are untouched. The formatting is **display-only** — a pure
  value→string function (`Source/ui/SynthParamLabels.cpp`) wired through the
  existing `ParamControl::setDisplayValueText` hook (the same pattern the FX
  section uses), so stored values, APVTS, serialization and the shared knob
  painter are never touched. Note: a few knobs whose DSP mapping is non-trivial
  (part portamento/volume, mix crush) intentionally show a `%` fallback, marked
  for a later verify pass.
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
- **Arpeggiator / note-sequencer ran ~24× too fast + stuck notes.** Two
  pre-existing engine bugs (not a regression from the recent sequencer-UI work):
  - *Speed:* the Sequencer's `clockTick` was called every raw transport tick
    (24 PPQN) while only the Arpeggiator self-prescales, so the note sequence
    (and the two modulation sequences) ran ~24× too fast at the default 1/4
    resolution — a buzzy trill instead of one note per arp step. Fixed by gating
    `seq.clockTick` on the arp's prescaled step (the Arp's `clockTick` now returns
    whether it fired), matching the firmware which runs `ClockSequencer()` and
    `ClockArpeggiator()` from the same prescaled branch. The modulation sequences
    now also advance at the correct rate.
  - *Stuck note:* `Sequencer::clockTick` had no cleanup when the last held key was
    released, and there was no `Sequencer::allNotesOff()`, so the sounding note
    was stranded (and `seq.start()` orphaned it permanently across transport
    restarts). Added `Sequencer::allNotesOff()`/`stop()` and wired it to every
    strand point — key-release-empty-stack (fires even when the clock has
    stopped), transport stop, before `start()`, and mode→OFF — plus a defensive
    self-clean branch in `clockTick`.
- **Note sequencer "only works from 50% of the range" + length control + a
  velocity clip.** The note-step knob crammed (note | gate) into one 0..255 byte,
  so the lower half (0..127, gate off) was a silent dead zone — only 128..255
  played. Each note step is now a single remapped rotary where the whole dead
  zone collapses into one **"Rest"** stop at the minimum and **1..128 = notes
  0..127** (gate on) across the rest of the travel, so the full range is audible.
  It bridges the existing byte param via `getParameterAsValue` +
  `addParameterListener` (the `FxSlotCard` composite pattern) — no new params, so
  presets/MIDI-learn/serialization are untouched. The 1..16 **sequence-length**
  knob is replaced by a **− [n] + stepper** (a knob was the wrong control for a
  small count). Also fixed a latent clip: the Note-Sequencer view stacked Note
  Pitch + Note Velocity (~390px) in a ~290px non-scrolling host, so Note Velocity
  was ~75% clipped — the view now shows Note Pitch only (Option A); Note
  Velocity remains editable in the full Sequencer tab. Velocity itself is
  unchanged.
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
  sanitizer / `-Werror` flags are MSVC-guarded (no-op under MSVC).
  README/CONTRIBUTING document Windows.
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

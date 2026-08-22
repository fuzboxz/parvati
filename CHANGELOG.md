# Changelog

All notable changes to Parvati. Dates are approximate (local dev chronology).

## [Unreleased]

### Added
- **Voicecard audio oracle in `firmware_parity_test` (2026-08-22).** The REAL
  firmware voicecard (ambika_reference/voicecard — Voice::ProcessBlock,
  oscillator, resources, audio ring) now compiles into the unified test binary
  wrapped in namespace `fwvc` (the controller and voicecard resources.cc share
  table symbol names with DIFFERENT generated contents, so both trees cannot
  link under their own names). New scenario [10] drives firmware Voice and the
  port's dsp::Voice block-by-block with identical patch bytes, note, and
  re-seeded RNG: static mix CVs must render BYTE-EQUAL, a mix_balance tick
  must diverge exactly on the tick block (the sanctioned mix-gain-glide
  divergence, now allowlisted and exercised per the known-divergences
  contract), and the renders must re-converge. Shim surface added:
  avr/io.h (E2END/_BV), avrlib/gpio.h shadow, and op.h additions (U8MixU16,
  the U24 family, S8S8MulShift8, U8U4Mix*) decoded from the real header's
  asm comments. Fixing the shim's U8Mix to the asm's single-shift form
  (it double-truncated, ±1 LSB/sample vs firmware) is what made the
  byte-level equality possible at all.

### Fixed
- **Envelope preview: minimal theoretical sustain width (2026-08-22).**
  With time-honest spans, attack 1 ms / decay 1 ms / sustain 100% / release
  1 ms rendered as three ~1/3-width fades — the sustain plateau now has an
  ABSOLUTE floor (~0.6 s hold in engine ticks) so that patch reads as an
  instant jump, a long plateau, an instant drop. The floor only binds when
  every timed segment is short; longer patches keep the representative
  share. Pinned by editor_test [19](d).
- **Mod-pill strips: min-max (oscilloscope-style) rendering (2026-08-22).**
  Each plotted point now carries the min/max of the RAW ring samples its age
  range covers, rendered as a filled band with a stroked outline — the TRUE
  amplitude envelope. Fast LFO/env/arp sources (more than ~1 cycle in the
  window) previously point-sampled into mush regardless of point count; the
  band cannot alias. Slow sources degrade gracefully: min ≈ max everywhere
  and the outline edges coincide, reading exactly as the previous trace
  line. Replaces the Bézier-smoothed centerline (no smoothing needed — the
  band's edges ARE the data).
- **Settings scrollbar now theme-coloured (2026-08-22).** A 2026-08-21
  override muted the drawer scrollbar's thumb to backgroundInput ("quiet
  thumb"), which read as an unthemed gray bar. The override is removed — the
  drawer scrollbar inherits the ParvatiLookAndFeel's themed rendering
  (accent-coloured thumb, base track, hover brighten), correct for every
  theme and re-applied on theme switches.
- **Envelope preview: time-honest ADSR spans + edge padding (2026-08-22,
  supersedes the 2026-08-20 attack floor).** Segment widths are now
  proportional to the segments' ACTUAL engine durations (the same env-LUT
  the envelopes run on), so a 1 ms attack/release renders as the true
  near-vertical ramp — the old ~8% attack floor made fast attacks read as
  "plenty of attack". A few pixels of silence pad the plot before the attack
  and after the release (kEdgePad) so the steepness is readable against the
  flat padding. The live stage marker is remapped through the same padding;
  editor_test [19]/[25] pins re-derived from the display's own span
  definition (no drifting hard-coded constants).
- **Mod-pill strips: fixed-length pre-zeroed window (2026-08-22 user spec).**
  The strip now renders the FULL ring as a circular buffer of constant
  length: slots never written yet (fresh open, or a source that has never
  produced data) read as ZERO, so an unmodulated source shows a full-width
  zero line from the first tick exactly like a modulated one — no growing
  window, and the scroll speed is constant end to end (the end-of-buffer
  phase change is gone). Supersedes the right-anchored partial-fill layout.
- **Settings drawer: language row unreachable / scrollbar never appeared
  (2026-08-22).** Two stacked defects. (1) JUCE's Viewport auto-scrollbar was
  effectively dead in this drawer: updateVisibleArea has an early-return
  path (content repositioning) that skips the bar's setVisible — observed as
  a content area narrowed for a bar that never appears — and with
  allowVerticalScrollingWithoutScrollbar (the default) false, wheel
  scrolling was disabled whenever the bar was hidden: NO way to reach the
  rows below the fold (the language row). Fix: the viewport now sets
  allowVerticalScrollingWithoutScrollbar (wheel/trackpad scrolling always
  works), the tracker asserts the bar's visibility explicitly, and the
  editor's 30 Hz tick re-asserts it while the drawer shows (JUCE can re-hide
  it on view-position changes). parvati_settings_probe pins the contract:
  overflow => bar visible after one tick AND scrolling to the bottom brings
  the last row fully into view.
- **Mod-pill strips: non-constant scroll speed + fast-LFO aliasing
  (2026-08-22, the "still looks weird" follow-up). (1) A partial history
  buffer was stretched across the full strip width, so right after every
  open/reset the trace RACED (apparent speed = width/count — up to ~3x) and
  decelerated as the buffer filled. Strips now render right-anchored by
  absolute age behind the newest sample: every append moves the trace
  exactly one slot — constant speed from the first append, partial history
  fills toward the left. (2) kStripMaxPts 48 -> 96: at the 256-append
  window, 48 points sampled every ~5 appends aliased fast LFOs (~14
  appends/cycle) into mush; 96 ≈ 1px spacing on the pill restores the
  waveform reading.
- **Settings drawer first-open latency (2026-08-22).** The drawer slid in
  blank and the content popped at the END of the ~250 ms slide: JUCE's
  SidePanel animation runs through a proxy component that snapshots the
  drawer when the slide STARTS, but the panel was sized from
  onPanelShowHide, which fires only AFTER the animation. The open path
  (gear button + the test seam) now pre-sizes the panel via
  SettingsScrollTracker::preSizeForOpen() BEFORE showOrHide(true), so the
  proxy snapshot already contains the laid-out rows and the drawer slides
  in fully drawn. parvati_settings_probe pins the new contract: the viewed
  panel must be non-zero SYNCHRONOUSLY at the open call, before any
  animation pumping.
- **Mod-pill strips scrolled visibly faster than the previews/indicators
  (2026-08-22).** The pill sparkline window spanned 1.57 s — much shorter
  than the envelope/LFO preview axes — so identical motion crossed the
  pills ~2x faster and read as "speeding off". kHistoryLen 128 -> 256
  (~3.13 s window): the strips now scroll at half the apparent speed with
  identical time fidelity (the append rate is unchanged; only how much
  history one strip spans). The idle drag-out pace follows (1 byte/append
  = a full-scale fall across exactly one window), ui_telemetry_test's
  window/saturation timing updated to the new contract.
- **Settings drawer rendered blank (2026-08-22).** The drawer's
  SettingsScrollTracker gated panel sizing on Viewport::getViewWidth(), but
  that returns jmin(viewedComponentSize, viewportSize) — and the viewed
  component IS the panel being sized (0x0 while collapsed). A circular gate
  that could never open: every settings row stayed 0x0 (all four probe
  scenarios, theme switch and close+reopen included). The tracker now sizes
  from the viewport's own bounds (budgeting the auto vertical scrollbar);
  parvati_settings_probe also gained the regression gate it was missing — it
  used to return PASS unconditionally — and now FAILs unless the viewed
  panel is sized, no named row is collapsed, and the named-row census
  survives (>= 5).
- **Fast mono note changes cut out / clicked (2026-08-22).** A mono
  retrigger while the previous note's release tail still sounds routed
  through juce::Synthesiser::startVoice, whose pre-emptive stopNote(0,false)
  -> Voice::Kill ZEROED the envelope — every fast note change re-attacked
  from silence (with a ~0.5 s attack that is an audible chop). The firmware
  re-Triggers the SAME voicecard: Envelope::Trigger(ATTACK) seeds its start
  from the CURRENT value. Fix: the engine's retriggerVoice now goes through
  startVoice with the kill guard neutralized (armRetriggerContinuation drops
  exactly the currentlyPlayingSound pointer that arms the guard — full
  truthful JUCE bookkeeping: note, channel, noteOnTime, pedals), and the
  MONO path uses it for ANY active voice (release tail included), not just
  legato overlaps. continuityNext_ also keeps the sounding voice's
  resampler FIFO and gain flowing through startNote (no ~1 ms de-click hole,
  no interpolator cold-restart click). New regression test
  mono_retrigger_continuity_test pins the envelope continuity directly
  (tail=182 -> 183 continuing vs 9 killed) — verified to fail on the old
  routing. (Note: the firmware's osc_2 phase Reset blip on retriggers is
  real Ambika behavior and stays.)
- **VCA gain now glides across internal blocks (2026-08-22).** The default
  output path applied the VCA gain once per 40-sample internal block — a
  zero-order-hold staircase whenever the envelope moves (the analog VCA
  hardware smooths these CV steps). The gain now ramps linearly across each
  block in both the 1x and oversampled paths (static CV -> zero diff ->
  bit-identical). Honest caveat, pinned by a synthetic-validated matched
  filter: the staircase measures at/below the engine's 8-bit quantization
  noise floor (~1%) even before the glide, so an output-domain regression
  test cannot separate the two — the user-audible "tiny noise in the
  release tail" is dominated by the 8-bit engine's own quantization noise,
  which is hardware-faithful Ambika character (same DAC, same tail noise on
  the real synth). release_vca_glide_test instead guards the class: no
  block-rate CV leakage above 2.5x the quantization floor at either the
  internal rate (980.4 Hz) or the host block rate (172 Hz — a CV applied at
  the wrong rate entirely).
- **Mix Balance / Mix Param zipper on knob drags (2026-08-22).** The mix
  crossfade gains were latched once per 40-sample block (line-for-line
  firmware port of voice.cc:441-442); in hardware the analog mixer smooths
  the DAC steps, but the digital port had no analog domain, so each balance
  tick stepped the gains by up to 16/255 of the waveform — audible zipper.
  The gains now glide linearly across each block (~0.9 ms slew, 8.8
  fixed-point, exact target landing — no residual), emulating the analog
  smoothing. Registered as the sanctioned "mix-gain-glide" firmware
  divergence and pinned BOTH ways by the new voicecard audio oracle
  (static CVs byte-equal; reverting the glide fails parity loudly —
  verified). The synth drag probe's mix_balance row was removed: the
  diff-census metric measures legitimate shape-change artifacts on any
  balance sweep (~0.06 regardless of the glide — verified by direct
  accumulator instrumentation) and cannot police this param; the probe's
  osc1_shape setInt also silently no-opped (choice param) and now uses
  setChoice. Full suite: 117/117 (first fully green run).

- **Untrusted-input boundary normalization (2026-08-22, memory-safety
  wave).** New `Source/dsp/patch_sanitizer.h` normalizes raw Patch (112 B)
  and PartData (84 B) bytes to their firmware-valid domains at INGESTION —
  wired at the two paths that previously pushed file/host bytes into the
  engine with no validation: `.MUL` multi loads
  (`ParvatiAudioProcessor::loadMultiFile`) and host-state blob restores
  (`SynthEngine::restoreState`). The `.PRO`/`.parvati` paths already clamp
  via their APVTS/descriptor decodes. All bounds derive from the patch.h
  enums (plus a static_assert against `kNumTuningPresets`), so they cannot
  drift from the DSP definitions; the sanitizer is the IDENTITY for
  legitimate firmware files (round-trips stay byte-exact — pinned by
  roundtrip/golden/multi_load/host_state/loader_fuzz) and deterministically
  narrows hostile blobs at every sink at once. The 2026-08-18 DSP-side sink
  clamps stay (defense in depth). Regression cover:
  tests/patch_sanitizer_test.cpp (unit domains + identity + END-TO-END
  wiring proofs through a real written `.MUL` and a real captured/restored
  state blob).
### Tools
- **`tools/run_tests_parallel.sh` (2026-08-22).** Parallel driver for the
  unified suite: greedy work queue across N lanes (default: performance
  cores, capped at 8; `PARVATI_TEST_JOBS` overrides; positional args select a
  subset), long-pole tests (loader_fuzz/perf_smoke/lifecycle/...) scheduled
  first. Safe because tests are self-contained and every shared mutable path
  goes through JUCE `File::tempDirectory` = `$TMPDIR/<exe>/` on macOS — each
  lane gets its own TMPDIR, so fixed-name temp fixtures ("b.PRO",
  "a.parvati", ...) stay lane-private. Full 117-test suite: ~9 min wall
  (bounded by loader_fuzz_test) vs ~15-20 min sequential; same exit-code
  contract (number of failures), per-test logs under the run dir. The
  sequential runner stays canonical.

### Build
- **libc++ hardened mode FAST in optimized builds (2026-08-22).**
  `_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST` for Clang/
  RelWithDebInfo/Release configs (skipped under sanitizers/Debug): the
  std::array/std::vector bounds checks the memory-safety migration leans
  on stay ACTIVE in shipped artifacts instead of Debug-only. Near-zero
  overhead; inert macro on older toolchains.

### Changed
- **Memory-safe C++ migration of the DSP layer (2026-08-22).** The
  first-party DSP code now makes its buffer/index contracts un-breakable by
  construction, with zero runtime cost and bit-identical audio (validated by
  firmware_parity / roundtrip_golden and the full 116-test suite; the only
  red row is the documented known-red Mix Balance probe):
  - Oscillator / SubOscillator / TransientGenerator render APIs take
    `uint8_t (&)[kAudioBlockSize]` sized references instead of `uint8_t*` —
    a mis-sized buffer is now a compile error, not a silent overflow — and
    the oscillator dispatch table is a size-checked `std::array`.
  - `Voice::set_patch_data` rejects addresses past the 112-byte Patch
    struct (a malformed preset / stray host write previously walked into
    Part / tempo / envelope memory); the unused raw-alias
    `mutable_patch_data()` is removed; modulation-source/destination
    accessors guard the fixed 31/19-slot arrays; `mutable_envelope` and
    `TriggerEnvelope` clamp; a hostile envelope stage sinks to DEAD instead
    of indexing past the 5-slot stage arrays.
  - `Voice::output()` returns a sized `std::array` reference instead of
    `const uint8_t*`, closing the last raw-buffer seam in the voice render
    path — `AmbikaVoice`'s crush (sample-and-hold) shadow now carries the
    same sized-array contract (pointer-to-sized-array, not `uint8_t*`),
    pinned end-to-end by patch_load_test [B5] (crush=28 alters the
    waveform) under ASan+UBSan.
  - `FxProcessor::setParams` (all 25+ effects) takes a bounded
    `std::array<float, kNumFxSlotParams>` instead of a decaying
    `const float[5]`; `ResourcesManager::Lookup(resource, i)` rejects
    out-of-range resource ids; the FxChain retired-processor parking is
    encapsulated in a `RetiredLot` type that owns every delete (RT-safe
    atomic slots unchanged — TSan-clean x4).
  - Regression cover in tests/dsp_memory_safety_test.cpp (all guards pinned);
    ASan+UBSan sweep clean on the DSP/concurrency/FX-clouds subsets.

### Fixed
- **Vendored-Clouds UB under UBSan (2026-08-22).** Found by the sanitizer
  sweep during the migration: the WSOLA correlator's `>> (32 - offset_bits)`
  shifts a 32-bit word by 32 when offset_bits == 0 (UB; the firmware's ARM
  `lsr` maps it to 0 — now reproduced via a 64-bit intermediate), and the
  phase vocoder's scaled phase delta could leave the uint16 range (direct
  float->uint16_t conversion is UB — now wraps mod 2^16 through int32, the
  arithmetic upstream intended). Audio is unchanged for in-range values.

### Build
- **`PARVATI_FORMATS` cache option + tests-only sanitizer trees (2026-08-22).**
  New cache option selects the plugin formats to build; default is unchanged
  (macOS: Standalone;VST3;AU, CLAP via `PARVATI_BUILD_CLAP`). An EMPTY value
  configures tests-only (no plugin targets), and `tools/run_sanitizers.sh`
  now uses it: fresh sanitizer dirs are 1.4 GB vs 9.6 GB before (they had
  built full instrumented AU/VST3/CLAP/Standalone bundles that never ran).

### Removed
- **GitHub Actions CI (2026-08-22, user request).** `.github/workflows/`
  (the PR-gate + nightly lanes added with the memory-safety wave) is
  removed — no hosted CI is wanted for this repo; the validation it ran
  stays fully runnable locally (the unified suite, the parallel runner,
  and `tools/run_sanitizers.sh`).
- **Static cutoff reference line in the filter preview (2026-08-22, user
  request).** The 1px vertical line at the cutoff frequency is gone — the
  curve's knee already carries the position and the line read as noise
  (same cleanup class as the earlier corner-label removal).
- **FX-slot graphic visualizers (2026-08-20).** The per-slot FxSlotVisualizer
  canvas (the dimmed-grid + per-algorithm illustration band in each FX card)
  was removed at the user's request — the cards now show header / algorithm
  picker / the 3x2 knob grid only. Source/ui/FxSlotVisualizer.{h,cpp}
  deleted; the knob grid owns the card body and centres in the freed height.
  FxWorkspace's FX top-row natural height was reclaimed 264 -> 240 (the
  band's 24px: nudge 4 + band up-to-30 + gap), which flows to the workspace
  rows below; the fixed 2 x 70px knob grid keeps full-size dials (44pt hit
  areas unchanged). Bypass alpha, theme re-tint and the card's test seams
  no longer reference the component; tools/editor_test [13]'s
  has-an-FxSlotVisualizer assertions removed with it.

### Fixed
- **Keyboard focus traversal + musical typing (2026-08-22).** An earlier
  musical-typing fix (kept QWERTY notes playing through knob/combo tweaks)
  had turned OFF `wantsKeyboardFocus` tree-wide, which emptied the editor's
  focus traversal — `KeyboardFocusTraverser().getAllComponents()` went from
  49 real focusables to 0, so Tab/focus navigation was dead app-wide (pinned
  by lifecycle_test [2] "focus traversal is non-empty"). Restored by
  forwarding unhandled plain keys from the editor down to the visible
  KeyboardView: controls keep focus (Tab works again) AND notes still play
  through control tweaks.
- **Arp-resolution labels (2026-08-22).** 8 of the 15 `arp_resolution`
  dropdown labels disagreed with the engine's tick table
  (`kMidiClockTickPerStep`, faithful to the Ambika firmware) — the factory
  default (6 ticks = straight 1/16) displayed "1/16T", and picking "1/16"
  played the triplet. The dropdown now uses the tick-aligned table (same
  values the LFO sync readout already used); `Arpeggiator` defaults match
  the factory part bytes (divider 10).
- **Mod-matrix index label, unified disable widget, header colour parity
  (2026-08-20).** (1) The row slot number showed "..." instead of "16":
  JUCE Label's default 5px-per-side border left the 18pt-wide index label an
  8px text box for the 13px text. Both matrices zero the border and allocate
  a measured 20pt. (2) ONE shared disable widget — `ParvatiModuleLamp`
  (ParvatiLookAndFeel.h) — now renders the synth mod-matrix bypass lamp, the
  FX mod-matrix lamp AND the FX slot power toggle: the FX toggle previously
  filled with accentSecondary (the style mismatch); all three now use
  accentPrimary on / textDisabled off, with the dot scaled by the band
  (44pt bands render a ~28-30pt dot — the "a bit bigger" request; the FX
  card header band ~15pt). (3) The FX card's "FX N" title now uses the SAME
  token as the synth GroupComponent titles (textPrimary; was textSecondary —
  the colour mismatch), re-resolved per paint. Tests: mod_matrix_ui_test [7]
  (index fits, no ellipsis) + [8] (shared-widget type check, per-theme lamp
  colour equality across synth/FX, FX header == theme textPrimary, dot
  diameter 28-30pt @ 44pt band).
- **Patch-table header alignment + tab regrouping (2026-08-20).** The
  column captions were painted from the header's full band while the row
  cells laid out from a 4px-inset band — every caption sat 4px left of its
  column. Both now consume the SAME inset band (kTableContentInset shared
  constant); a per-column x-equality test pins 0px drift on both tabs. The
  [Voice|MIDI] segmented toggle moved to the LEFT of the summary row
  (before the arrangement combo). Column split regrouped to the Ambika
  note-path semantics: **MIDI** = Part, Ch, Zone Lo/Hi, Octave (transpose
  acts on the note stream), Polyphony (the Mono/Poly/Unison/Cyclic/Chain
  note-to-voice allocator); **Voice** = Part, Voices, Porta, Lgo, Vol,
  Fine, Spr, Tune (sound-shaping; Porta/Legato straddle the line — they
  shape note transitions but live with the sound character). Hidden-tab
  cells remain live (automation + write seams on both tabs).

### Added
- **Live modulation feedback system (2026-08-21).** Pigments-style live
  indicators, GPU-disciplined end to end
  (docs/LIVE_MOD_FEEDBACK_DESIGN.md). (1) **Mod-pill history strips**: every
  CentralModBar pill (except the Const cluster + the bar-only NOTE sentinel)
  draws a subtle family-coloured sparkline of the RECENT values its modulation
  source produced (LFO waveform, envelope motion, aftertouch, velocity hits,
  ~1.57 s window at an ~81.7 Hz engine-side append rate) — bipolar sources
  (LFO/bend/note) centre on the strip midline, unipolar ones fill from the
  bottom. (2) **Envelope stage marker**: while a key is held, the ADSR
  EnvelopeDisplay previews draw a dot + hairline riding the curve through
  Attack/Decay/Sustain/Release from the engine's REAL envelope stage +
  progress. (3) **Live filter preview**: when cutoff/resonance is ACTIVELY
  modulated (env-2 sweep, LFO, matrix, wheel...), FilterResponseDisplay draws
  the live EFFECTIVE curve + cutoff tick over the opaque base preview (kept
  in place), with the base tick dimming while modulation is active.
  Architecture: ONE seqlock-guarded engine frame (audio thread, fixed-size
  bounded writes inside renderPartFx — no allocation, bit-identical audio)
  read ONCE per tick by the editor's LiveFeedbackHub; consumers repaint via
  per-pill bounded dirty RECTS gated by change signatures, so idle sources
  cost nothing and there is exactly one timer per seam (visibility-gated).
  **Visual Refresh** setting (Settings, 10/15/30/60 Hz, persisted as
  `ui_refresh_hz`, default 30) re-times the whole system. Patch loads /
  .MUL / .parvati loads / part switches / state restores reset the
  histories (epoch bump + wipe) so indicators never carry a previous patch
  or part across the seam. Engine-side suite:
  tests/ui_telemetry_test.cpp (parvati_ui_telemetry_test).
- **Patch-table Voice / MIDI tabs (2026-08-20).** A compact segmented
  toggle (GroupPager idiom) inline in the table's summary row splits the
  13-column row into two focused views: **Voice** (default — Part, Voices,
  zone, Oct/Porta/Lgo, Vol/Fine/Spr, Tune, Poly) and **MIDI** (Part, Ch,
  zone). The shared column geometry now distributes the FULL band width
  across the ACTIVE tab's columns (per-column min/flex/max specs; knob
  columns cap at 64pt so text columns drink the slack) — fixing the table
  occupying only ~60% of the page on wide editors (measured 1208/1248pt at
  the default 1280 window). Cells stay constructed on the hidden tab: every
  accessor/write seam and host automation works regardless of the active
  tab. A cumulative-rounding clamp keeps the last column inside the band at
  every width (overlap-test pinned).

### Changed
- **Test suite unification — one binary, fork-isolated (2026-08-22).** All
  ~115 standalone per-test executables were replaced by a single
  `parvati_unified_tests` binary (was ~3.5GB of per-test binaries + ~45 min
  builds; now one ~31MB binary, ~8 min). Tests register via `TEST(name)` and
  each runs in a `fork()`ed child that `_exit()`s — per-test memory is fully
  reclaimed between tests and a crash/OOM/leak cannot take down the suite
  (the first in-process design died to monotonic RSS growth under the GUI
  harnesses plus cross-test JUCE static pollution that produced ~94 false
  FAILs). Usage: `./build_unified/parvati_unified_tests [list|<names>...]`;
  `PARVATI_UNIFIED_INPROCESS=1` disables the fork for debugging. The 8
  `example_*` demo tests are now opt-in (`-DPARVATI_TEST_EXAMPLES=ON`;
  `example_failing_test` deliberately fails and was keeping the default
  suite exit non-zero). Harness follow-ups: FX live-repro held-chord health
  now scans its intended per-chord window (the old scan ran to buffer end
  and swallowed the next chord's release-gap silence — 0/6 false-fails;
  product exonerated), and the synth drag-probe Mix Balance row is red
  deliberately (hardware-faithful block-rate CV zipper, root-caused).
- **Sanitizer sweep + contributor docs truth-up (2026-08-22).**
  `tools/run_sanitizers.sh` had been silently broken by the unification — it
  globbed the removed per-test binaries, running 0 tests and exiting 0
  (false green). It now configures/builds the unified target per sanitizer
  config and drives it directly, validates requested test names against the
  runner registry (typos fail fast instead of "passing"), and accepts exact
  test names for quick subset iteration (`tools/run_sanitizers.sh
  envelope_test`). `CONTRIBUTING.md` build/test instructions updated to the
  unified flow (the per-target/per-binary commands were stale).
- **Synth-page header padding parity (2026-08-20).** The synth top row now
  takes the FX page's uniform 8pt gap on all four sides + between the
  OSC/MIX/FILTER columns (kRowGap hoisted to a class constant on both
  workspaces, pinned equal by the new workspace_padding test). Pages reflow
  against the full row height so the padding stays breathing room, not a
  height override.
- **FX matrix on the icon idiom (2026-08-20).** The FX mod-matrix rows now
  use the synth matrix's mute-lamp + delete-X buttons (44pt hit targets,
  accessible titles) instead of the old text "M"/"Clear" buttons; rows were
  already drag-source-free.
- **Concise tooltips (2026-08-20).** The long multi-sentence ParamHelp
  entries were trimmed to single sentences (ranges/units preserved; part
  scale/tuning/volume/spread rewritten, everything >200 chars cut to its
  first sentence).
- **Patch-table column headers + tooltips (2026-08-20).** The part table
  gained a single localized column-header strip (Part/Voices/Ch/Zone Low/Zone
  High/Oct/Porta/Lgo/Vol/Fine/Spr/Tune/Polyphony) painted from the SAME shared
  column-geometry source the rows lay out from (new `partColumnRects()` — one
  source of truth, so captions and cells can never drift); the per-row caption
  bands are gone (one header instead of six repetitions, rows now centre their
  controls on the full row height — 44pt HIG bands unchanged). Every
  interactive table cell now carries a tooltip: the part_* columns read their
  existing ParamHelp entries (volume/tuning/spread/octave/legato/portamento/
  raga/polyphony), the table-only controls (name, Voices, Ch, key-zone knobs)
  get inline localized help. The tooltips honour the editor-wide Settings
  toggle via `PatchPage::setTableTooltipsEnabled` (the ParamControl contract,
  mirrored); language switches re-translate the inline texts. Tests:
  editor_test [22] (header captions/order, per-cell tooltip completeness,
  gate blank/restore) + ui_mirror_test's name-label source moved from the
  dead caption-cluster layout detector to a row accessor
  (`displayedPartNamesForTest`).

### Changed
- **Patch-page simplification (2026-08-20).** The Patch page now owns the
  per-part settings outright, with exactly ONE editor surface per setting:
  - **New table columns** Oct / Porta / Lgo (octave, portamento, legato —
    PartData bytes 1 / 6 / 5) absorb three knobs from the old "Part / Play"
    panel: Oct and Lgo are compact combos (the 24pt-drawn/44pt-band HIG
    idiom), Porta a 44pt NoTextBox knob with a centre-arc %-readout copying
    the Zone-knob idiom. Writes use the established engine-direct pattern
    (setCurrentPart + applyPartByte + restore + postPartEdit + current-part
    APVTS re-sync), exactly like the Poly/Tune columns.
  - **Completing absorption (same day, follow-up):** Volume / Tuning /
    Spread joined the table as the Vol / Fine / Spr columns (bytes 0 / 2 / 3;
    byte 2 SIGNED int8) — the compact "Part / Play" row is GONE and the
    hosted page renders ONLY [Global options + arrangement + table], per
    the "ideally everything in the table except global" direction. Width
    was re-MEASURED (probe at the 1024x500 floor): editor 992 -> scrollbar
    8 -> hosted page 984 -> group panel 952 -> row 944. The briefed
    "~130pt slack" measured 108pt, so the two 8pt gaps before Tune/Poly
    were tightened to the 4pt idiom the Oct/Porta/Lgo trio already uses
    (no cell shrunk) to fund three 36pt cells; the measured row ends at
    940 <= 944 with the symmetric inset intact (row 944 <= the 948 target).
    The knobs are 36pt dials in 36x44 bands (L&F squares via jmin(w,h)):
    full 44pt-tall tap bands; three 44pt-wide cells (132pt + gaps) are
    arithmetically impossible at the floor. Readouts match the established
    formatters exactly (% of 127 / ±ct via x*100/128 / % of 40).
  - **Removed the duplicate knobs** part_raga ("Scale" — the table's Tune
    column already covers it) and part_polyphony ("Polyphony" — the Poly
    column). The APVTS PARAMETERS remain fully valid (created by the
    untouched descriptor table): host automation, saved states and files keep
    driving the bytes; only the page knobs are gone. With the completing
    absorption ALL EIGHT part knobs are table columns; the Global page is
    exactly the 3 global options (VCA curve / filter card / drive), down
    from 11.
  - **Mirror:** SynthEngine::applyPartByte's display-version bump (the 30 Hz
    pollPatchPageMirror seam) now covers bytes 0 / 1 / 2 / 3 / 5 / 6 in
    addition to 4 / 15, so host automation / NRPN / undo of every absorbed
    part param keeps the visible table honest.
  - New localized captions ("Oct" / "Porta" / "Lgo" / "Vol" / "Fine" /
    "Spr" + "On"/"Off" FR+DE; the dead "Part / Play" strings removed), new
    PatchPage test hooks (getDisplayed/choose for all six columns),
    editor_test [21] placement + byte-write coverage (incl. the SIGNED
    bytes), ui_mirror_test battery extensions (APVTS + engine-direct
    writes, both seams), tools/editor_test coverage-count + Global-page
    identity update (6 -> 3).

### Fixed
- **UI polish wave 2 (2026-08-20): mod-matrix interactions, seq combo font,
  module-header contrast, top-bar chrome.** Four user-reported items:
  (1) **Mod matrix rows are no longer drag sources** — modulators are dragged
  ONLY from the CentralModBar pills (the per-row six-dot grip is deleted from
  ModMatrixView AND, for consistency, the FX matrix's FxSourceDragGrip from
  FxMatrixView; rows remain drop-targets/assignable). "Clear" is replaced by a
  path-drawn delete **X** (IconButton::Close, new glyph + setGlyphInset) as the
  row's rightmost control; mute/bypass is now the FxSlotCard PowerToggle-style
  lamp widget, moved to the far LEFT, accent-lit while the routing is active
  (new `parvati_mod_matrix_ui_test`: no-drag sweep over every row child, X
  position/click-clears, lamp position/mute-restore, incl. the FX-matrix
  sweep). (2) **Seq dropdown font normalized** — the SEQ length picker's inline
  number was 17pt bold (largest text on the page) and every PopupMenu list
  read 15pt against 14pt combo text; both are now 14pt (bold retained for the
  value tier) — unified in the L&F so every dropdown matches (new
  `parvati_ui_typography_test`). (3) **Module headers ("FX", "Sequencer",
  "Osc 1"…) brightened** — GroupComponent titles moved from the dim
  textSecondary tier to textPrimary: contrast vs the card rises from
  4.6–5.8:1 to 12.9–15.8:1 across all six themes (≥7:1 target; light themes
  improve too). (4) **Top-bar polish** — the brand block now sizes to the
  version subtitle (the 10px "by 805Labs · v…" overhung the wordmark and
  nearly touched the patch indicator; gap ≥18px, pinned ≥10), header buttons
  are 36pt-tall on desktop (44pt iOS hit targets preserved, HIG asserts
  intact) with Save/Load trimmed to keep the 1024px minimum-width contract,
  and unselected header buttons + the patch indicator get a ~16% accent wash
  + textPrimary so they read as clickable (selected state unchanged, still
  visually stronger).
- **UI fixes (2026-08-20): preview updates, seq-length indicator, themed
  keyboard.** Three user-reported UI regressions:
  (1) **Oscillator/envelope/filter previews never updated** — the F-ios-perf-3
  30 Hz battery gate relied on `visibilityChanged()`, which JUCE only sends on
  the component's OWN `setVisible` call; combined with `addAndMakeVisible()`
  firing it BEFORE parenting (so `isShowing()==false`), the constructor-started
  poll timer was stopped once at construction and never restarted — previews
  froze on their constructor-built first render. Fix: all three displays now
  override BOTH `visibilityChanged()` and `parentHierarchyChanged()` (which JUCE
  DOES recurse through children — editor peer attach, page hosting swaps),
  funnelling into one `updatePollTimer()`. New [19] regression test in
  `editor_test` runs headless AND `--windowed` (real desktop peer): shape
  dropdown change, osc param, env attack, and filter cutoff each drive a
  preview generation bump (fail-frozen pre-fix). Test hooks:
  `previewGeneration()`/`isPollRunningForTest()` + `ParamPage::getGroup*ForTest`.
  (2) **Seq-length number invisible** — the tap-hit button (solid
  `backgroundPanel` fill) was created AFTER the label and `resized()` bounds it
  over the full cell, occluding the number (the Carbon seam is nearly
  invisible). Three independent defences, each test-pinned: tap button added
  BEFORE the label, `numberLabel_->setAlwaysOnTop(true)` (click-through), and
  fully transparent button fills. The number also moved from the dim caption
  tier to `textPrimary` (the knob-readout token), re-resolved on theme change;
  per-theme contrast now 12.7-17.6:1 (WCAG AA ≥ 4.5). New
  `parvati_seq_stepper_test` (43 checks incl. live theme switch).
  (3) **Bone-colored keyboard** — `keyWhite` was warm ivory in every factory
  and sharps used `backgroundBase` (near-invisible on light themes). Token
  re-spec: `keyWhite` = theme-matched elevated surface (mid-slate on dark,
  neutral near-white on light), NEW `keyBlack` token (recessed / charcoal).
  KeyboardView resolves EVERYTHING through the theme (zero colour literals
  left in the .cpp), rounded modern key-fronts per the L&F card idiom, presses
  = accentPrimary, hover washes, C-labels auto-contrast. `keyboard_view_test`
  [8]: 30 new checks — no stock JUCE bone palette, resolver mirrors tokens per
  theme, natural/sharp contrast 2.2-16.2:1, live re-resolve on switch.
- **FX review waves 1-3 (2026-08-19): reverb decay / drive calibration /
  tail-table corrections + hardening.** A per-algorithm DSP review (specs in
  `audit/fx_review_20260819/`, lane reports in `audit/fx_fix_20260819/`)
  found three FV-1 reverb decay knobs that did nothing, distortion drive
  ranges 8x hotter than documented, and a tail table that mis-reported six
  FX families. All fixed with before/after measurements:
  - **Plate/Room/Spring decay (per-pass RT60 law).** The comb feedback used
    the per-SAMPLE law `10^(-3/(decay*fs))` applied once per PASS, so the
    gain exceeded the 0.999 clamp for essentially every setting — the Decay
    knob was INERT above ~3% travel (Plate/Room) or at every setting (Spring,
    capped) and real LF t60 ran 5-17 minutes (Plate: measured >=20 s in a 20 s
    render; review computed 308-1027 s; Spring: 18.94 s identical at decay
    0.0/0.5/1.0 — knob provably dead). Now per-comb/per-spring
    `g = 10^(-3*D/(decay*fs))`: t60 == Decay by construction. Measured:
    Plate 4.0 -> 3.58 s, 2.05 -> 1.84 s; Room 3.0 -> 2.69 s, 1.55 -> 1.39 s;
    Spring 4.0/chirp0 -> 3.61 s, 0.2 -> 0.201 s (knob live; chirp still
    shortens the tail by design). ~10% systematic undershoot = in-loop
    damping-LP residual loss. New `parvati_reverb_decay_test` (21 checks:
    EDC t60 vs knob x3 effects, chirp trade-off, mono pin).
  - **Spring Width=0 is now TRUE mono.** Old `outL=a+w*b/2` left two
    decorrelated springs at w=0; R is now a bit-exact copy of spring A at
    width 0 (mirrors Room). Pinned in the new decay test (fails pre-fix).
  - **Distortion drive calibration (`>>13` -> `>>16`).** Both wavetable
    distortions read the table at 8x the intended input: "Drive 1..16x" was
    effectively 8..128x and Drive=1 small-signal gain measured 7.78 (should
    be unity). Now the true domain `xT = D*x`: Drive 1 gain 1.04 (Overdrive,
    was 7.78), 1.03 (LUT Soft, was 9.0); ranges are genuinely 1..16x/1..8x.
    Bias now the real +-0.3 domain value (was +-0.60). Excursion-split
    invariant pinned (drive/amplitude trade to identical output). New
    `parvati_drive_calib_test` (20 checks).
  - **LUT Distortion mono ceiling.** The stereo sum saturated BEFORE halving,
    capping mono L==R input at 0.5 (-3.5 dB headroom + an unintended clip
    knee that `fv1_newfamily_test` had rationalized as a "morph artifact").
    Halving pre-add: mono ceiling now the true 0.78; the shape-switch
    discontinuity metric dropped 0.0423 -> 0.0040 (10x).
  - **Level knobs 0..2 are real.** Overdrive + Compressor `q14(p*2)` clamped
    at 1.0, dead-upper-half; integer/fractional split now spans the range
    (OD rms 0.107/0.215/0.322/0.429 at Level 0.5/1/1.5/2 — upper half was
    flat).
  - **DC blockers on distortion outputs.** Cheby2 (-0.71), OctUp (-0.34),
    Asym and full Bias emitted SUSTAINED DC on the wet bus; a ~10 Hz one-pole
    HP on both distortion outputs zeroes all measured silence DC (<1e-4).
  - **Distortion latency reported.** The 6x-OS pair returned latency()==0
    despite 8 internal samples of SRC group delay -> comb filtering at dry/wet
    blend (first notch ~2 kHz @50%). Now reports 12 host samples @48k; series
    chain budget 12+12+8=32 = kChainDelayCap exactly.
  - **Echo tap glide.** Stepped Time/Spread jumped the read pointer (clicks).
    Both taps now Q.16-glide exactly like the Clocked Delay (cap 0.25
    sample/internal-sample); a circulating echo attenuates during the slew —
    authentic tape retarget. Pinned via single-probe impulse tap profiles
    (30 ms -> 57.6 mid-glide -> 90 ms settled).
  - **Mod-delay depth clamps.** Chorus/Flanger/Ensemble Depth could exceed the
    minimum delay, pinning 19%/49%/46% of every LFO sweep at the 1-sample
    read floor (the flanger jet collapsed at Manual 0/Depth 1). Depth now
    caps at center-1 (base-1): floor dwell 0.2-3.7%, documented ranges intact.
  - **CVerb Tone=0 no longer mutes the reverb.** `klp=0` froze the one-pole
    state -> wet == 0 at the knob minimum; Tone now maps to [0.05..1] (0 is a
    genuinely dark 5% leak).
  - **RingModulator output clamp.** The diode sum grows ~x/9 unbounded — hot
    input at full amount measured ~16x overshoot (the Wavefinder-crash
    class). Input-domain clamp `jlimit(+-1)` pre-diode restores the upstream
    ADC-bounded contract: <=full-scale is bit-identical, the hot path peaks
    2.975. Regression-pinned in `parvati_clouds_fx_test`.
  - **Chain master-EQ stability clamp.** The 5 kHz shelf's RBJ coefficients
    leave the unit circle below a 10 kHz host rate (mid below ~2 kHz) —
    runaway risk at exotic rates. All three band centers clamp to
    `<= 0.45*rate` (mirrors the RateBridge guard); no-op at sane rates.
  - **Tail-table corrections (`tailSecondsForFx`)**: CVerb loop length
    8483 -> 15353 (cross-coupled tank; max-time 35.7 -> 64.6 s); Echo ping-pong
    loop `T*(2+p3)` (mid-fb 4.65 -> 9.30 s, min 10 -> 20 ms); Ensemble 0 ->
    1.64 s, Chorus 0 -> 0.25 s, Flanger 0 -> 0.50 s (feedback-decay law from
    each effect's own loop); Resonator 0 -> decay-law (0.36 s @d.3, 5.75 s
    @d.6, 12 s cap @d1). Zero-tail family pruned; `render_quality_test`
    re-pinned for every changed case.
  Deliberately NOT done: sub-32 kHz HostRateBridge AA (rejected as
  shit-tier-audio territory), Spectral's 64 ms unreported STFT latency (needs
  a product decision — the chain delay cap cannot hold it), ClockedDelay
  retarget slew duration (1-3.75 s capped glide; crossfade rework deferred),
  PitchShifter block-size-dependent glide + spread-1-folds-to-mono, WSOLA
  correlator CPU (~0.1 core). All documented in the review reports.
  Gate note: `parvati_fx_crackle_diag_test` (SIGBUS, previously ~2/5) was a
  STALE Aug-10 binary whose CMake target no longer exists — deleted, not
  fixed; the real suite is 118/118.

- **Oversized-block slice-0 MIDI drain (2026-08-19, test wave).** The chunked
  oversized-block render handed slice 0 the FULL host MidiBuffer, and
  `juce::Synthesiser::processNextBlock`'s closing `std::for_each` drains every
  remaining event of the handed buffer beyond `numSamples`
  (`juce_Synthesiser.cpp:232-235`) — so out-of-window MIDI events fired early
  at slice 0's start AND re-fired in their home slice (double-fire): audible
  timing corruption in offline/freeze renders whose blocks exceed the prepared
  size (a note-on at sample 600 in a 1024-block sounded at ~266). Every slice
  of a tiled render now receives ONLY its own window's events, rebased to
  `[0,n)`; a single-slice (in-budget) block keeps the byte-identical fast path.
  Found by the new `render_quality_test` [2b] checks (onset now 610 for @600,
  778 for a @768 boundary), which previously pinned the bug as KNOWN-BUG.

### Changed
- **Test wave: 17 new test targets + extensions across DSP core / FX /
  engine / UI (2026-08-19).** A coverage-gap audit (specs in `audit/gap_*.md`,
  lane reports in `audit/tests_*.md`) mapped every source area against the
  existing ~105-binary suite; the previously-untested DSP core now has direct
  unit tests, and the FX/render path, engine glue, and UI helpers gained the
  missing pins. Full suite: 116/117 green (the one failure,
  `parvati_fx_crackle_diag_test`, SIGBUSes ~2/5 runs on clean HEAD —
  diagnostic-only binary, documented, not fixed). New targets:
  `parvati_fixed_math_test` (fixed-point helpers + Random LFSR; corrected the
  audit's U8AddClip expectation — the wrap happens before the compare),
  `parvati_transient_generator_test` (255-sample decay, CLICK/GLITCH/POP,
  shape clamps), `parvati_envelope_test` (stage chain, sustain target `<<1`,
  DEAD trigger), `parvati_sub_oscillator_test` (shape half-increment, triangle
  fold, amount-0 attenuation), `parvati_osc_sync_test` (UPDATE_PHASE reset
  order, sync carry, plus a byte-exact voice-level OP_SYNC end-to-end render),
  `parvati_voice_pitch_test` (kHighestNote clamp, bend offset, octave stride),
  `parvati_note_stack_test` (sorted/played orderings, saturation eviction,
  re-press dedup — the class behind a past hosted SIGBUS),
  `parvati_transport_clock_test` (samplesPerTick, drift-free carry, BPM
  clamps), `parvati_os_reaper_test` (deterministic retired-oversampling
  reaper: cap-2 parking, inline-delete fallback, no growth),
  `parvati_paramhelp_parity_test` (all 262 paramIDs have help),
  `parvati_translations_test` (FR/DE key parity + fallback),
  `parvati_note_step_control_test` (slider<->byte codec incl. the gate-off
  data-loss pin), plus extensions: render-quality (tail table freeze/multi-part
  MAX/tempo-move recompute, DC-blocker slice continuity + ~33 dB DC
  attenuation, MIDI rebase), fx-param-coverage (chain latency invariants
  N1/N2), fx-routing (FV-1 parallel topologies, mid-stream bypass gain, staged
  swap across rate changes, chain setTempo), clouds-fx (<=32 kHz bridge
  branch), midi-param (CC96/97 nudges + signed-range clamps), and formatter
  families (seqnote_vel legato bit, part_*/mix_crush, env2/env3 dispatch,
  valueFromString failure paths). Two small test-enabling surface changes:
  `AmbikaVoice::debugRetiredOsCount()` (const, side-effect-free accessor) and
  NoteStepControl's `sliderToByte`/`byteToSlider` statics made public. 78
  missing FX help strings added to ParamHelp (slot/fxmod families loop-generated
  to mirror the descriptor table); header contract updated to 198 curated +
  64 generated = 262 ids.

- **QoL wave: custom-scale removal, host parameter integration, offline
  max-quality render (2026-08-19).** A coordinated multi-lane quality pass
  from the JUCE-framework QoL audit (`audit/out_{tuning,render,host,ui}.md`).
  **(1) Custom Scala tuning removed.** The custom 12-entry tuning-table
  subsystem proved hard to get right against the hardware-parity contract
  (±1-semitone clamp, 1/128-semitone quantization, octave-repeating only);
  Parvati now ships the Ambika factory raga presets exclusively (Tune combo
  ids 1..33). Deleted: `ScalaImport.{h,cpp}`, the TuningEditor popover, the
  per-part custom-tuning engine state, the .MUL-export D14 custom-tuning
  warning, and the .scl/.kbm iOS document types + open-in parking (3 doc
  types / 3 UTIs remain; CMake PlistBuddy verify updated). Backcompat: engine
  blob bumped v7→v8 (v7 tuning blocks are parsed-and-ignored, size-checked;
  a v7 custom mode-33 restores as 12-EDO — its raga byte was 0 by the
  custom-active invariant); legacy `.parvati` `tuning_mode: 33` loads 12-EDO,
  `tuning_mode: 1..32` maps to the raga byte; `.PRO`/`.MUL`/host state are
  byte-identical (raga rides part byte 4). **(2) Host parameter
  value-to-text + groups.** Every `AudioParameterInt` now carries
  string↔value functions backed by the existing pure formatters
  (`SynthParamLabels::paramValueTextSynth`, `FxSlotLabels`), so host
  automation lanes show "+50ct"/"2.1s"/"440Hz"/"1/16" instead of raw
  0..127 bytes, and typed entry (Cubase/Bitwig) parses back; the EQ/drywet/
  amount label helpers were hoisted gui-free so UI knobs and host text share
  one implementation. Parameters are grouped into 13
  `AudioProcessorParameterGroup`s (Osc/Mix/Filter/Env/LFO/Mod/Modif/Part/Seq/
  Arp/Global/FX/FXMod) → VST3 Units / grouped AU lists; within-group order
  preserved exactly (flat automation indices permute only inside two spans,
  and every shipped wrapper addresses params by string/hash id). **(3) Host
  parameter context menus.** `ParamControl`'s right-click / touch long-press
  menu now resolves the editor's `getHostContext()` and takes the host's
  parameter menu (automation lane shortcuts, Cubase/Reaper VST3) as the BASE,
  appending Reset/Randomize below a separator; null-context hosts (AU/AUv3/
  standalone) fall through to the unchanged local menu. **(4) Preset stepping
  + shortcuts.** `PresetBrowser::selectNext/Prev` step the cached tree in
  exact menu order with wrap; `[`/`]` (plain or Cmd/Ctrl) step presets,
  Cmd/Ctrl+O opens Load, Cmd/Ctrl+S saves .parvati, Cmd/Ctrl+1..6 switch
  Parts (plain `[`/`]` verified unclaimed by the keyboard view/combos;
  picker shortcuts desktop-gated; tooltips advertise them, FR/DE included).
  **(5) Offline auto-max filter oversampling (desktop).** Entering
  non-realtime render (bounce/freeze/export) bumps the per-voice filter OS
  to 8x via the staged-install path (message-thread pre-build, audio-thread
  pointer swap — no AT allocation, click-free); leaving offline restores the
  user's factor, a re-prepare leak guard covers hosts that never call
  `setNonRealtime(false)`, and a mid-bounce user change re-targets the
  restore point. The boost NEVER persists (applied via `applyOversamplingFactor`,
  which skips `setUiOversampling`) — host state and the Settings combo keep
  the user's choice; latency re-reports through the existing dirty-flag seam.
  iOS excluded by measurement (8x = 2.3–3.7x realtime on A12-class cores).
  **(6) Chunked oversized-block rendering.** A host block larger than the
  prepared size (buffer transitions, offline/freeze, some AU/AUv3 hosts) was
  previously clamped, silently zeroing the tail of every oversized block.
  `processBlock` now tiles prepared-size slices, each running the full
  render pipeline (engine buffers always addressed from 0) and mixing into
  the host buffer at [done, done+n); MIDI is handed to each slice as its
  window's events rebased to [0, n) (slice 0 byte-identical to the old
  path; transport clock still advances on the full count). **(7)
  `processBlockBypassed` overridden** to clear all output buses (silence is
  correct for a bypassed synth) — the inherited default jasserts in debug
  builds when `getLatencySamples() > 0`, which filter-OS always reports.
  Voices/FX keep running internally so an un-bypass resumes where it left
  off. **(8) Dynamic `getTailLengthSeconds`.** Was hard-coded 0.0 — hosts
  (Logic/Cubase) truncate offline bounces at the reported tail, cutting
  reverb and delay ends off exports. The engine now maintains a tail cache:
  max over every enabled FX slot of a pure per-effect t60 estimate —
  reverbs by their decay/predelay mappings, DELAYS by the feedback-decay law
  `t60 = T · ln(10⁻³)/ln(g)` (FV-1 Echo, tempo-synced ClockedDelay at the
  current BPM, the CVerb tank), granular freeze at the 12 s cap — clamped to
  [0.2 s, 12 s] and recomputed on FX-param or tempo changes. **(9)
  Accessibility names for custom controls:** IconButton titles (Undo/Redo/
  Settings, localized), ModPill press-action handlers, wheel slider titles
  (Pitch/Mod, localized), ModMatrix/FXMatrix row group roles, FX slot card +
  power-toggle + type-combo titles; GroupPager verified already accessible
  via TabBarButton's built-in handler. New tests: `parvati_host_param_text_test`
  (group structure/order + ~25 text/parse round-trips) and
  `parvati_render_quality_test` (offline-boost no-persist/idempotency/leak-
  guard, chunked-render quarters non-silent, tail table + processor cache);
  `host_state_test`/`voice_slots_test` blob fixtures are now version-discovered
  (v8-aware stride math — the old v7 hard-code was a heap OOB read on fresh
  captures); tuning/preset/ios-openin/loader/shadow/golden/editor suites
  updated for the raga-only world. Full suite: 112/112 green
  (`parvati_fx_crackle_diag_test` SIGBUS pre-existing on HEAD, documented);
  the stale-`gen_templates`-binary template mutation seen mid-wave was
  chased to a mid-lane build artifact — the rebuilt generator reproduces the
  committed templates byte-identically (self-verify 5/5).

### Changed
- **AUv3 follow-up wave: no-host-tempo arp clock + fixed-size undo history
  (2026-08-19).** Two robustness items from the AUv3 compatibility analysis.
  (1) Hosts that expose no musical context to the plugin (GarageBand-class
  AUv3 hosts; the Standalone app has no transport at all) used to run the
  arpeggiator clock at a hard-coded 120 BPM. `processBlock` now resolves the
  clock tempo as HOST bpm when the playhead carries one, else a persisted
  MANUAL bpm (new `manual_bpm` state property, default 120 = the old
  behaviour; setter + restore clamp 40..300). New Settings ▸ Arp Clock row:
  a manual-BPM slider + a live status line showing which source is driving
  the clock ("Host tempo: N BPM (manual ignored)" / "No host tempo - manual
  tempo active"), and the editor posts a one-time transient hint on the
  Host→Manual transition. FR/DE translations included. Device item: whether
  GarageBand feeds AUv3 musical context is D8 in `audit/ios_device_checklist.md`.
  Review-wave hardening (fresh-context subagent review, all findings applied):
  degenerate host tempi (0/negative/NaN) are rejected at the resolution gate
  (treated as no-musical-context → manual fallback; NaN previously reached a
  `static_cast<uint16_t>` in the tempo-synced voice-LFO path — formally UB);
  the Arp Clock block sits ABOVE Filter Quality/Language in the Settings
  drawer so the R3 bottom-first degradation keeps it reachable in exactly the
  short AUv3 panes it targets (460pt drawer keeps it visible; the zoom slider
  row moved to 44pt for thumb/touch parity with the BPM row); the
  legacy-default and restore-clamp state paths are now pinned by REAL state
  surgery (strip `manual_bpm` from a saved state / doctor 9999 and 0), plus a
  Settings-panel height sweep asserting the Arp Clock row's visibility
  thresholds and no visible-child overlap/escape at any height (41 checks).
  iOS-simulator toolchain build of Standalone + AUv3 + the new test target:
  BUILD SUCCEEDED (appex `com.805labs.parvati.AUv3`, aumu/805L confirmed).
  (2) The APVTS UndoManager is constructed with an explicit FIXED-SIZE bound
  (`kUndoMaxUnits` 16000 ≈ ~130 undo steps, `kUndoMinTransactions` floor 16;
  a ValueTree property transaction ≈ sizeof(itself) ≈ ~120 units and APVTS
  routes every parameter write through the manager), so a long editing
  session cannot grow undo memory without bound — chosen deliberately over
  runtime memory-pressure machinery (an AUv3 extension hosts several
  instances in one process sharing one memory budget; a bounded history is
  the whole policy with zero moving parts). New regression suite
  `tests/undo_clock_test.cpp` (fixed-bound behaviour + clock fallback,
  state round-trip, clamping; `parvati_undo_clock_test`).
- **iOS quality wave (2026-08-19).** A five-lane iOS/iPadOS hunt (build +
  plists, AUv3 lifecycle, touch/HIG, Files integration, perf/thermal — full
  lane reports in `audit/ios_hunt_20260819/`, per-fix reports alongside)
  triaged ~25 findings; the high-severity set is being fixed in coordinated
  waves this session. Headline fixes landing in this wave (details + tests in
  each fix report): declared `CFBundleDocumentTypes` + five exported UTIs so
  .parvati/.PRO/.MUL/.scl/.kbm can be opened from Files/AirDrop/Mail and
  `LSSupportsOpeningDocumentsInPlace` stops being inert (CMakeLists graft via
  `ios/parvati_filetypes.plist`, configure-time FATAL_ERROR verify); the
  `std::system` probe in `tests/parvati_tests.cpp` is now iOS-guarded so the
  deterministic suite compiles under the iOS toolchain; dead
  `ENABLE_BITCODE` attrs removed and the correct Xcode `--config Release`
  invocation documented (the `CONFIGURATION=` form silently builds Debug and
  poisons the artefacts tree). A new on-device checklist
  (`audit/ios_device_checklist.md`) collects every UNKNOWN-NEEDS-DEVICE item
  (AUv3 instantiation in AUM/GB, interruption/route changes, save-from-host
  visibility, open-in handling — including the known follow-up that the app
  must still LOAD the opened file at launch via the JUCE Standalone delegate
  seam —, hardware-keyboard host-return, save-picker overwrite prompts,
  thermal at 2x OS).
- **iOS open-in loop + launch-sync follow-ups (2026-08-19, second wave).**
  (1) The open-in loop is COMPLETE: document types were declared earlier, but
  JUCE 9's iOS glue has NO `application:openURL:options:` anywhere — "Open in
  Parvati" launched the app and silently dropped the file. New
  `Source/ui/IosOpenIn.{h,mm}`: a pure headless-tested routing core (presets
  atomically import into the shared USER tree, .scl/.kbm park in
  Parvati/Tuning for interactive import) + an Obj-C++ shim that resolves
  JUCE's file-private `JuceAppStartupDelegate` by runtime name and adds the
  absent selector via `class_addMethod` (clean category-style addition, no
  swizzle; a swizzle branch future-proofs), bracketing security-scoped URL
  access for open-in-place. Routed presets load through the same main-thread
  seams the FileChooser completions use (`tests/ios_openin_test`). The first
  real iOS-toolchain build of the processor caught three compile bugs that
  desktop-only validation had masked (a `#if JUCE_IOS` block placed before
  the first JUCE header — undefined macro, silently compiled out on iOS too;
  JUCE 9's `wrapperType` is a public member, not a getter; app-level Obj-C++
  needs explicit `#import <UIKit/UIKit.h>` — JUCE's iOS headers are
  module-internal). Full Standalone + AUv3 .appex simulator build green.
  (2) F-ios-lc-4 remediated with the option-(a) mirror: presets saved inside
  AUv3 hosts land in the shared USER tree but the per-save Documents mirror
  writes into the extension's PRIVATE container (invisible to Files).
  `PresetBrowser::syncTreeNewestWins` — an additive, newest-wins, atomic,
  idempotent launch sync that never deletes (Files-app user edits/ deletions
  are respected; `*_temp` in-flight writes skipped; source mtimes propagated
  for exact compares) — publishes the whole tree into the containing app's
  Documents at every Standalone launch (`ios_file_flow_test` [5]). Device
  verification steps in the checklist's D3b. (3) The thermal hint (F-ios-perf-2
  rider) is surfaced in the editor's status line via a pure escalation-matrix
  seam, mutation-verified in `lifecycle_test` [5].
- **PresetBrowser menu cache (Wave 10).** The preset menu's directory scan
  and the per-factory-`.PRO` name parse ran synchronously on the message
  thread on EVERY open (all 4 banks x 128 programs parsed each time) —
  visible open-jank on a large USER library and constant churn against the
  factory tree. The scanned tree + parsed labels are now cached: a rebuild
  costs only PopupMenu construction from cached data. The cache is dropped
  by the new `PresetBrowser::invalidate()` (wired into every successful
  editor save: .PRO / .parvati / .MUL) and self-heals when any
  previously-seen directory's mtime changed (external adds/removes/renames
  via the Files app — one `stat` per known directory, no filesystem
  watcher). Headless cache-contract test added ([17] in the editor tests:
  scan/parse counts + external-change pickup).
- **FX engagement defaults seed only on UI picks (Wave 10).** Host
  automation / NRPN writes of `fx{N}_type` used to fire the same
  `parameterChanged` path as a combo pick, clobbering the current
  enabled/drywet/param1..5 with the incoming type's engagement defaults on
  every automation step. The seeding moved to an explicit seam
  (`FxSlotCard::seedEngagementDefaultsForType`), called by the type-combo
  popup item action and the prev/next chevrons BEFORE the param write; the
  listener now only reflects. The W7 undo guard is subsumed (nothing seeds
  in the listener, so undo replay can never seed — still pinned by the
  editor test, now alongside the automation no-clobber pin and the seam
  seeding pin).
- **Part-combo 30 Hz relabel churn (Wave 10, verified).** The display-string
  cache from the earlier fix (`partComboLabelCache_`) already reduced the
  30 Hz poll to a 6-string compare with change-only `changeItemText` —
  verified against the current tree; no further change needed.

### Added
- **Adaptive header for AUv3 compact panes (Wave 9).** AUv3 hosts
  force-resize the editor to the pane (the 1024pt `setResizeLimits` floor is
  desktop-only advice), and the header's fixed budgets drove controls to
  0px width in narrow panes (invisible AND untouchable — AUM keyboard-open
  ~570pt, GarageBand smart-controls ~700pt). Secondary header controls now
  fold into the existing "..." overflow popup at MEASURED breakpoints
  (< 1024pt: Part combo + [Synth]/[FX]; < 810pt: [MOD]/[MAP]/gear;
  < 650pt: Redo + the [Patch] page button), each popup item driving the SAME
  seam as the hidden control (showTopPage / triggerClick / the real combo's
  attachment). The preset browser's width is elastic below the floor. Primary
  controls (preset, Load, Save, Undo, [KBD], "...") never fold.
  tests/layout_minwidth_test sweeps 560..1800pt: every VISIBLE interactive
  header child keeps positive extent, the primary set stays visible down to
  560pt, no sibling overlaps; the >= 1024pt designed-width gates are
  unchanged (the layout above the floor is byte-identical).
- **iOS safe-area trim now edge-aware.** The editor trims only the display
  edges it actually SPANS (screen bounds vs the display, 4pt tolerance); a
  centred AUv3 pane keeps its full width instead of losing ~47-59pt/side to
  insets it never touches. Full-screen Standalone behaviour is unchanged;
  headless (no peer / no display) skips the trim.
- **Four firmware-parity convergences (Wave 8) — each locked as an equality
  check in the differential harness** (tests/firmware_parity_test; the
  known-divergences allowlist drops from 5 to 1, the remaining one being the
  deliberate velocity-0 UX substitution):
  - **Multicast note routing.** `Multi::NoteOn/NoteOff` deliver a note to
    EVERY part whose channel+zone accepts it (firmware multi.h:120-138);
    Parvati routed first-match only, so a layered Omni + per-channel setup
    played one part where hardware plays both. `forEachAcceptingPart`
    (header-inline template over the routing predicates) now drives note-on,
    note-off AND the arp held-key routing; release pairing is symmetric by
    construction (the same predicate that routed the on routes the off).
  - **Wrap-around key zones.** `keyrange_low > keyrange_high` is the firmware
    complement zone (accepts `<= hi OR >= lo` — the classic hardware split
    trick); Parvati only had contiguous zones, silently rejecting the wrap
    half of the keyboard. Ported in `partAcceptsNote`; the .parvati loader
    now PRESERVES inverted zones (it previously jmin/jmax-swapped them into
    the wrong contiguous range), still clamping both ends to 0..127, and
    the load-invariants contract allows lo>hi.
  - **Arp/seq phrase restart while the transport is stopped.** Firmware
    `Multi::NoteOn` calls `Start()` when `!running_` — a new phrase (the
    held-key stack was empty) restarts the arp/sequencer at pattern step 0;
    Parvati resumed mid-pattern. Ported on the audio thread in
    processTransport's arp routing (Start before the stack push, exactly
    the firmware order), gated on `!isPlaying` (a stopped DAW transport) —
    which also required a mock AudioPlayHead in the parity oracle (the
    processor defaults isPlaying to true headlessly).
  - **Polyphonic aftertouch.** Firmware `Part::Aftertouch(note, vel)` writes
    MOD_SRC_AFTERTOUCH to the note's voice per polyphony mode (POLY/
    CYCLIC/CHAIN: the voice playing the note; UNISON_2X: the pair; MONO:
    channel-wide). Parvati dropped poly-AT entirely. Ported as a
    `handleAftertouch` override routed through the multicast predicates.
  Harness notes: the phrase-restart observable is a generated-notes recorder
  (the arp's live previousNote_ resets on pattern-skipped steps while the
  firmware oracle reads a persistent log), and `lastModWrite` now takes the
  MAX over a part's voices (a last-writer-wins read clobbered the written
  voice with an idle one — pre-existing harness bug masked by the
  channel-wide CC family).
- **Sustain pedal (CC64) — firmware parity (Wave 7, round-3 lane-B finding
  1).** The engine's `noteOff` override had fully replaced juce's
  `Synthesiser::noteOff`, which was the only place the base class gated
  release on the sustain pedal — so CC64 was a functional no-op (key release
  killed the note regardless; pedal-up did nothing). Implemented the firmware
  `Part::NoteOff`/`ControlChange` semantics (`part.cc:335-390`) with per-part
  audio-thread-only state: pedal-down swallows note-offs (the note keeps
  sounding and is remembered, arp-held keys keep arpeggiating), pedal-up
  drains the remembered releases through the normal release path. Routed per
  channel like every other CC (Omni parts match everything).
- **All-Notes-Off (CC123) / All-Sound-Off (CC120) now clear per-part
  bookkeeping (Wave 7, lane-B finding 2).** juce dispatches these directly to
  `Synthesiser::allNotesOff`, which only stops channel-matched VOICES — the
  arp held-key stacks, sequencer notes and mono stacks survived, so an arp
  kept re-triggering from "held" keys after CC123. `allNotesOff` is now
  overridden per firmware `Multi::AllNotesOff`/`Part::AllNotesOff` (incl. the
  ignore-note-off gate: a held sustain pedal makes CC123 a no-op, exactly like
  the hardware).
- **CC1/CC2/CC4 (mod wheel / breath / foot) route per channel (Wave 7,
  lane-B finding 3).** `applyGlobalModSource` wrote the mod source to EVERY
  voice in the pool, so a mod wheel on channel 3 modulated all six parts of a
  multitimbral setup. Firmware routes the CC to the channel-matching parts'
  voicecards first — mirrored: only parts whose receive channel matches (Omni
  or exact) write the source. Single-channel setups are unchanged.
- **`tests/cc_routing_test.cpp` (W7):** sustain swallow/drain, sustain + arp
  held-key semantics, CC123 stack clearing (+ the held-pedal no-op gate) and
  CC1 channel scoping — all against a real processor/processBlock.

### Fixed
- **FX type undo no longer seeds engagement defaults over the restored values
  (Wave 7, lane-A finding 1).** JUCE replays an undo transaction's actions in
  reverse order, so the `fx{N}_type` write was restored LAST — firing
  `FxSlotCard`'s seeding listener DURING the replay and clobbering the
  just-restored parameters with the new type's defaults (undoing a
  Chorus→Flanger switch destroyed the user's Chorus settings). The seeding
  block now bails while `UndoManager::isPerformingUndoRedo()` (the
  `onPartSelect` idiom). Regression-tested in `tests/editor_test.cpp` [12].
- **Tap-to-assign ([MAP]) mode no longer leaks across editors (W7, lane-A
  finding 2).** Its state is process-global (statics + a registry spanning
  every live `ParamControl`): an editor closed with [MAP] ON left other
  plugin instances — or the next open of this one — in assign mode while
  their [MAP] button showed OFF. `~ParvatiEditor` now resets it (mirrors the
  zoom-reset teardown). The KeyboardView settings-report "primed" flag is
  per-instance too (a process-wide static was consumed by the first editor
  ever opened, so later instances flashed a spurious status transient at
  open).
- **Notes held on the on-screen/QWERTY keyboard at editor close no longer
  sustain forever (W7, lane-B finding 6).** `~ParvatiEditor` now calls the new
  `KeyboardView::releaseAllNotes()` (note-offs for every held mouse/touch/
  computer-key note) BEFORE nulling the callback.
- **`getPrimaryDisplay()` null-guarded in both dialog launchers (W7, lane-A
  finding 4).** TuningEditor / MulExportDialog dereferenced the primary
  display unconditionally — a null in headless/automation contexts crashed at
  dialog open. Both fall back to the un-capped height now.
- **Header Part-combo relabel no longer churns 6 `changeItemText` calls every
  30 Hz tick (W7, lane-A finding 6).** `refreshPartComboNames` caches the six
  display strings and only rewrites the items whose label actually changed
  (language switches still update — the placeholder text is part of the
  compared string). The selected-item re-apply from the earlier fix is kept.

### Changed
- **`deploy` re-signs the CLAP bundle after copying it (W7, lane-C finding
  4)** — the same `codesign --force --sign -` the VPT3 (moduleinfo seal) and
  Standalone copies already apply; the plain `cp` invalidated the built
  bundle's signature there too.
- **Stale docs/comments (W7):** the FxRoutingBar "Dry/Wet starves to 0×0 at
  1280×634" note (superseded by `kTopRowNaturalH` flooring — audit T9's
  matching discovery note marked resolved) and tools/editor_test.cpp's
  "single [SYNTH] tab" header (the selector has been [SYNTH][FX] for a
  while) corrected.

- **Last pre-existing red test root-caused and honestly recalibrated (Wave 6):
  `parvati_part_fx_routing_test` [3].** The check asserted the dry FX output
  equals the RAW mono sum of the six voicecard buffers sample-exact, but the
  engine's `renderPartFx` has (since the FX crackle audit) passed that sum
  through a documented chain-input safety knee — `8 * SoftLimit(s/8)` plus a
  ±16 hard ceiling, "transparent by design" (-0.04 dB at |s|=1) — so the
  check has been red since that commit landed without updating the test
  (verified pre-existing at HEAD under release and ASan builds; measured
  deviation exactly matches the knee: constant-gain fit 0.9996, corr 1.0).
  The test now replicates the knee from the vendored stmlib header (single
  source of truth) and asserts `fx == knee(sum)` near-sample-exact (1e-6;
  the residual is float-vs-double accumulation), plus a guard that the knee
  is actually exercised — so neither routing drift nor a future knee removal
  can pass silently.
- **Compiler-warning cleanup in the wave-touched files.** Fixed the known
  `-Wunused-parameter` on `ParvatiEditor::afterMultiSaved` (juce::ignoreUnused;
  the parameter is iOS-only), a `-Wsign-conversion` in `SynthEngine`
  state capture (`out.write` already takes size_t — dropped the redundant
  int cast), the two deprecated JUCE `Displays::userArea` reads
  (MulExportDialog, TuningEditor → `userBounds`), and the deprecated
  `startDragging` Image overload in WheelsComponent (`ScaledImage` swap,
  behavior-identical). Also silenced two cert-err33 snprintf-return warnings
  in the recalibrated test. Left alone (untouched files / house style):
  the vendored-style sign-conversions in FxProcessors, the intentional
  float-equality idempotence guards in analog_filter, the JUCE-side switch-
  enum/int-float notes in FxSlotLabels/FxSlotVisualizer, and the four
  remaining `startDragging` call sites in files these waves did not touch.
- **FR/DE translation coverage completed (Wave 4 polish).** 86 new keys per
  language block: the MulExportDialog strategy picker (heading, all 6
  strategies + descriptions, preview labels, the custom-tuning warning), the
  TuningEditor popover chrome, the two missing `.MUL` file-chooser titles, the
  `Ultra (8×)` oversampling item, the header zoom/page-toggle/status-strip
  strings (including the tap-to-assign transient hints and the CPU tooltip),
  the parameter context menu, the mod/FX matrix views, the FX routing bar +
  slot-card tooltips (restructured to suffix-fragment keys so the FX number
  stays outside the translated literal), the keyboard octave / sequencer
  stepper buttons, and the preset browser. Hardcoded-English tooltips on
  user-facing controls (status load, wheels, FX slot cards) are now TRANS'd.
  Verified with a standalone probe that every key resolves in both tables
  (embedded-`\n` and `\"` keys included; LocalisedStrings unescapes them).
  Docs refreshed: README/COVERAGE_SPEC now list the five current arrangement
  presets (Mono/Poly/Unison/Multitimbral/Drum Kit, 0-voice parts first-class),
  the UI plan's `MultiPage` reference became `PatchPage`, and stale 36px/1100px
  layout comments were corrected; a stray mid-function `#include <cstdio>` in
  SynthWorkspace was removed. Patch-page Zone Low/High caption columns widened
  48→56px so the FR/DE captions ("Zone (bas)"/"Zone (tief)") no longer squash.

### Fixed
- **Arp/sequencer note-routing + loaded-byte hardening, NRPN signed values,
  and a legato click (second same-class bug hunt, Wave 5).** Six findings,
  all fixed with regression coverage:
  (1) **Enabling the arp/sequencer while a note sounds no longer strands that
  note.** The note-off path handed EVERY release to `arp.noteOff` while the
  mode was active, but a note that was sounding BEFORE the enable never
  entered the arp's held-key stack — its release was swallowed and the direct
  voice sustained forever (firmware `Part::NoteOff` releases all allocated
  voices when the stack empties). `NoteStack::contains` + `Arpeggiator::
  holdsNote` now let a non-held release fall through to the direct MIDI path
  (arp_test: enable-arp-while-held regression).
  (2) **Raw PartData arp/seq bytes are clamped at the staging site**
  (`stageArpSeqFromPartBytes`): a hand-edited `.MUL` (or corrupt host-state
  blob) carrying `arpMode=5` made the part silently swallow every note
  (`isActive()` true, `isEnabled()` false), and `arpOctave=0` with direction
  Random never terminated the Random branch's octave-wrap loop — on the
  audio thread (host hang). Mode/direction/octave/pattern/resolution/seq
  lengths now clamp to the firmware parameter ranges, and the wrap loop
  itself gained the `octaveRange_ > 0` guard (the sibling `numNotes` guard's
  shape) (arp_test: clamp + no-hang).
  (3) **NRPN now honours two's-complement negatives on INT8 parameters.**
  `applyValue` clamped the raw byte against the signed APVTS range
  unsigned, so any byte >= 128 saturated to +max — an Ambika
  editor/librarian could not set a single negative detune or mod amount
  (osc range/detune, all 14 mod amounts). The byte is reinterpreted as int8
  before clamping, matching firmware `parameter.Clamp` (midi_param_test:
  NRPN(3, 0xC0) -> -64, positive control +64).
  (4) **Legato retriggers no longer click.** `startNote` cleared the
  resampler FIFO + reset the interpolator UNCONDITIONALLY — on the legato
  path that discards ~0.4-1.3 ms of unconsumed internal samples and restarts
  the Lagrange interpolator cold: a time-skip discontinuity at every MONO/
  legato step (the de-click ramp was already skipped there for exactly this
  reason). The FIFO clear is now gated on `!legato`; the hard-stop path
  still clears (legato_test: max sample slew at the retrigger < 0.25).
  (5) **Tuning-table class index confirmed firmware-faithful** (a hunt
  finding re-verified against the reference): firmware `Part::TuneNote`
  (part.cc:640) indexes the raga by the RAW `midi_note % 12` — exactly what
  Parvati does; the comment now cites the line so it is not "fixed" away
  from parity by a future pass. No behaviour change.
- **Coverage-metric correction: `mix_fuzz` check was provably unable to see
  the fuzz.** The waveshaper table (`wav_res_distortion`) is monotonic with
  f(128)==128, so every sample keeps its sign under the wet/dry mix —
  zero-crossing rate is invariant under a monotonic map (a saw and its
  near-squared version have the same 2 crossings/period). The check now
  compares RMS (fuzz 63 = ~98.8% wet squares the saw up; expected >1.3x) and
  prints both metrics; the DSP itself was traced byte-exact to firmware
  `voice.cc` and is untouched.
- **MulExportDialog is now fully translated (FR/DE).** All 24 of its TRANS
  literals (heading — split into two single-line fragments because the
  LocalisedStrings tables are line-parsed — strategy labels + descriptions,
  preview labels, the custom-tuning warning, Cancel/Export titles) exist in
  both blocks, verified symmetric by a parser-faithful script (105 rows
  each).
- **Visible-page staleness, atomic state restore, and file-integrity sweep
  (same-class bug hunt, Wave 3).** Five more findings, all fixed with
  regression coverage:
  (1) **The Patch page now tracks out-of-band engine writes while it is on
  screen.** Host automation of `part_polyphony`/`part_raga`, MIDI NRPN, host
  undo and state restores mutate the engine with no editor hook, so a
  VISIBLE Patch page could keep showing stale Poly/Tune/Voices/Ch/Zone rows
  until the next reveal or load. The engine now carries a monotonic display
  version bumped by its message-thread mutators of Patch-page-mirrored state
  (`SynthEngine::getDisplayVersion`); the editor's poll timer re-reads the
  page when the version moved while page 2 is shown (change-only, O(1)
  check; `ParvatiEditor::pollPatchPageMirror`).
  (2) **A truncated host-state blob no longer half-applies.**
  `SynthEngine::restoreState` mutated `parts_[p]` as it parsed and returned
  false mid-way on a corrupt/truncated blob, leaving a half-restored engine
  (early parts from the blob, the rest the previous session) that the
  caller's legacy APVTS fallback then layered on top of. Restore is now
  two-phase — parse the whole blob into local snapshots, commit only on full
  success — so a rejected blob leaves the engine exactly as it was (verified
  at 25/50/75%/last-10-bytes cut points).
  (3) **A failed ChainSplit multi save no longer leaves an inconsistent unit
  set.** `saveMultiFile` wrote the sibling `-2.MUL`/`-3.MUL` unit files
  BEFORE the primary file, so a mid-set disk failure left new-generation
  units beside a stale primary (a chained-Ambika set that matches neither).
  The primary is now written first (atomic TemporaryFile write) and any unit
  failure deletes every file the save already wrote — the whole set lands or
  none of it does.
  (4) **A `.MUL` truncated before the last part's objects is now rejected
  instead of loading as a hybrid.** The MBKS walker stops cleanly at a
  trailing cut, so such a file parsed `ok` and applied its NEW routing over
  the PREVIOUS multi's patch/part bytes for the missing parts. The loader now
  rejects any multi whose 6 parts do not all carry Patch + PartData (the
  firmware writer always emits all six).
  (5) **Integrity quick wins.** Hand-edited `.parvati` routing values are
  clamped to the engine's accepted ranges (channel 0..16, keyzone 0..127 with
  an inverted zone normalized by swap, legacy bitmask 0..0x3F) — out-of-range
  values previously uint8-wrapped into silently dead parts; the factory
  preset installer's bank gates are gone (the `.PRO` gate looked in the
  non-recursive parent — dead code — while the `.MUL` gate could permanently
  skip the rest of the bank after a partial first extraction; both banks now
  run the cheap per-file write-if-missing unconditionally and an interrupted
  first run self-heals); the ChainSplit preview no longer labels unit 1
  `"-.MUL"` (now a translated "this file"); preset browser menus sort
  naturally ("Patch 2" before "Patch 10"); and a host-state restore
  re-echoes `part_select` to the blob's saved current part so the header
  combo cannot disagree with the engine after a restore.
- **Shadow-state, async-lifetime, undo-integrity and failure-feedback sweep
  (same-class bug hunt, Wave 2).** Nine more findings from the parallel
  review, all fixed with regression coverage:
  (1) **Part aliases no longer survive whole-setup loads they no longer
  describe** — a `.MUL` load after the Drum Kit template kept labelling the
  new multi's parts "Kick"/"Snare". Multi loads now clear all six names
  (the `.parvati` multi format always re-applies its own); single-patch loads
  (`.PRO`/`.parvati` patch) deliberately KEEP the current part's alias — an
  alias is track metadata, not patch content.
  (2) **Degenerate `.parvati` multi documents are rejected instead of
  half-loading** — `parts: []` (or bare scalar entries) previously
  "loaded successfully" over the previous multi's leftover routing; the
  pre-parse now requires a non-empty parts array whose entries carry at
  least one recognized part key. A PRESENT entry missing `channel`/
  `keyzone_*` keys now falls back to the engine init defaults (channel =
  partIndex + 1, zone 0..127) instead of silently inheriting the previous
  multi's routing.
  (3) **`SettingsPanel` Filter-Quality combo rebuild uses
  `dontSendNotification`** — the last live instance of the async-`clear()`
  class (an uncommanded `setOversamplingFactor` on every language switch).
  (4) **SafePointer guards on every async popup/dialog callback** — the
  zoom-overflow menu, the Save-format menu, PresetBrowser leaf actions and
  the MulExport DoneCallback no longer capture raw `this` (the host can
  tear the editor down while a menu is open). The TuningEditor popover now
  owns a theme-copy LookAndFeel instead of borrowing the parent editor's
  (a freed-L&F paint after editor teardown) and closes itself once its
  launch parent is gone; the live-edit callback is SafePointer-guarded too.
  (5) **Undo can no longer corrupt a part switch.** `loadPartIntoApvts`
  display dumps are written non-undoably (a ~250-action dump no longer
  becomes one giant undo step), `onPartSelect` clears the history
  (JUCE's `replaceState` idiom), and the editor's undo/redo entry points
  (`undoSafe`/`redoSafe`) sweep stragglers JUCE's append-after-listeners
  ordering and the 10 Hz APVTS tree-flush can leave — previously one undo
  after a part switch silently overwrote the NEW part with the OLD part's
  sound. Undo history is now cleared at every part switch (undo never
  crosses a switch — the replay-misrouting risk makes that the only safe
  semantics).
  (6) **Save/load failures are no longer silent** — an unwritable save
  location or a corrupt load now raises a native warning alert (skipped
  headlessly), instead of pretending success (data loss) or doing nothing.
  (7) **`.parvati` top-level `name:` is escaped** — a `"` in a patch name
  truncated it on reload and an embedded newline SPLIT the line-based
  document (the `params:` block never parsed → a silent load failure).
  (8) **A corrupt `.parvati` PATCH no longer kills sounding voices** — the
  patch load path got the same validate-before-mutate guard the multi path
  has.
  (9) **Renaming the selected part relabels the header Part combo
  immediately** — `changeItemText` only updates the menu entry; the inline
  label kept the old text until the next part switch.
- **Stale `customTuningActive` lifecycle closed across every remaining load
  and edit path (parallel same-class bug hunt, Wave 1).** The earlier fix
  covered `.PRO`/`.MUL`; a fresh-context review found the same bug class in
  three more paths, all now fixed: (1) a `.parvati` **patch** load never
  cleared the flag, so loading a 12-EDO patch over a custom-tuned part kept
  playing the old microtonal table; (2) a `.parvati` **multi** load had the
  same hole for a subtler reason — the serializer only emits `tuning_mode`
  for non-zero modes, so every 12-EDO part loads with the key ABSENT and the
  loader's hasProperty-guarded clear never ran (the loader now applies the
  same file-is-truth rule when the key is missing and byte 4 is 0); (3) a
  LIVE `part_raga` = 0 write through the APVTS (hosted param-grid combo, host
  automation, NRPN 116) only wrote byte 4 while leaving the custom flag
  armed — the parameter path now clears it, matching the Patch page's Tune
  combo (D4-inverse: an explicit 12-EDO selection). The dormant-custom
  "resurface" behaviour on a live preset-clear is retired accordingly (the
  combo read "12-EDO" while the part played a microtonal table); the way back
  to a custom table is the TuningEditor / re-import. The bulk sync path
  deliberately still does NOT clear (byte 4 = 0 under an armed custom is valid
  state on a part switch), pinned by a new regression check.
- **TuningEditor "Custom…" live edits now re-sync the APVTS.** The popover
  wrote the part's PartData engine-direct (`setPartTuningCustom`: byte 4 = 0
  + flag armed) but its change notification only refreshed the row combo, so
  the hosted `part_raga` combo kept the stale preset label and — worse — an
  APVTS-based save exported the STALE preset byte while the engine played the
  custom table. The post-edit notification (`PatchPage::tuningEditorApplied`,
  now also a public headless test seam) re-syncs the current part.
- **`.parvati` multi loads now STAGE FX slot types into the DSP chains.** The
  loader wrote only the per-part `fxState.slotType` atomics; slot types reach
  the DSP exclusively via message-thread chain staging (`FxChain::setSlotType`,
  audit F1 — the audio thread's `fxDirty_` service deliberately never installs
  types), so a loaded multi's FX were silently absent on a fresh engine or
  kept playing the PREVIOUS effect on a painted one. New engine API
  `stagePartFxSlotType(part, slot, type)` (the explicit-part twin of
  `setFxSlotType`) stages every restored type for all 6 parts; a new
  installed-chain-type test hook (`fxChainSlotTypeForTest`) proves the DSP
  actually holds the loaded type, not just the atomic.
- **Host state restore re-applies the global option params to the engine.**
  `vca_curve` / `filter_card` / `filter_drive` live in the APVTS but not in
  the engine blob, and the blob-restore branch only ran `loadPartIntoApvts`
  (which skips `isOption` descriptors) — so a restored session rendered with
  the ENGINE defaults while the UI combos showed the saved values (typical
  hosts call `prepareToPlay` before `setStateInformation`, so the ctor/prepare
  sync never re-applied them afterwards). The restore now re-applies every
  non-`part_select` option from the restored APVTS, mirroring the legacy
  branch's coverage.

- **Patch-loading UI wiring (end-to-end review pass): every load path now
  refreshes the Patch page, and failed loads never mutate the engine.**
  Single-patch loads (.PRO / .parvati patch) previously skipped the Patch
  page refresh (only multis refreshed), so the current part's Poly / Tune
  combos went stale after a drop; the refresh now runs on EVERY successful
  load. The page also re-reads the engine each time it is REVEALED — a host
  state restore (setStateInformation) rewrites the engine with no editor
  notification, so the previously hidden page caught up only on the next
  file load. Corrupt .parvati multi files failed validation only AFTER
  `resetAllVoices()` + the init slot reset had already run, silently
  re-partitioning the pool on a failed load; validation now parses the
  document before any mutation. .MUL / .PRO loads never cleared a stale
  custom-tuning flag, so a loaded 12-EDO file stayed stuck showing "Custom…"
  (resolved mode 33) — the file is the whole tuning truth on those formats,
  so a zero raga byte now clears the flag (mirrors the .parvati path). The
  language-switch combo rebuilds no longer fire a stale async `onChange`
  (the default `ComboBox::clear()` notification re-ran the arrangement /
  channel / poly write paths with a pre-rebuild selection, silently
  overwriting engine state — now `dontSendNotification` like the voices /
  tune combos). Header left cluster min-width fit at the 1024px floor
  (preset dropdown 168 -> 156; [FX] kept ~38px, now its full 50px — pinned
  by a new editor test). FR/DE translation gaps closed ("Tune",
  "Custom…"; the Voices tooltip is TRANS-wrapped). New editor test section
  drives the REAL user entry points: `filesDropped` (multi + single .PRO +
  corrupt-file no-mutation) and the page-reveal refresh.
- **Note-onset crackle with the FV-1 distortion FX (Overdrive / Wavemangler)
  — shaper aliasing at the 32.768 kHz internal rate.** Symptom: with an FX
  slot active, EVERY key press started with a short crackle/fizz burst that
  subsided as the envelope settled (the attack peak drives the nonlinear
  table deepest). Root cause: a hard-nonlinear transfer curve generates
  harmonics far above the 16.384 kHz internal Nyquist, which FOLD back as
  INHARMONIC components — noise-like fizz locked to the waveform corners,
  not musical distortion. Measured with a new direct probe
  (`parvati_fv1_alias_probe`, EXCLUDE_FROM_ALL): at a 3 kHz input the worst
  folded spur sat only **16 dB below the fundamental** (1 kHz: 32 dB) —
  clearly audible; through the full engine the dense onset impulse train
  peaked 0.176-0.183 (Overdrive) / 0.11-0.14 (LUT Dist). Fix: the two
  hard-nonlinear FV-1 effects now run their TABLE stage inside a **6x
  oversampled domain** — the vendored Warps polyphase FIR
  (`SampleRateConverter<SRC_UP/DOWN,6,48>`) the Wavefolder/RingMod slots
  already use — with the Q.23 saturating fixed-point shaper evaluated per
  oversampled sample (the chip-contract DSP is unchanged; only the linear
  rate conversion around it is float, exactly like the Warps slots). Tone
  LP / Level / clock-jitter / the shape-crossfade clock stay at the 1x rate
  (linear stages and internal-sample-defined timings do not alias).
  Measured after: worst inharmonic spur **-45 dB at 3 kHz** (1 kHz: -55,
  220 Hz: -87); engine-level onset impulses 0.138 / 0.107 (the remaining
  wrap-locked steps are the driven table squaring the saw — the effect's
  sound); real-Standalone verification (device callback + restored state +
  virtual-MIDI note, via the new `tools/state_donor.cpp` +
  `tools/vmidi_probe.cpp` harness) reproduced the burst pre-fix and
  confirmed the drop post-fix. Along the way the LUT shape-switch crossfade
  got BETTER too (slope excess 0.042 -> 0.018 — the 6x path smooths the
  morph), after fixing a first-restructure bug where the fade advanced for
  the whole block before the table ever evaluated (caught by
  `parvati_fv1_newfamily_test`: 0.136, now 0.018). Onset-regression bounds
  tightened: Overdrive 0.20 -> 0.16 (documented measured character), LUT
  Dist rejoins the default 0.10 class (measured 0.055-0.066).
- **LUT Distortion shape-switch crackle + Digital Echo feedback honesty**
  (subagent-driven fix pair). Switching LUT shapes mid-audio swapped the
  wavetable pointer instantly — two transfer curves at one sample value
  jumped the output (measured **0.19** switch-caused excess slope). Shape
  changes now crossfade old->new over 128 internal samples (~3.9 ms) with a
  per-sample Q.14 fade (fixed-point, lock-free state); post-fix **0.042**,
  pinned by a phase-robust regression (8 alternating 0->8 switches vs
  no-switch reference renders). Digital Echo's feedback mapped the knob to a
  0.95 internal max while displaying 100% — the display lied. Now 0.995
  (-0.044 dB/repeat, reads-as-infinite); regression discriminates 0.95
  (tail 0.77) vs 0.995 (tail 0.97) with 2 dB margin.

### Changed
- **Patch page voice/part configuration presets rebuilt around the voice-first
  model; 0 voices is a real per-part setting.** The arrangement selector's
  six legacy card-split templates (Single / Dual Layer / Dual Split / Quad
  Split / Multi 6 + Mono) are replaced by five voice/part presets:
  **Mono** (1 part, 1 voice, mono), **Poly** (1 part, 16 voices, poly),
  **Unison** (1 part, 16 voices, mono with a per-voice detune spread so the
  stack reads fat, not flat), **Multitimbral** (6 parts, 16 voices each, mono,
  one part per MIDI channel 1..6) and **Drum Kit** (6 parts, 1 voice each,
  mono, Omni, each part mapped to a single GM drum note). Per-part Voices is
  now 0..16 where 0 DISABLES the part (picked as a real combo item, not a
  ghosted placeholder) — the 6 hardware voicecards stay derived from the
  counts. **Custom is now a normal selected combo item instead of ghosted
  placeholder text** (drawn at half opacity through the default
  nothing-selected path), fixing the wrong "Custom" style shown when opening
  a patch that matches no preset. The bottom Voice-pool section was dropped
  from the Patch page (the per-part Voices totals + "Voices Y/96" readout in
  the table cover it); with it went the 30 Hz provider poll. Stock templates
  regenerated to the new presets — every template INCLUDING Drum Kit (GM) is
  generated from its arrangement preset as the single source of truth, so it
  loads showing "Drum Kit" (6 x 1 mono voice, GM zones) instead of Custom;
  Unison carries its spread byte. Header left cluster (preset |
  Patch | Part | Synth | FX) gained consistent pixel gaps between every
  element. Editor test now loads the shipped template files through the real
  load path and asserts the Patch page mirrors them.
- **FX-page labels/readouts fit the knobs (hard 6-char rule).** "Echo" ->
  "Digital Echo", "LUT Distortion" -> "Wavemangler" (display-only; the choice
  stores the index), and the Wavemangler's Shape knob is labelled
  "Wavetable". Digital Echo's Spread param relabelled "Stereo"; LUT shapes
  shortened ("SFold", "Asym", "HGate"). Every Hz readout on the FX page now
  uses the synth filter's compact style ("820Hz", "1k2", "2k6", "15k"),
  times integer ms when >= 10 ("250ms"), decay one decimal ("1.6s"), rates
  spaceless ("0.87Hz"), shifter signed-compact. The FX-routing master-EQ row
  label "Low" -> "Low Cut" (it is a low-cut high-pass). New regression sweeps
  all 24 types x 5 params x raw 0..127 (15,360 samples): every readout <= 6
  chars, plus choice-list rename anchors.

### Added
- **Nine new FV-1-style effects** (second wave, all inside the chip's 32K-word
  delay RAM / saturating Q.23 contract; enum values append-only so presets
  are safe): **Overdrive** (asymmetric 12AX7 LUT, Drive 1-16x + Bias),
  **LUT Distortion** (16 stepped weird waveshapes: Clip/Soft/Tube/Wrap/OctUp/
  Fuzz/Square/Steps/SinFold/Cheby2/Cheby3/AsymCub/Mirror/HalfGate/Crush4/
  Sparse, Drive + shared-clock Jitter +-12 samples + Tone — no bitcrushing,
  the Clocked Delay's Grit owns that), **Compressor** (clean 2:1-10:1 leveling,
  knob-set attack/release), **Gate** (Threshold=0 = fully disabled — the knob
  turns it off; attack/hold/release), **Chorus** (detuned dual-voice),
  **Flanger** (0.15-6 ms, 180-deg stereo sweeps, 0.92 damped feedback),
  **Echo** (ping-pong, 10-470 ms/side, consumes the ENTIRE 32K RAM budget
  exactly), **Room** (Schroeder: 4 combs + decorrelated stereo AP chains),
  **Spring** (two dispersive allpass-cascade springs — real chirp/boing).
  Categories now: Delay 3, Distortion 3, Dynamics 3, Mod 6 (Phaser already
  existed — no duplicate added), Pitch/Time 4, Reverb 5. Per-effect labels/
  readouts/engagement defaults wired; combined JUCE-free unit test
  (parvati_fv1_newfamily_test) + param-coverage + onset-regression coverage
  for all 24 effects (measured-character bounds documented for the
  distortion family).
- **Categorized FX-type dropdown with submenus.** The FX-slot picker now
  groups effects by what they do: "None" plus one SUBMENU per category,
  alphabetical — Delay (Clocked Delay, Looping Delay), Distortion
  (Wavefolder), Dynamics (Vinyl Compressor), Mod (Ensemble, Frequency
  Shifter, Phaser, Ring Modulator), Pitch/Time (Pitch Shifter, Resonator,
  Spectral, Pitch Stretch), Reverb (CVerb, Diffuser, Plate) — effects
  alphabetical inside. 7 top-level entries instead of 16 flat rows, so the
  popup fits short panes. Display-only: the combo's internal items (and the
  APVTS choice list hosts see) stay in enum order, so serialization, host
  automation, and the < > step buttons are untouched; the collapsed combo
  keeps the plain effect name. The Resonator files under Pitch/Time (its
  defining control is Pitch — it re-rings the input at the tuned pitch; a
  modal bank is the filter-domain dual of a tuned delay network). New
  regression pins the display-order invariants (every type exactly once,
  None first, categories ascending).
- **FX display renames:** "WSOLA Stretch" -> "Pitch Stretch" and "Plate
  Reverb" -> "Plate" (display-only — the APVTS choice stores the index,
  never the text, so saved sessions/presets are unaffected).

### Changed
- **Vinyl Compressor retuned to the SP-303/404 "Vinyl Sim COMP" character.**
  Grounded in the Roland spec (COMP = "the compression feel, a unique part
  of the analog record's sound"; NOISE; WOW FLUT — the old-school 303/505
  "slower heavy, warpy" flutter) and producer descriptions (warm, gluey,
  snare-friendly squash). Compression is far deeper: threshold now sweeps
  1.0→0.04 (was →0.1), ratio 4:1→~16:1 (was fixed 4:1), makeup 1→6 (was
  1→4) with a 0.8 ms attack / 250 ms release envelope — measured: a 15:1
  input-level ratio is crushed to 1.17:1 at full Compress, and quiet
  material is pushed UP (the SP glue). New character stage: a fixed-point
  cubic soft-saturation "lathe" after the makeup (unity slope at zero,
  flat-top above the knee, drive rises with Compress) — the analog warmth.
  Wow/flutter is the old-school SP warble and now AUDIBLE: slow heavy
  0.4 Hz wow (≤ 300 samples ≈ 2.3 % pitch deviation) + 3.1 Hz flutter
  (≤ 24 samples ≈ 1.4 %) on the 50 ms delay; the first tune (24/6 samples
  = 0.18 % deviation) sat below the ~0.3 % slow-FM hearing threshold —
  measured period swing is now 12 % p-p, pinned by a new zero-crossing
  audibility regression (≥ 1.5 %). The Pitch knob is relabelled "Wow/Flut". The crackle is now a SUBTLE noise floor instead of
  full-scale pops: sparse (~0.6 %) decaying soft ticks with u²-skewed
  amplitudes (ceiling ~0.18, measured max 0.138 at full knob vs 1.0
  before) + a low ~-54 dB hiss; Age LP range widened to 700..15 kHz.
  Card default Crackle lowered to 0.14. Tests rewritten to the SP
  semantics (squash ratio, tick ceiling, hiss floor, flat-topping
  character); all FX + FV-1 suites pass.

### Fixed
- **"Loud crackle at note start with FX" — the binaries in use were stale
  (built before the FX crash/crackle fixes). First found for the installed
  plug-in bundles (Aug 11, even pre-FV-1); the remaining report was the
  STANDALONE: /Applications/Parvati.app was an Aug 16 build — after the
  FV-1 family but before the Aug-17 `69e678d` crackle fixes — because the
  `deploy` target built the Standalone but never INSTALLED it (README kept
  the .app copy as a manual step). Investigation across ALL 15 FX on current
  code (fresh-start note onsets at 44.1/48/96 kHz × buffers 64–1024, single
  note / 50 % mix / 6-note vel-127 chord, plus mid-note enable / type-change /
  dry-wet steps) found no onset crackle: worst impulse ≤ 0.09 (VinylComp's
  designed crackle knob) vs the dry baseline 0.06. Rebuilding the exact
  commits the stale binaries matched reproduced the report: Wavefolder 0.61
  (247 impulses, the LUT-overrun rodata garbage) and RingModulator 0.98.
  Fix: rebuilt + reinstalled VST3 / AU / CLAP AND the Standalone
  (`cmake --build build_release --target deploy` now also installs
  Parvati.app into /Applications with an ad-hoc re-sign — a plain cp leaves
  JUCE's resource-referencing signature invalid; AU validation passes, the
  standalone launches/quits cleanly). New onset regression
  (`parvati_fx_onset_regression_test`, built by default) pins all 15 FX'
  first 250 ms — the region the continuity regression deliberately skips.
  Also fixed that regression's params: `fx1_param3 = 64` (64/127 = 0.504 >
  the 0.5 threshold) FREEZES LoopingDelay/WSOLA into a silent empty
  buffer, so their wet path was never actually tested; params are now
  freeze-safe and the buffer FX' real wet output is asserted.
  Known non-crackle follow-up found by the new chord scenario: Reverb can
  peak ~1.6 (> 0 dBFS) on a loud 6-note chord at full wet (tank build-up,
  ~330 ms in).
- **FX onset regression coverage validated + hardened.** The new
  `parvati_fx_onset_regression_test` is now PROVEN to fail on the pre-fix
  commit d2669c4 (built and run in a worktree): RingModulator fails every
  scenario (0.34–0.98, the stale Warps SRC history pointer) and
  VinylCompressor fails onset (0.18–0.21) — while all 60 checks pass on the
  fixed tree. Fixes found while validating: the Wavefolder override bound
  the wrong slot (Tone is param4, not param3 — the tone LP stayed engaged,
  masking, and Bias was slammed to +0.2); Wavefolder now runs the all-64 grid
  whose 2 kHz tone LP separates legit fold HF (fixed tree ≈0.09) from the
  impulsive pre-fix LUT garbage; measured-character bounds (0.16) documented
  for Wavefolder (fold slope-gain on loud attacks, 0.098 chord),
  VinylCompressor and Phaser (FV-1 designed lo-fi resampling/fixed-point
  floor 0.112–0.136 — identical before/after the fixes, i.e. character);
  a sustain window (0.25–1.2 s) catches mid-note crackle beyond any
  onset-only window. The Wavefolder LUT-overrun defect is pinned directly:
  the `parvati_clouds_fx_test` LUT-domain checks were tightened from a
  1e3 no-garbage bound to rails-bounded (|out| ≤ 1.2; fixed peak 0.935,
  pre-fix 1.7e30) plus continuous-under-overrun (worst delta < 0.7; fixed
  0.439, pre-fix ~1e36) — validated both ways on d2669c4.
- **FX crash + crackle root causes (deep audio/memory audit).** The
  Wavefolder hard crash was an out-of-bounds read in the fold waveshaper:
  the LUT lookup index was never clamped (valid |sl| ≤ 2.295), so max Drive
  plus a loud chord's summed voice output read past `lut_bipolar_fold`
  into rodata garbage (crackle) or off the module (SIGSEGV; NaN input made
  the int cast UB). The index is now clamped to the valid domain (both
  channels) and the per-part FX chain input rides a unity-gain soft-limit
  knee (8·SoftLimit(s/8), transparent below unity, hard-capped ±16).
  Also fixed: Reverb tank state could seed from uninitialized heap (NaN
  forever); the Warps SRC_DOWN fast/circular path switch left a stale
  history pointer (periodic crackle in Wavefolder/RingMod at ≥88.2 kHz —
  measured 1.66-sample discontinuities, now exactly the signal slope);
  no guard against host blocks larger than prepared (6×-amplified heap
  overflow in the Wavefolder scratch) — clamped at both the processor and
  chain layers; the deferred-parameter drain allocated under its spinlock
  (audio thread could spin against a malloc — priority inversion); FX
  parameter automation allocated strings on the render thread (now
  deferred off-thread like arp/seq); the FV-1 clocked delay stepped its
  delay length on tempo changes (now a per-sample Q.16 glide).
- **Pitch Shifter right-channel crackle with Spread > 0.** The stereo
  spread offset shared the left channel's crossfade window, so the right
  taps wrapped the delay window where the envelope gain was NOT zero — a
  periodic right-only discontinuity scaling with Spread. The right channel
  now has its own crossfade phase (rate-limited offset, phase-locked to L);
  spread=0 stays bit-identical mono. New regression: R/L jump symmetry.
- **WSOLA Stretch startup splice transient.** The first correlator-placed
  window ramped to full gain exactly as it stepped onto the first recorded
  sample (an instantaneous step, right-channel-worst with phase-offset
  input). The startup window is now head-anchored so its gain ramp fades
  the recorded audio in; new startup-symmetry + post-startup-cleanliness
  regressions. (A per-effect sweep across all 15 FX types x parameter
  corners found no other one-sided discontinuities.)
- **FX audio-thread allocation elimination (deferred audit items).** FX
  type changes no longer construct/destroy/free processors on the audio
  thread: the message thread builds + prepares the replacement and stages
  it (3-state CAS per slot); the audio thread installs it with pointer
  moves only; a 60 Hz reaper frees the retired processor. The per-voice
  filter-oversampling change (up to 96 `juce::dsp::Oversampling` rebuilds
  in one callback) uses the same staged-swap pattern with a belt-and-braces
  AT fallback. `FxChain::latency()` reports the staged type immediately
  (PDC/dry-alignment planning no longer lags one install behind).
- **FX card header geometry.** The power-indicator hit band no longer
  overlaps the type dropdown at the narrowest cards (44×22 header band,
  combo exactly centred, overlap-gate clean at the 800 px floor).

### Changed
- **Status strip + chrome cleanup: right-side indicators, no per-voice
  indicators.** The bottom status strip keeps ONLY the current
  CPU% (`CPU N%`, the current block's render-time/budget ratio — the peak /
  overrun suffix, its colour keying and the right-click probe reset are
  gone) and moves BOTH indicators to the right edge (`CPU %` rightmost, the
  `0/16` part-relative voice count just left of it; the hover-tooltip bar
  fills the left). The [KBD] keyboard-overlay toggle keeps its 44px target
  at the far right of the header's icon cluster. The per-voice `V1..V16`
  activity indicators are REMOVED: the part-relative VoiceMeter component is
  deleted entirely, and the Patch page's voice-pool view drops its per-voice
  squares (each row keeps its part label + `active/allocated` count and the
  `X/96` total). The Patch/Global panel now also hosts the ARRANGEMENT
  selector (Mono/Single/Dual Layer/Dual Split/Quad Split/Multi 6) and the
  `Voices Y/96` readout as a 44pt summary row above the 6 part rows (the
  page-level heading chrome is gone — the panel reads top-down:
  arrangement, then the parts it configures), and the per-part Zone Low/High
  knobs ignore the mouse wheel (an unhandled wheel scrolls the page instead
  of tweaking the knob). The wheels panel's `[<][>]` octave-switch buttons
  grow from a 22x18 chevron to a 44x44 HIG touch target (pinned by the sizing
  test). The voice-pool view's 30 Hz poll is now gated on
  `isShowing()` (the old `visibilityChanged` gate never fired for the nested
  view when an ancestor page hid, so the provider ran at full rate while the
  Patch page was off-screen). The GitHub Actions workflow is removed; the perf
  gates stay runnable locally (`parvati_perf_smoke_test`, `tools/profile_ui.sh`).
- **Patch page: one merged Global section + honest voice counts.** The
  hosted Part/Play group is merged INTO Global (one 11-control panel:
  part volume/octave/tuning/spread/scale/legato/portamento/polyphony +
  VCA curve/filter card/drive), and the hosted page now lays out at its
  NATURAL height (new `reflowToWidth(w, -1)` natural-height mode) — the
  ~160px viewport-fill void between the global section and the part rows
  is gone. The per-part Ch column widens for "Omni", the Legato On/Off
  combo readout gets room for its widest choice, and `Voices Y/96` (+ the
  voice-pool picture) counts ASSIGNED slots (`getPartVoiceSlots`), not the
  audio-thread allocation snapshot — one active part with 1 voice reads
  `Voices 1/96`.
- **FX: indicator-dot power toggles + a diagram-safe ROUTING column.** The
  per-slot on/off power glyph is replaced by a bordered circle filled with
  the FX accent when enabled and grey when bypassed (same click wiring,
  44pt target). The ROUTING column floor rises 176 → 232pt (19%, capped
  288) so the series/parallel flow diagram never clamps its blocks into
  the OUT label.
- **Multi loads reset voice settings to init.** Loading a `.MUL` or a
  `.parvati` multi first resets every Part's voice slots to the engine init
  allocation (Part 1 = 6 voices, others disabled) — a file that does not
  carry voice settings for a Part can no longer inherit the previous
  multi's leftover counts; files that DO carry slots still round-trip them.
- **Filter oversampling defaults to 2x, supports 8x.** New instances (and
  states that never persisted a factor) run the filter at 2x; the combo
  gains "Ultra (8x)" and the whole chain (voices, latency probe, clamps)
  accepts 1/2/4/8. Persisted factors — including 1x — restore unchanged.
- **Big two-octave keyboard overlay.** The on-screen keyboard shows exactly
  C3–C5 (25 keys, ~80pt white keys) and the overlay strip grows 76 → 246pt
  — tall enough to cover the entire bottom row (generator editor + mod/FX
  matrix) when [KBD] is on; the pitch/mod wheels widen to match. QWERTY
  musical typing is clamped inside the visible window.
- **[KBD] stays visible in Patch mode + workspace rows rebalanced.** The
  virtual keyboard overlay (and the pitch/mod wheels) is re-lifted above
  the full-page Patch overlay whenever it is toggled on or the Patch page
  is entered with [KBD] active (previously entering Patch mode buried a
  visible keyboard under the overlay); a showing Settings side panel is
  re-lifted last so it still covers the keyboard. The SYNTH/FX workspace
  bottom row (active generator editor + mod/FX matrix) now keeps the height
  it has with the [MOD] pill bar shown and is capped at exactly four
  matrix rows (+ header/add chrome); hiding the pill bar grows only the
  top synth/fx section, and the matrix scrolls inside its own viewport for
  longer routing lists. SYNTH and FX keep byte-identical row math so the
  mode toggle never reflows.
- **Patch page: Part/Play + Global knobs on top, Polyphony column shrunk.**
  The hosted patch-wide ParamPage (Part/Play + Global groups) now leads the
  scrolled body (part rows and the voice-pool view follow below), and each
  part row's Polyphony column is sized to its dropdown (140pt) instead of
  stretching across the leftover row tail.
- **Voice-first Patch page: voice counts are the user model, cards are
  derived.** On real Ambika hardware a voice IS a voicecard (the digital
  voice section lives on the card), so Parvati now exposes exactly that: a
  **Voices 1..16** count per Part (drawn from the fixed 96-voice pool,
  6x16 — any combination is legal, every Part can be maxed at once), and the
  6-voicecard bitmask is DERIVED internally from those counts
  (`mul_export::deriveMasks`: contiguous proportional share, exact counts
  while they fit in 6, min 1 per active Part) purely for the individual
  outputs, `.MUL`/hardware export and the legacy seeds. The Cards column,
  the 6-card budget enforcement and the Voices combo's `Auto` item are
  GONE (a Part is disabled by an arrangement preset or a loaded multi; a
  disabled Part shows `0` with no selection); the header keeps a single
  `Voices Y/96` pool readout. MONO/unison follows the count (MONO + N
  voices = N-voice unison).
- **Six new arrangement presets** replace the five card-budget ones
  (Single/Stack 2/Split 2/Layer 2/Multi 6): **Mono** (TRUE mono: 1 voice +
  MONO poly — one retriggering voice, no unison), **Single** (16 voices),
  **Dual Layer** (8+8, both full-range Omni), **Dual Split** (8+8, split at
  48), **Quad Split** (4x8, splits at 36/60/84) and **Multi 6** (6x16 —
  the whole pool, Parts on channels 1..6). Inference is an exact-preset
  match over voices + channels + zones + polyphony; anything else reads
  Custom. FR/DE translations added (Mono, Dual Layer, Dual Split, Quad
  Split; Cards/Auto/Stack/Split 2/Layer 2 entries removed).
- **Templates updated to the slots model.** Mono loads part 0 with 1 voice
  (+ MONO), Poly/Unison with 6, Multitimbral with 3+3 — the old files
  carried `voice_slots: 0`, which the loader now (correctly) reads as a
  disabled Part. All five templates regenerated via `parvati_gen_templates`
  and byte-identical to a fresh run (this also picks up the `part_raga`
  line the committed files had drifted on). The `.MUL` export strategies,
  fallback dialog and `.PRO` export are untouched; a default (AsIs) save
  now carries the DERIVED contiguous masks, which round-trip identically
  for contiguous setups.
- **Part-relative voice meter + the global voice-pool view on the Patch
  page.** The voice-activity UI now reflects the per-part voice-slot
  extension (96-voice pool). The Global-page meter and the bottom status
  count show the CURRENT part only (its allocated voices / its active
  count), and the global picture lives in a new "Voice pool" panel on the
  Patch page: all 6 parts listed with a square per allocated voice (filled
  = active), per-part `active/allocated` counts and the total allocation
  `X/96`, below the part rows and above the Global section. A `Voices
  Y/96` pool-budget readout sits in the header next to the arrangement
  combo; the Voices combo carries a plain-language tooltip (voices =
  this part's polyphony from the shared 96-voice pool; cards are derived
  for the individual outputs + `.MUL` export). CHAIN parts can own up to
  32 voices (their set is doubled): a pool row shows 16 squares plus a
  `+N` marker, but every count (per-row and the `X/96` total) reflects
  the FULL allocation. Both view timers pause while their page is
  off-screen. New strings translated (FR/DE): Voices, Voice pool.
- **Full test sweep + docs brought to the voice-first model.** All 64
  `parvati_*` test targets pass (one stale `export_fallback` assertion
  updated: the default/AsIs `.MUL` baseline is now the DERIVED masks);
  the four preset templates are regenerated to match `parvati_gen_templates`
  byte-for-byte. README features, `docs/ARCHITECTURE.md` (voice-model /
  slots-truth section, `deriveMasks` as the single card-mask source of
  truth), `docs/UI_MODERNIZATION_PLAN.md`, `docs/DSP_PORT_SPEC.md` and
  `tests/COVERAGE_SPEC.md` (new voice-counts & arrangement coverage
  section) now describe voices-1..16 + derived cards instead of the
  6-card budget.
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
- **Voice-activity UI was stuck on the pre-pool 6-voice model.** The
  Global-page meter kept 6 fixed cells fed by pool voices 0..5 — which are
  always Part 1's first six voices (the pool is partitioned in part order),
  so it showed Part 1 regardless of the selected part and could never show
  more than 6 of a part's up-to-16 voices. The bottom status count mixed a
  whole-pool active numerator with the current part's allocation denominator
  (impossible fractions like `23/16`). Both are now part-relative; the
  global picture lives in the new Patch-page voice-pool view.
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
    Tom 45), Omni channel, 1 mono voice per drum (matching the built-in
    Drum Kit arrangement, so it loads showing "Drum Kit"), named
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

### Known limitations (documented, deferred — see audit/IPAD_TOUCH_TODO.md)
- AUv3 panes narrower than the 1024pt floor collapse header chrome (needs an
  adaptive header design decision; lane-C finding 1).
- Note routing is first-match, not all-matching-parts, where firmware triggers
  EVERY accepting part — a deliberate product decision is needed (lane-B
  finding 4).
- Polyphonic aftertouch is silently ignored (channel pressure works; lane-B
  finding 5).

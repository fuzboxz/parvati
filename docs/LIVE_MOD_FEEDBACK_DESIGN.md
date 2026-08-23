# Live Modulation Feedback (Pigments-style) — Design & Contract

Status: implemented (2026-08-21), reviewed and fix-hardened. This document was
the BINDING CONTRACT for the implementation. The deviations below are recorded
per its own rule.

## Recorded deviations (post-review)

- **kNumSources = 32 vs MOD_SRC_LAST = 31.** The header comment originally
  claimed equality. The enum's sentinel value is 31 (31 named sources). The
  engine therefore asserts `kNumSources >= MOD_SRC_LAST`, and the spare slot
  31 stays zero (the UI enumerates only real enums).
- **Filter live-overlay activity is TEMPORAL, not spatial** (revised from the
  original "live vs base >= 2 bytes" gate). The effective cutoff byte includes
  key tracking (~2 bytes/semitone). A spatial threshold therefore trips for
  every held note on any patch with tracking — an always-on second curve for
  a static patch setting. The overlay now shows while the effective bytes
  MOVE (>= 1 byte/tick) and holds ~270 ms after the last movement. A settled
  note returns to the single opaque base preview. The base tick still dims
  to 0.30 while the overlay is visible.
- **The two preview polls stay at 30 Hz** (the architecture diagram's
  numbers). The refresh setting caps the engine→UI data cadence and the
  bar-strip animation. The previews' repaints are change-gated off the hub
  cache, so the setting caps their EFFECTIVE animation cadence end-to-end.
  Only the (cheap, gated) poll wakeups stay at 30 Hz. The editor's
  visibilityChanged additionally stops the hub itself (a hidden editor polls
  nothing).
- **The engine's ring head lives IN the frame** (`historyHead`, written under
  the same seqlock critical sections) rather than as a separate plain member.
  The reader's copy is therefore always self-consistent. The linearized UI
  frame zeroes it.
- **Strip diff-gate signature includes a position-weighted moment** (Σ j·v[j])
  on top of count/first/last/min/max, so a pulse that slides through an
  otherwise static window (GATE / VELOCITY / ARP) still animates.
- **Note-Sequencer pill preview rides the spare telemetry slot
  (kNoteSeqSlot)**: the bar-only NOTE sentinel has no MOD_SRC_* enum (its
  output is note events). The engine therefore appends the tracked part's
  active sequencer note (0..127 -> 0..254, 0 = rest) into the one
  spare history slot — a unipolar melody trace with rests as gaps.
- **Filter preview shows ONE curve (2026-08-21 user request)**: while the
  live overlay is active, the STATIC base curve + its dim fc tick are HIDDEN.
  The live curve renders with the base recipe (gradient fill), so the preview
  reads as one curve that moves under modulation and settles back at rest
  (the temporal gate hands over seamlessly).
- **STICKY telemetry voice (2026-08-21 — the "jumpy slow envelope" fix)**:
  history appends follow ONE voice per note, NOT the FX representative voice
  (which jumps to the newest strike). A slow-release tail interleaved with
  fresh attacks made the ENV rows read as noise. The sticky pick (voice slot
  + triggerSeq) holds until that exact trigger dies, then re-picks. Pinned by
  ui_telemetry_test [7]: a held slow attack stays monotonic (max 1
  byte/sample) AND a re-strike mid-hold does not interleave. Also:
  kModRingCap 12 -> 34. Larger host buffers (512+ @ 48k) dropped their
  trailing internal blocks and re-read a stale ring entry.
- **Musical typing survives control tweaks (2026-08-21)**: every control in
  the editor tree has wantsKeyboardFocus OFF (except the KeyboardView itself
  and TextEditors) via a ctor-end + [KBD]-show pass. Clicks on knobs, wheels,
  combos or pills mid-performance no longer steal the QWERTY-note focus.
- **Strip contrast/stroke (2026-08-21 user feedback)**: alpha 0.85/0.95
  (inactive/active) + a 2.2px stroke. The early low-alpha thin trace was
  illegible against the dark pill fill. The drag-only pills' dotted
  left-handle specks were removed (the family band + underline carry the
  cue).
- **OscPreviewDisplay live overlay + filter-parity smoothing (2026-08-23
  parity pass)**: the OSC waveform preview joined the consumer set with the
  SAME contract that the filter preview has. New snapshot field
  `effOscParam[2]` (0..127, `dst_[MOD_DST_PARAMETER_{1,2}] >> 7` — exactly
  what `UpdateDestinations` feeds the oscillators); new payload
  `LiveOscValues`; new hub accessor `liveOsc(i)`; and the editor wires a
  `setLiveValuesProvider` per osc preview. The activity gate is the filter's
  temporal one (a >= 1 byte move vs the previous tick arms a ~270 ms hold).
  There is no second curve — the ONE waveform's smoothed display target
  switches from the knob value to the live effective byte while armed and
  eases back at rest. Fluidity parity came with it: the displayed parameter
  is now a critically-damped smoothed value (tau 130 ms, the filter's exact
  model), rebuilt whenever its byte-quantized level moves. This replaces the
  former 8-step quantized rebuild + 66 ms morph-restart chain. The per-byte
  rebuild of the DSP-sampled shapes is flicker-free, because every rebuild
  uses a FRESH `Oscillator` instance (deterministic per (shape, byte);
  pinned by osc_preview_live_test [a]).
- **Poll timers START unconditionally (provider + rate), not gated on
  isShowing()** (2026-08-21 reliability lesson). The hub and the mod-bar
  strip poll originally required isShowing() to START. But isShowing() is
  peer-derived and proved unreliable as a start condition: JUCE's
  visible-before-desktop / content-then-peer sequencing can starve the
  visibility hooks entirely, and a deactivated host process flips the peer
  state under a running tick. A starved start left the pump dead forever and
  every strip cleared (the shipped "no indicators visible" bug; editor_test
  [25] now pins this with an end-to-end check: real processor + real editor
  + held note -> the editor's OWN bar must animate). The per-tick guards
  remain (a cheap no-op when not visible / no provider). The data-driven
  repaint gates stay the real cost control. Vertical geometry also settled
  per user spec: symmetric 4px above the label tab and below the pills
  (kBarHeight 82); horizontal paddings at their original tight values.

## Goal

A Pigments-like live feedback system:

1. **Mod pills show a history sparkline** — every CentralModBar pill (except
   the Const cluster and the bar-only Note Sequencer sentinel) draws a
   subtle, themed, animated strip of the RECENT values that its modulation
   source produced (LFO waveform, envelope shape, aftertouch motion, velocity
   hits...). The strip is visible but not distracting. It fits the flat
   quiet-chrome pill style.
2. **Envelope previews show the live stage** — while a key is held, the ADSR
   EnvelopeDisplay draws a position marker (dot + hairline) that moves
   through Attack / Decay / Sustain / Release. It uses the engine's real
   envelope stage + progress.
3. **Filter preview reflects live modulation** — when the filter cutoff /
   resonance is ACTIVELY modulated (env-2 sweep, LFO, matrix, wheel...), the
   FilterResponseDisplay draws the live effective curve + a moving cutoff
   tick. The static base preview stays fully opaque in place.
4. **Reset semantics** — history, envelope markers and the live filter curve
   reset (hide / clear) on patch load / patch switch / part switch / init.
5. **Performance-first** — one shared engine snapshot per tick (seqlock,
   single audio-thread writer), per-component change-gated bounded repaints,
   a user-tunable refresh rate (10/15/30/60 Hz, persisted), and full
   visibility gating (nothing polls while the editor / seam is hidden).

## Architecture

```
audio thread                       message thread
--------------                     ------------------------------------------
renderPartFx (per part,            LiveFeedbackHub (editor-owned juce::Timer
  per internal block)                 at ui_refresh_hz) -> engine.readUiTelemetry()
    |                                   | cached parvati::ModTelemetrySnapshot
    +-- decimated history append        +-- CentralModBar::timerCallback (bar rate)
    +-- per-block snapshot write        |     per-pill diff gate -> strip repaint(rect)
        (seqlock, ~4 KB)                +-- EnvelopeDisplay::timerCallback (30 Hz)
                                        |     stage marker (change-gated)
                                        +-- FilterResponseDisplay::timerCallback (30 Hz)
                                        |     live curve (change-gated)
                                        +-- OscPreviewDisplay::timerCallback (30 Hz)
                                              live effective param (change-gated)
```

- The engine NEVER allocates on the audio thread for this; writes are
  bounded, fixed-size, seqlock-guarded.
- The UI NEVER blocks: `readUiTelemetry` is a bounded-retry copy; on a torn
  read, it returns false and the hub simply keeps the previous cache for one
  tick.

## Shared types — `Source/ui/ModTelemetryTypes.h` (WRITTEN, do not modify)

`parvati::ModTelemetrySnapshot` (trivially copyable, dependency-light):

- `kNumSources = 32`, `kHistoryLen = 256` (doubled from 128 on 2026-08-22
  user feedback — strips scrolled visibly faster than the previews; the
  window is now ~3.13 s) (engine static_asserts
  `kNumSources == ambika::dsp::MOD_SRC_LAST`).
- `epoch` (uint32) — bumped by the message thread on every reset; a snapshot
  whose `epoch` != the engine's authoritative epoch is INVALID.
- `part` (int) — the part this telemetry describes.
- `sources[32]` — CURRENT effective mod-source values, 0..255.
- `history[32][256]` + `historyCount` — OLDEST→NEWEST per-source ring,
  linearized by `readUiTelemetry` (storage uses `historyHead` internally).
- `envStage[3]` (0..4 ATTACK/DECAY/SUSTAIN/RELEASE/DEAD), `envProgress[3]`
  (0..1 within stage), `envLevel[3]` (0..1 current output) — the
  representative (most-recently-triggered active) voice's envelopes 1..3.
- `effCutoff`, `effResonance` (uint16 0..255 — effective,
  modulation-applied), `filterMode` (0..3), `voiceActive`.

`parvati::LiveEnvStage` and `parvati::LiveFilterValues` — the small structs
that the display components take (this avoids cross-includes between hub and
components).

## Engine contract — `SynthEngine` (engine task)

Public API (implemented in SynthEngine.h/.cpp):

```cpp
bool     readUiTelemetry (parvati::ModTelemetrySnapshot& out) const; // false = torn/stale
void     resetUiTelemetry();          // message thread: bump epoch + request clear
uint32_t uiTelemetryEpoch() const noexcept;
void     setUiTelemetryPart (int part); // message thread: track this part (clears on change)
```

Write path (audio thread, inside `renderPartFx`):

- Track ONLY `uiTelPart_` (MT-published atomic). Servicing rules:
  - If the tracked part changed or `uiTelResetReq_` is set: clear history +
    sources (voiceActive=false), copy the CURRENT authoritative epoch into
    the snapshot, reset the decimator.
  - Append history ONLY while the tracked part has an active representative
    voice, decimated every 12th internal block (~81.7 Hz at the 980 Hz
    internal cadence) → 256 samples ≈ 3.13 s window (was 128/1.57 s; see the
    kHistoryLen note).
  - Once per `renderPartFx` for the tracked part (with an active rep voice,
    or on the active→inactive transition — never a steady idle write):
    update `sources[]`, envelope stage/progress/level, effective cutoff/
    resonance/mode, voiceActive — all under the seqlock.
- Seqlock: `uiTelSeq_` (single writer = audio thread; single reader =
  message thread). Writer: odd-begin / release-fence / write /
  release-fence / even-end. Reader: bounded (64) retry, acquire loads —
  same discipline as `Part::readPendingConfig`.
- Epoch validity: `resetUiTelemetry()` bumps an MT-authoritative
  `uiTelemetryEpoch_`; `readUiTelemetry` returns false when the snapshot's
  epoch is stale. A reset is therefore visible to the UI IMMEDIATELY
  (before the audio thread services the clear).
- Envelope progress: `phase() / 65536.0f` when `phase_increment() > 0`,
  else 1.0 (SUSTAIN/DEAD hold). Needs new const accessors in
  `dsp/envelope.h` (`phase()`, `phase_increment()`, `value_byte()`), a
  const `envelope(i)` accessor on `dsp::Voice`, and AmbikaVoice wrappers
  (`envelopeStage/Phase/PhaseIncrement/ValueByte(i)`, `effectiveCutoff()`,
  `effectiveResonance()`, `filterMode()`).
- Bipolarity for display (mirrors the voice mod-matrix AC coupling):
  bipolar sources = `MOD_SRC_LFO_1..4`, `MOD_SRC_PITCH_BEND`,
  `MOD_SRC_NOTE`; every other source is unipolar (0 = floor). UI draws
  bipolar around a midline. (The informational constant lives in
  ModTelemetryTypes.h: `parvati::isBipolarModSource(int)`.)

## CentralModBar contract (modbar task)

- `CentralModBar` privately inherits `juce::Timer`.
- New API:
  ```cpp
  void setTelemetryProvider (std::function<bool(parvati::ModTelemetrySnapshot&)> fetch);
  void setTelemetryRateHz (int hz);       // clamps 5..60; 0 disables; restarts timer
  int  telemetryRateHz() const noexcept;
  void clearTelemetry();                  // hide all strips (invalid snapshot)
  int  telemetryGeneration() const noexcept;   // TEST-ONLY: bumped on a real data-driven repaint
  ```
- Timer: gated by visibility (the F-ios-perf-3 dual-hook pattern:
  `visibilityChanged` + `parentHierarchyChanged` → start/stop). One timer
  for the whole bar (never per pill).
- Per tick: fetch snapshot (invalid → clearTelemetry, stop repainting). For
  each pill with `enumValue_ >= 0 && cluster_ != Cluster::Const`: downsample
  `history[enum]` to ≤ 24 points and compare against the last-drawn
  signature (eps 1/255). If changed → update the pill's cached strip points
  and call `pill->repaint(stripRect)` (BOUNDED dirty rect — this is the
  GPU-cost control). Idle sources cost nothing.
- Pill paint: a history STRIP band at the bottom of the pill
  (`kStripH ≈ 13px`, above the family underline), stroke ≈ 1.25px
  round-joined polyline in the family accent at LOW alpha (0.45 inactive /
  0.6 active), no fill. Bipolar sources centre on the strip midline;
  unipolar fill starts at the strip bottom. The label moves up into the
  remaining pill top (label area = pill height − strip). Geometry otherwise
  unchanged. When `historyCount == 0`, the strip paints nothing.
- Const cluster pills and the Note Sequencer sentinel never draw a strip.

## EnvelopeDisplay contract (previews task)

- New API:
  ```cpp
  void setLiveStageProvider (std::function<parvati::LiveEnvStage()> p);
  // TEST-ONLY:
  bool  liveMarkerVisibleForTest() const noexcept;
  float liveMarkerXForTest() const noexcept;   // normalized 0..1
  ```
- Refactor the ADSR segment math so that the marker and the curve share ONE
  definition (a static `adsrSegmentSpans(a,d,s,r,&wA,&wD,&wS,&wR)` helper;
  the existing `adsrCurveLevel` keeps its exact shape).
- Marker (previewMode 0 only, provider present, `active && stage != DEAD`):
  x = segment start + progress × segment width (fraction of total), with
  the SAME segment proportions as the drawn curve. Draw a 3.5px filled dot
  ON the curve at (x, adsrCurveLevel(x)) in the trace colour, plus a 1px
  vertical hairline (trace @ 0.28 alpha) through the plot. SUSTAIN pins
  progress=0 → the dot rests at the plateau start.
- Repaint gating: the existing eps param-change gate is EXTENDED — also
  repaint when the marker x moves > eps or the active flag flips. No
  provider / LFO mode → zero overhead.

## FilterResponseDisplay contract (previews task)

- New API:
  ```cpp
  void setLiveValuesProvider (std::function<parvati::LiveFilterValues()> p);
  // TEST-ONLY:
  bool  liveCurveVisibleForTest() const noexcept;
  float liveCutoffXForTest() const noexcept;   // normalized 0..1
  ```
- `LiveFilterValues.cutoff01/reso01` are the EFFECTIVE bytes normalized to
  0..1 of the 0..255 domain (the same domain as the base curve's
  `roundToInt(cN * 255)`).
- Paint: the BASE curve stays exactly as today (opaque, in place). When the
  provider is active AND the live bytes differ from the base bytes by ≥ 2
  (either axis): additionally draw the LIVE curve (same magnitude model,
  live fc/K) at higher weight (≈1.75px, full trace alpha, no fill) + a
  bright live cutoff tick. The base tick dims to ~0.30 alpha while
  modulation is active. Below the threshold → single base curve only (no
  duplicate strokes).
- Repaint gating: extend the existing eps gate with the live-vs-base and
  live-vs-lastLive deltas.

## OscPreviewDisplay contract (2026-08-23 parity pass)

- New API:
  ```cpp
  void setLiveValuesProvider (std::function<parvati::LiveOscValues()> p);
  // TEST-ONLY:
  bool liveOverlayActiveForTest() const noexcept;
  ```
- `LiveOscValues.param01` is the EFFECTIVE byte normalized to 0..1 of the
  0..127 domain (the same domain as the display's base `roundToInt(p*127)`);
  the hub derives it from the snapshot's `effOscParam[osc]`.
- ONE waveform, no overlay curve: while the temporal gate is armed (the live
  byte MOVED >= 1 vs the previous tick, held ~270 ms), the display's
  SMOOTHED parameter target switches from the knob value to the live byte;
  at rest it eases back. The code rebuilds the cycle whenever the
  byte-quantized SMOOTHED value moves (analytic glyph or a fresh
  deterministic Oscillator render — see the deviation bullet above).
- Repaint gating: the filter's exact gate — shape change || base param
  change (eps) || live change (activity flip or >= 1 byte) || still
  converging (eps 1/1024).

## Settings + persistence (settings task)

- `ParvatiAudioProcessor`: `int getUiRefreshHz() const` /
  `void setUiRefreshHz(int)` (clamp 5..60, default 30), guarded by
  `uiPrefsLock_`, persisted as `"ui_refresh_hz"` in getStateInformation /
  setStateInformation next to the other ui_* props (legacy states default
  30).
- `SettingsPanel`: a "Visual Refresh" (TRANS) label + combo with items
  10 / 15 / 30 / 60 Hz (IDs = Hz, 30 default; labelled e.g. "30 Hz
  (Default)"), new ctor callback `onRefreshChanged(int)` fired after
  `proc.setUiRefreshHz`. Row layout follows the existing pattern.

## Editor wiring (wiring task — AFTER the above)

- Editor owns `std::unique_ptr<parvati::LiveFeedbackHub> liveHub_`
  (Source/ui/LiveFeedbackHub.{h,cpp} — WRITTEN, do not modify) constructed
  with a fetcher bound to `engine.readUiTelemetry`.
- Wire `setTelemetryProvider` on BOTH workspace bars (SynthWorkspace::modBar_
  and FxWorkspace::modBar_ — add a public `CentralModBar* modBar()` accessor
  to each workspace if missing).
- Wire `setLiveStageProvider` on the three ADSR EnvelopeDisplays (Env 1/2/3)
  and `setLiveValuesProvider` on the FilterResponseDisplay, via the hub
  cache.
- Apply the refresh rate: in `ParvatiEditor::timerCallback`, when
  `processor.getUiRefreshHz()` differs from `lastAppliedRefreshHz_` →
  `liveHub_->setRateHz(hz)` + `modBar->setTelemetryRateHz(hz)` (both bars).
  Also apply once at editor construction. (This also covers the
  SettingsPanel combo path — no extra callback plumbing required, but pass
  the callback so that the combo updates the processor immediately.)
- Reset hooks (PluginProcessor.cpp): `loadProgramFromBytes`,
  `loadParvatiPatchFile`, `loadMultiFile`, `loadParvatiMultiFile`,
  `onPartSelect`, and the engine-restore epilogue of `setStateInformation`
  each call `engine_.resetUiTelemetry();
  engine_.setUiTelemetryPart(currentPart_);`.
- CHANGELOG.md entry under `[Unreleased] / Added`.

## Tests

- `tests/ui_telemetry_test.cpp` (engine task; target
  `parvati_ui_telemetry_test` registered in CMakeLists): drives the engine
  headless (copy the controller_mod_test harness idioms) and asserts —
  ALWAYS-ON contract (2026-08-21, replacing the old "populates only while a
  note sounds / frozen after release" semantics): history populates from a
  ZERO BUFFER before any note and keeps scrolling forever; per-voice
  generators (LFO/ENV/...) fall to ZERO on release and STAY zero while idle
  (their actual state); persisted controllers (WHEEL/WHEEL_2/EXPRESSION —
  the voice tables handleController writes them into, sounding AND idle —
  plus PITCH_BEND via lastModSources_) show their live value while idle;
  constants keep their literal values; `resetUiTelemetry` invalidates
  (epoch bump) and the next render repopulates; part switch clears;
  envelope stage walks ATTACK→DECAY→SUSTAIN while a key is held with a
  long-ish attack and RELEASE→DEAD after note-off; `effCutoff` follows the
  base cutoff with no modulation and departs from it with env-2→cutoff
  modulation active; `readUiTelemetry` is false immediately after a reset
  (stale epoch). Section [8] pins the four contract corners end-to-end
  (zero start / animate while held / fall-to-zero + stay / idle wheel
  visible).
  - Classification lives ONCE in ModTelemetryTypes.h
    (`telemetrySourcePersistsWhenIdle` / `telemetryConstantByte` /
    `telemetryIdleRow`): PITCH_BEND/WHEEL/WHEEL_2/EXPRESSION persist,
    CONSTANT_* are literals, everything else is zero when idle.
  - `voiceActive` in the snapshot stays TRUTHFUL (drives the
    envelope-marker and filter-overlay hiding) — only the history appends
    became unconditional. The FX tail-modulation use of lastModSources_ is
    untouched.
- editor_test.cpp additions (verify task): bar telemetry generation
  increments when fed a synthetic moving snapshot and NOT when the snapshot
  is static; `clearTelemetry` hides strips; EnvelopeDisplay marker
  visibility/x monotonic movement; FilterResponseDisplay live-curve
  visibility threshold; settings pref round-trip + clamp.

## Performance rules (all tasks)

- No per-pill timers; no allocations in the audio-thread write path; bounded
  repaint RECTS (never whole-component repaint storms); eps change gates
  everywhere; every poll timer gated on `isShowing()` with the dual-hook
  pattern; the refresh-rate setting caps the animation cadence end-to-end.

# Live Modulation Feedback (Pigments-style) — Design & Contract

Status: implemented (2026-08-21), reviewed and fix-hardened. This document was
the BINDING CONTRACT for the implementation; the deviations below are recorded
per its own rule.

## Recorded deviations (post-review)

- **kNumSources = 32 vs MOD_SRC_LAST = 31.** The header comment originally
  claimed equality; the enum's sentinel value is 31 (31 named sources), so the
  engine asserts `kNumSources >= MOD_SRC_LAST` and the spare slot 31 stays
  zero (the UI only enumerates real enums).
- **Filter live-overlay activity is TEMPORAL, not spatial** (revised from the
  original "live vs base >= 2 bytes" gate). The effective cutoff byte includes
  key tracking (~2 bytes/semitone), so a spatial threshold trips for every
  held note on any patch with tracking — an always-on second curve for a
  static patch setting. The overlay now shows while the effective bytes MOVE
  (>= 1 byte/tick) and holds ~270 ms after the last movement; a settled note
  returns to the single opaque base preview. The base tick still dims to 0.30
  while the overlay is visible.
- **The two preview polls stay at 30 Hz** (the architecture diagram's numbers)
  while the refresh setting caps the engine→UI data cadence and the bar strip
  animation. The previews' repaints are change-gated off the hub cache, so
  their EFFECTIVE animation cadence is capped by the setting end-to-end; only
  the (cheap, gated) poll wakeups stay at 30 Hz. The hub itself is additionally
  stopped by the editor's visibilityChanged (a hidden editor polls nothing).
- **The engine's ring head lives IN the frame** (`historyHead`, written under
  the same seqlock critical sections) rather than as a separate plain member —
  the reader's copy is always self-consistent. The linearized UI frame zeroes
  it.
- **Strip diff-gate signature includes a position-weighted moment** (Σ j·v[j])
  on top of count/first/last/min/max, so a pulse sliding through an otherwise
  static window (GATE / VELOCITY / ARP) still animates.

## Goal

A Pigments-like live feedback system:

1. **Mod pills show a history sparkline** — every CentralModBar pill (except the
   Const cluster and the bar-only Note Sequencer sentinel) draws a subtle,
   themed, animated strip of the RECENT values its modulation source produced
   (LFO waveform, envelope shape, aftertouch motion, velocity hits...). Visible
   but not distracting; fits the flat quiet-chrome pill style.
2. **Envelope previews show the live stage** — while a key is held, the ADSR
   EnvelopeDisplay draws a position marker (dot + hairline) moving through
   Attack / Decay / Sustain / Release, using the engine's real envelope stage +
   progress.
3. **Filter preview reflects live modulation** — when the filter cutoff /
   resonance is ACTIVELY being modulated (env-2 sweep, LFO, matrix, wheel...),
   the FilterResponseDisplay draws the live effective curve + a moving cutoff
   tick, while the static base preview stays fully opaque in place.
4. **Reset semantics** — history, envelope markers and the live filter curve
   reset (hide / clear) on patch load / patch switch / part switch / init.
5. **Performance-first** — one shared engine snapshot per tick (seqlock, single
   audio-thread writer), per-component change-gated bounded repaints, a
   user-tunable refresh rate (10/15/30/60 Hz, persisted), and full
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
                                              live curve (change-gated)
```

- The engine NEVER allocates on the audio thread for this; writes are bounded,
  fixed-size, seqlock-guarded.
- The UI NEVER blocks: `readUiTelemetry` is a bounded-retry copy; on a torn read
  it returns false and the hub simply keeps the previous cache for one tick.

## Shared types — `Source/ui/ModTelemetryTypes.h` (WRITTEN, do not modify)

`parvati::ModTelemetrySnapshot` (trivially copyable, dependency-light):

- `kNumSources = 32`, `kHistoryLen = 128` (engine static_asserts
  `kNumSources == ambika::dsp::MOD_SRC_LAST`).
- `epoch` (uint32) — bumped by the message thread on every reset; a snapshot
  whose `epoch` != the engine's authoritative epoch is INVALID.
- `part` (int) — the part this telemetry describes.
- `sources[32]` — CURRENT effective mod-source values, 0..255.
- `history[32][128]` + `historyCount` — OLDEST→NEWEST per-source ring,
  linearized by `readUiTelemetry` (storage uses `historyHead` internally).
- `envStage[3]` (0..4 ATTACK/DECAY/SUSTAIN/RELEASE/DEAD), `envProgress[3]`
  (0..1 within stage), `envLevel[3]` (0..1 current output) — the representative
  (most-recently-triggered active) voice's envelopes 1..3.
- `effCutoff`, `effResonance` (uint16 0..255 — effective, modulation-applied),
  `filterMode` (0..3), `voiceActive`.

`parvati::LiveEnvStage` and `parvati::LiveFilterValues` — the small structs the
display components take (avoids cross-includes between hub and components).

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
    sources (voiceActive=false), copy the CURRENT authoritative epoch into the
    snapshot, reset the decimator.
  - Append history ONLY while the tracked part has an active representative
    voice, decimated every 12th internal block (~81.7 Hz at the 980 Hz internal
    cadence) → 128 samples ≈ 1.57 s window.
  - Once per `renderPartFx` for the tracked part (with an active rep voice, or
    on the active→inactive transition — never a steady idle write): update
    `sources[]`, envelope stage/progress/level, effective cutoff/resonance/
    mode, voiceActive — all under the seqlock.
- Seqlock: `uiTelSeq_` (single writer = audio thread; single reader = message
  thread). Writer: odd-begin / release-fence / write / release-fence /
  even-end. Reader: bounded (64) retry, acquire loads — same discipline as
  `Part::readPendingConfig`.
- Epoch validity: `resetUiTelemetry()` bumps an MT-authoritative
  `uiTelemetryEpoch_`; `readUiTelemetry` returns false when the snapshot's
  epoch is stale, so a reset is visible to the UI IMMEDIATELY (before the audio
  thread services the clear).
- Envelope progress: `phase() / 65536.0f` when `phase_increment() > 0`, else 1.0
  (SUSTAIN/DEAD hold). Needs new const accessors in `dsp/envelope.h`
  (`phase()`, `phase_increment()`, `value_byte()`), a const `envelope(i)`
  accessor on `dsp::Voice`, and AmbikaVoice wrappers
  (`envelopeStage/Phase/PhaseIncrement/ValueByte(i)`, `effectiveCutoff()`,
  `effectiveResonance()`, `filterMode()`).
- Bipolarity for display (mirrors the voice mod-matrix AC coupling):
  bipolar sources = `MOD_SRC_LFO_1..4`, `MOD_SRC_PITCH_BEND`, `MOD_SRC_NOTE`;
  every other source is unipolar (0 = floor). UI draws bipolar around a
  midline. (Informational constant lives in ModTelemetryTypes.h:
  `parvati::isBipolarModSource(int)`.)

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
  `visibilityChanged` + `parentHierarchyChanged` → start/stop). One timer for
  the whole bar (never per pill).
- Per tick: fetch snapshot (invalid → clearTelemetry, stop repainting). For
  each pill with `enumValue_ >= 0 && cluster_ != Cluster::Const`: downsample
  `history[enum]` to ≤ 24 points, compare against the last-drawn signature
  (eps 1/255); changed → update the pill's cached strip points and
  `pill->repaint(stripRect)` (BOUNDED dirty rect — this is the GPU-cost
  control). Idle sources cost nothing.
- Pill paint: a history STRIP band at the bottom of the pill
  (`kStripH ≈ 13px`, above the family underline), stroke ≈ 1.25px round-joined
  polyline in the family accent at LOW alpha (0.45 inactive / 0.6 active), no
  fill. Bipolar sources centre on the strip midline; unipolar fill from the
  strip bottom. The label moves up into the remaining pill top (label area =
  pill height − strip). Geometry otherwise unchanged. When `historyCount == 0`
  the strip paints nothing.
- Const cluster pills and the Note Sequencer sentinel never draw a strip.

## EnvelopeDisplay contract (previews task)

- New API:
  ```cpp
  void setLiveStageProvider (std::function<parvati::LiveEnvStage()> p);
  // TEST-ONLY:
  bool  liveMarkerVisibleForTest() const noexcept;
  float liveMarkerXForTest() const noexcept;   // normalized 0..1
  ```
- Refactor the ADSR segment math so the marker and the curve share ONE
  definition (a static `adsrSegmentSpans(a,d,s,r,&wA,&wD,&wS,&wR)` helper; the
  existing `adsrCurveLevel` keeps its exact shape).
- Marker (previewMode 0 only, provider present, `active && stage != DEAD`):
  x = segment start + progress × segment width (fraction of total), using the
  SAME segment proportions as the drawn curve; draw a 3.5px filled dot ON the
  curve at (x, adsrCurveLevel(x)) in the trace colour, plus a 1px vertical
  hairline (trace @ 0.28 alpha) through the plot. SUSTAIN pins progress=0 →
  the dot rests at the plateau start.
- Repaint gating: the existing eps param-change gate is EXTENDED — also repaint
  when the marker x moves > eps or the active flag flips. No provider / LFO
  mode → zero overhead.

## FilterResponseDisplay contract (previews task)

- New API:
  ```cpp
  void setLiveValuesProvider (std::function<parvati::LiveFilterValues()> p);
  // TEST-ONLY:
  bool  liveCurveVisibleForTest() const noexcept;
  float liveCutoffXForTest() const noexcept;   // normalized 0..1
  ```
- `LiveFilterValues.cutoff01/reso01` are the EFFECTIVE bytes normalized to
  0..1 of the 0..255 domain (same domain as the base curve's
  `roundToInt(cN * 255)`).
- Paint: the BASE curve stays exactly as today (opaque, in place). When the
  provider is active AND the live bytes differ from the base bytes by ≥ 2
  (either axis): additionally draw the LIVE curve (same magnitude model, live
  fc/K) at higher weight (≈1.75px, full trace alpha, no fill) + a bright live
  cutoff tick; the base tick dims to ~0.30 alpha while modulation is active.
  Below the threshold → single base curve only (no duplicate strokes).
- Repaint gating: extend the existing eps gate with the live-vs-base and
  live-vs-lastLive deltas.

## Settings + persistence (settings task)

- `ParvatiAudioProcessor`: `int getUiRefreshHz() const` /
  `void setUiRefreshHz(int)` (clamp 5..60, default 30), guarded by
  `uiPrefsLock_`, persisted as `"ui_refresh_hz"` in getStateInformation /
  setStateInformation next to the other ui_* props (legacy states default 30).
- `SettingsPanel`: a "Visual Refresh" (TRANS) label + combo with items
  10 / 15 / 30 / 60 Hz (IDs = Hz, 30 default; labelled e.g. "30 Hz (Default)"),
  new ctor callback `onRefreshChanged(int)` fired after
  `proc.setUiRefreshHz`. Row layout follows the existing pattern.

## Editor wiring (wiring task — AFTER the above)

- Editor owns `std::unique_ptr<parvati::LiveFeedbackHub> liveHub_`
  (Source/ui/LiveFeedbackHub.{h,cpp} — WRITTEN, do not modify) constructed
  with a fetcher bound to `engine.readUiTelemetry`.
- Wire `setTelemetryProvider` on BOTH workspace bars (SynthWorkspace::modBar_
  and FxWorkspace::modBar_ — add a public `CentralModBar* modBar()` accessor to
  each workspace if missing).
- Wire `setLiveStageProvider` on the three ADSR EnvelopeDisplays (Env 1/2/3)
  and `setLiveValuesProvider` on the FilterResponseDisplay, via the hub cache.
- Apply the refresh rate: in `ParvatiEditor::timerCallback`, when
  `processor.getUiRefreshHz()` differs from `lastAppliedRefreshHz_` →
  `liveHub_->setRateHz(hz)` + `modBar->setTelemetryRateHz(hz)` (both bars).
  Also apply once at editor construction. (This also covers the SettingsPanel
  combo path — no extra callback plumbing required, but pass the callback so
  the combo updates the processor immediately.)
- Reset hooks (PluginProcessor.cpp): `loadProgramFromBytes`,
  `loadParvatiPatchFile`, `loadMultiFile`, `loadParvatiMultiFile`,
  `onPartSelect`, and the engine-restore epilogue of `setStateInformation`
  each call `engine_.resetUiTelemetry(); engine_.setUiTelemetryPart(currentPart_);`.
- CHANGELOG.md entry under `[Unreleased] / Added`.

## Tests

- `tests/ui_telemetry_test.cpp` (engine task; target `parvati_ui_telemetry_test`
  registered in CMakeLists): drives the engine headless (copy the
  controller_mod_test harness idioms) and asserts: history populates while a
  note sounds + stays frozen after release; `resetUiTelemetry` invalidates
  (epoch bump) and the next render repopulates; part switch clears; envelope
  stage walks ATTACK→DECAY→SUSTAIN while a key is held with a long-ish attack
  and RELEASE→DEAD after note-off; `effCutoff` follows the base cutoff with no
  modulation and departs from it with env-2→cutoff modulation active;
  `readUiTelemetry` is false immediately after a reset (stale epoch).
- editor_test.cpp additions (verify task): bar telemetry generation increments
  when fed a synthetic moving snapshot and NOT when the snapshot is static;
  `clearTelemetry` hides strips; EnvelopeDisplay marker visibility/x monotonic
  movement; FilterResponseDisplay live-curve visibility threshold; settings
  pref round-trip + clamp.

## Performance rules (all tasks)

- No per-pill timers; no allocations in the audio-thread write path; bounded
  repaint RECTS (never whole-component repaint storms); eps change gates
  everywhere; every poll timer gated on `isShowing()` with the dual-hook
  pattern; the refresh-rate setting caps the animation cadence end-to-end.

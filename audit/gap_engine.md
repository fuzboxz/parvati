# Test-Coverage Gap Audit — engine + processor glue

Scope: `Source/SynthEngine.{h,cpp}`, `AmbikaVoice.{h,cpp}`, `PluginProcessor.{h,cpp}`, `MidiParameterMap.{h,cpp}`, `NoteStack.h`, `TransportClock.h`, `Arpeggiator.{h,cpp}`, `Sequencer.{h,cpp}`. Method: grep of `tests/` per symbol + read of the cited seams; `git log -8` (latest: QoL wave `542ada3`).

## Prioritized missing tests (max 6)

1. **Oversized-block MIDI rebasing across slices** — unit/processor. `PluginProcessor.cpp:583-600` rebases later-slice events to `[0,n)` via `sliceMidiScratch_`; only slice-0 delivery is pinned (`render_quality_test.cpp` [2] sends note-on at sample 0). Pin: prepare @256, render 1024 with note-on at sample 600 (slice 3) → note must sound starting ~sample 600 (rebased pos 88), and a note-on at exactly `done` boundary fires in the right slice; no event leaks into an earlier slice. Target: extend `tests/render_quality_test.cpp` [2]. Effort **M**.

2. **Tail-cache invalidation on tempo move** — engine/processor. `SynthEngine.cpp:1832-1841` recomputes `tailSecondsCache_` when |Δbpm|>0.25; the pure table is pinned at fixed BPMs (`render_quality_test.cpp` [3b]), never the dynamic invalidation. Pin: ClockedDelay enabled (fb>0), playhead 120 BPM → tail X; move playhead to 480 BPM, render one block → tail ≈ X/2 (and ≤0.25 BPM jitter does NOT recompute). Target: new [3e] in `tests/render_quality_test.cpp` (needs a `FakePlayHead`). Effort **M**.

3. **Retired-oversampling reaper capacity + fallback** — voice/engine. `AmbikaVoice.cpp:216-235` parks ≤2 (`kRetiredOsCap`), deletes on overflow; `SynthEngine::reapRetiredAudioObjects()` (`SynthEngine.cpp:448-462`, fed by the 60 Hz timer, `PluginProcessor.cpp:141`) frees them. Only exercised nondeterministically by `concurrency_test.cpp:247`. Pin deterministically: 3 OS flips without reaping → 3rd park falls back; reaper clears all slots; repeated flip+reap cycles don't grow. Needs a small test-only counter (e.g. `debugRetiredOsCount()`) since `retiredOs_` is private. Target: new `tests/os_reaper_test.cpp`. Effort **L** (hook + staging).

4. **NoteStack ordering/saturation semantics** — unit. Zero direct tests (no `tests/` file mentions `NoteStack`; only indirect via arp engine tests). Pin vs firmware: `sorted_note(i)` pitch ordering, `played_note(i)` LIFO order, `most_recent_note`, saturation evicting the least-recently-played at capacity 12, re-noteOn dedup (no duplicate/size inflation), `contains`. This class caused a past hosted SIGBUS (see header comment). Target: new `tests/note_stack_test.cpp`. Effort **S**.

5. **TransportClock tempo math + clamps** — unit. No direct tests. Pin exact values: samplesPerTick = sr·60/(bpm·24) → 100.0 @48k/120; `advance()` fractional carry is drift-free across e.g. 3-sample calls at 480 BPM (ticks sum equals sr·60/(bpm·24)⁻¹ over 10⁶ samples); bpm>999 clamps (advance never spins >numSamples); bpm<=0 keeps the previous rate; `reset()` zeroes phase. Target: new `tests/transport_clock_test.cpp`. Effort **S**.

6. **Master DC blocker slice/state continuity** — processor. `PluginProcessor.cpp:643-656` applies the 15 Hz high-pass per slice; only conflated coverage via `idle_silence_test.cpp` (idle-voice gate + DC blocker together, no MIDI). Pin: a sustained low note's main-bus output in one 1024 oversized block vs four 256 in-budget blocks is near-identical across the slice boundary (no step/click from per-slice filter state) and near-DC offset is attenuated ≥20 dB vs raw voicecard sum. Target: extend `tests/render_quality_test.cpp` [2]. Effort **M**.

## Already covered (one-liners)

- Offline OS boost/restore, double-entry, prepare-time leak guard, pref non-persistence: `render_quality_test.cpp` [1].
- Tail pure table (reverbs/delays/freeze/clamps/NaN) + processor cache (floor/enable gating): `render_quality_test.cpp` [3a-3d].
- Oversized-block chunked render tail (slice-0 MIDI only): `render_quality_test.cpp` [2].
- OS pref clamping/round-trips: `host_state_test.cpp` [6]; OS flips under TSAN: `concurrency_test.cpp:247`.
- Display-version mirror healing + restoreState bump: `ui_mirror_test.cpp` (canaries A/B, line 525).
- Arp prescaler gating, seq lifecycle/self-clean, engine no-stuck-note, chord release, clamped raw bytes: `arp_test.cpp`, `hellcat_arp_seq_timing_test.cpp`.
- NRPN direct/stride/signed/unmapped + CC map end-to-end: `midi_param_test.cpp`; sustain/CC123/CC routing: `cc_routing_test.cpp`.
- Sequencer note seq + mod seqs: `sequencer_test.cpp`; voice slots/allocation: `voice_slots_test.cpp` et al.

## Residual notes

- `MidiParameterMap` CC96/97 data increment/decrement (`nudgeValue`, `MidiParameterMap.cpp:356-371`) is also unpinned — cheap add to `midi_param_test.cpp` if a 7th slot opens.
- Latency re-report via `latencyDirty_` (`PluginProcessor.cpp:478-484`) is only hit through the offline path; a live editor OS change mid-stream shares the same code (low marginal risk).

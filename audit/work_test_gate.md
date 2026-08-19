# TEST-WAVE FINAL GATE

## 1. Slice-0 MIDI drain — FIXED

**Design.** `PluginProcessor.cpp` oversized-block tiling handed slice 0 the full host `MidiBuffer` ("byte-identical old path"), relying on `Synthesiser::processNextBlock` honouring `numSamples` — but its closing `std::for_each` (`juce_Synthesiser.cpp:232-235`, verified in `~/JUCE`) drains every remaining event of the handed buffer. Out-of-window events therefore fired early at slice 0's start AND re-fired in their home slice (double-fire). Fix: a `tileMidi` flag (`prepared > 0 && totalSamples > prepared`, computed once before the loop) makes **every** slice — slice 0 included — copy only its window `[done, done+n)` into `sliceMidiScratch_` rebased to `[0,n)` (the exact filtering later slices already used). A single-slice block keeps the byte-identical fast path (host buffer handed as-is). Verified `processTransport` swaps in `processedMidi_` beforehand (direct notes at host positions + arp/seq-generated events within `[0,totalSamples)`), so both event classes window-filter correctly. Comment block rewritten to document the JUCE drain requirement.

**Un-pinned [2b]:** all three KNOWN-BUG markers → hard `check`s: mid-slice @600 onset must be in (600,780]; no audio before the event position; boundary @768 onset ≥768. `reportKnownBug`/`g_knownBugs` machinery removed (now unused). Measured: onset 610 (was 266), boundary 778 (was 266). Test cases kept.

## 2. ParamHelp.h contract comment
"120 curated / 184 parameters" → **198 curated + 64 generated = 262** (both header mentions; notes the loop-generated slot/fxmod families).

## 3. Full suite — 116 PASS / 1 FAIL

`cmake --build build -j8`: 0 errors. Then every `parvati_*test` + `parvati_tests` from repo root (timeout 300; screen/menu-shots skipped, no display). 117 ran, **116 PASS**. All 12 new targets present + green: fixed_math, transient_generator, envelope, sub_oscillator, osc_sync, voice_pitch, note_stack, transport_clock, os_reaper, paramhelp_parity, translations, note_step_control. Extensions all green: render_quality (0 failures, no known bugs), fx_param_coverage (479/479), fx_routing, clouds_fx, fv1_clocked_delay, midi_param [9], synth_paramtext, host_param_text [5], editor/multigui, host_state, voice_slots (×5 stable).

**The one failure is the documented pre-existing one:** `parvati_fx_crackle_diag_test` SIGBUS rc=138 — re-verified at exactly the documented ~2/5 rate (runs: 0,0,138,138,0); proven pre-existing on clean HEAD by the prior gate (stash→rebuild→pop). Diagnostic-only binary. Not fixed, per instructions.

## 4. CHANGELOG
Two entries at top of [Unreleased]: new **Fixed** section (slice-0 MIDI drain: early-fire + double-fire → per-slice windowing, onset numbers, test provenance) and **Changed** (test wave: all 17 targets with one-line purposes, the extensions list, 78 FX ParamHelp strings + loop-generated fxmod, `debugRetiredOsCount` accessor, NoteStepControl statics made public, suite verdict, crackle-diag pre-existing note).

## Residual notes
- Behavior deltas pinned during the wave (tests document, not change, code): U8AddClip wraps before compare (audit's "wrap→max" was wrong); envelope peaks at 253 not 254; CLICK bursts 32 samples not 31; `mix_balance ≥ 128` reads as osc1-only.
- `osc_sync` voice-level pin depends on current LUT values (preconditions asserted in-test so a table regen fails loudly).
- Nothing staged; `presets/` untouched this gate.

## Changed files (this gate)
`Source/PluginProcessor.cpp` (MIDI fix + comments), `Source/ui/ParamHelp.h` (comment), `tests/render_quality_test.cpp` (un-pin + cleanup), `CHANGELOG.md`. Lane files from the four test lanes remain in-tree, unstaged.

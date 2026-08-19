# FX + render-path test-gap fill — work report (tests_fx)

## Item 1 — render_quality_test [3]: tail table + cache
- **WSOLAStretch freeze**: `param[3]>0.5 → kTailCapSeconds`, no-freeze → 4.0 (was unpinned; LoopingDelay p3 / Spectral p4 already were).
- **Zero-tail family**: all 14 memoryless/short types (PitchShifter…Flanger) loop-asserted `== 0.0` exactly.
- **Multi-part MAX**: part 0 Room@3 s + part 1 Plate@4.1 s via engine `setCurrentPart`/`setFxSlot*` (the APVTS view is part-0 only) → cache reports **4.1**; disabling part 1 falls back to 3.0. Never sum, never part-0-only.
- **[3e] tempo-move (new)**: FakePlayHead (settable bpm). ClockedDelay div-1 + fb 63: 120 BPM → 9.1816 s; 480 BPM → 4.5908 s (ratio 0.5000, ±1 ms); **+0.2 BPM jitter → bit-equal cache** (`juce::exactlyEqual` — a recompute would move it); +1.0 BPM → strictly smaller.

## Item 2 — [2b] MIDI rebase across slices → **REAL BUG FOUND**
Fresh processor, note-on @600 in a 1024 block (prepared 256): onset measured at **266** (= slice-0 start + attack/latency), not ~(600…780). Root cause verified in JUCE source: `Synthesiser::processNextBlock`'s closing `std::for_each` (**juce_Synthesiser.cpp:233-235**) drains every remaining event of the handed buffer — and `PluginProcessor.cpp` hands slice 0 the **unfiltered** host MidiBuffer ("byte-identical old path"). Out-of-window events fire in slice 0 *and re-fire in their home slice* → timing corruption in oversized/offline renders. Same for a note-on @768 (boundary): onset 266.
Fix is a Source/ change (slice 0 must window-filter when `done+n < totalSamples`) — outside this lane's file ownership, so pinned via the repo's `reportDrift`-style **KNOWN-BUG** marker (3 pins: mid-slice window, boundary window, no-early-audio; suite stays green; the Source fix flips them to failing and prompts un-pinning). Hard checks that pass today: note never dropped (sounding after its window).

## Item 3 — [2c] DC blocker (new)
- **Slice continuity**: sustained low note via 4×256 in-budget blocks vs one 1024 oversized block → **max |diff| = 0.000e+00** (bit-identical; per-slice filter state contiguous).
- **DC attenuation**: low-cutoff patch (cutoff 10, static env) accumulating block-by-block raw voicecard sum + main bus over 65 536 samples (~89 note periods so LF ripple averages out): raw mean −1.90e-2, main 2.22e-4 → **~32.6 dB ≥ 20 dB**. (A 256-sample window was useless — partial-period ripple, not DC.)

## Item 4 — fx_param_coverage_test: chain latency
Series OS+OS+OS = **24**; OS+0+OS = 16; OS+0+None = 8; Parallel12to3 max(A,B)+C = 8 (both branch placements); Parallel1to23 A+max(B,C) = 8. **N1**: `latency()` unchanged across disable/re-enable. **N2**: after bypass + ~2.7 s fade-out (500 zero blocks), an impulse exits the bypassed OS slot **exactly at sample 8** (pure delay: nothing before/after). 479/479 green.

## Item 5 — fx_routing_test
- **B8**: Echo/Plate/Spring through BOTH parallel topologies: finite + wet (the topology sweep only used Clouds ports).
- **B9 bypass continuity**: bypassing one Parallel12to3 branch mid-stream: rms 0.0446 → 0.0446 (**ratio 1.000** — no +6 dB activeCount snap); still-audible while fading; stay-vs-bypassed twin-chain divergence grows 1.48e-2 → 1.44 over 40 blocks (fade decay pinned directly; the single-chain energy is non-monotonic — the surviving echo fills).
- **B7b rate change**: 48 k warm-up → **staged** Echo swap + re-prepare @96 k → wet on first block (staged swap applied at prepare); re-prepare @44.1 k → still wet, finite (bridge re-armed, no stale-rate dropout).

## Item 6 — parvati_clouds_fx_test: ≤32 kHz branch
Diffuser/CVerb/LoopingDelay @ {22050, 32000} (ratio>1 upsample, aaActive_ off): finite, non-silent, differs from dry, **and waveform differs from the 48 k render** (bridge genuinely resamples differently). Surprise: my first all-0.5-params probe starved CVerb (100 ms predelay) and LoopingDelay (burst tail silent + position 0.5) — *not* a bug; final config mirrors the proven per-effect sections (continuous tone for the looper).

## Item 7 — chain setTempo
`fv1_clocked_delay_test` is deliberately JUCE-free (no Parvati link; CMake edits out of scope) → the chain seam landed in **fx_routing_test T1/T2**: (T1) `chain.setTempo` drives the 1/16 ClockedDelay echo **3003 @240 vs 6003 @120 samples (ratio exactly 2.0)** — required an install-block first (staged processors install at the first `process()`; `setTempo` fans to `slots_`) and a ~0.25 s glide-settle (delay retarget glides ≤0.25 sample/internal-sample, else the echo lands mid-glide — measured 4803). (T2) `setTempo` on Diffuser+Plate chains at 120 vs 187.5 BPM: **bit-identical** output. In fv1_clocked_delay_test: added the DSP-side `setTransport(0.0)` guard pin (echo stays at 4003 = the 240-BPM position, no snap to the 120 default).

## Validation
`cmake --build build --target <all 5> -j8`: 0 errors. Runs: render_quality **ALL CHECKS PASSED (0 failures, 3 known bugs)**; fx_param_coverage **479/479**; fx_routing **ALL CHECKS PASSED** (66 ok); clouds_fx **ALL CHECKS PASSED**; fv1_clocked_delay **ALL CHECKS PASSED**. No Source/ or CMake edits. Other modified files in the tree belong to sibling lanes.

## Top finding for the parent
**Slice-0 MIDI drain bug** (`PluginProcessor.cpp` oversized-block path): out-of-window events fire early and double-fire. Pinned as KNOWN-BUG in render_quality_test [2b]; needs a Source fix (window-filter slice 0 too), then un-pin.

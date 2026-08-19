All verification complete. Here is the review report:

---

# FX DSP Review — batch "Clouds time-domain (adapters)"

**Scope**: `Source/dsp/fx/FxProcessors.{h,cpp}` (FxDiffuser, FxPitchShifter, FxLoopingDelay, FxWSOLAStretch adapter) + vendored cores `clouds/dsp/fx/diffuser.h`, `fx/pitch_shifter.h`, `looping_sample_player.h`, `wsola_sample_player.h` (+ supporting `fx_engine.h`, `audio_buffer.h`, `window.h`, `correlator.{h,cc}`, `HostRateBridge.h` as rate-assumption context). Read-only; all arithmetic below was computed, not eyeballed.

---

## 1. FxDiffuser — verdict: CLEAN

- **[OK-PINNED] Delay-line layout fits the tank.** `diffuser.h:56-65` reserves 126+180+269+444 (L, base 0..1022) and 151+205+245+405 (R, base 1023..2031); total 2032 ≤ 2048 (`FxEngine<2048,FORMAT_32_BIT>`, MASK 2047). L uses lines 0–3, R lines 4–7 at the shared per-sample `write_ptr_` — no cross-channel aliasing. Allpass coefficient 0.625 is lossless (|H|=1): no DC accumulation, bounded for |x|≤1; float storage = zero quantization error.
- **[OK-PINNED] Full-wet pin.** `FxProcessors.cpp:51-55` sets `amount_=1.0`; per-sample `in_out->l += 1.0f*(wet−in_out->l)` collapses to `wet` exactly; `previous_amount_` tracks after the first block, so `ParameterInterpolator` is a no-op thereafter.
- **[NIT] m==0 division.** At host rates >32 kHz, `renderPartFx` 1-sample sub-chunks (`SynthEngine.cpp:2262`, `if (sub<=0) sub=1`) periodically yield `m==0` (at 44.1 kHz ≈38% of 1-sample calls: carry 0→0.378→0.756→1.134⇒m=0). `Diffuser::Process(scr,0)` constructs `ParameterInterpolator(...,0)` → `increment_ = 0/0 = NaN` (amount converged) — but the loop never runs and the destructor writes `*state_ = value_` (unchanged copy), so the NaN is discarded. No audio effect unless FP traps are enabled (non-default). Same for `pitch_shifter.h:64` (`0.05f/0.0f = +inf`, discarded).
- **[OK-PINNED] Lifecycle/tail.** `prepare`/`reset` re-`Init` → `FxEngine::Clear()` zeroes tank + write_ptr (FxProcessors.cpp:30,38); bridge phases reset. Tail table `2048/32000 = 64 ms` (FxTypes.h:289) ≥ longest path (L chain 1019 samples + smear). Latency 0 pinned by test.
- **Test suggestions**: (exists) continuity/diag/tail tests cover it; add a direct 1-sample-block loop at 44.1 kHz asserting finite output (deterministically exercises the m==0 path).

## 2. FxPitchShifter — verdict: FINDINGS

- **[RISK] Glide time is block-size dependent** — `pitch_shifter.h:64`: `size_coeff = 0.05f/size` where `size = m` (internal samples per call). Time constant τ = 20·m internal samples: 20 ms at m=32 (engine ~980 Hz sub-chunk path), ≈58 ms at m=93 (128@44.1k), ≈320 ms at m=512. Unlike FxLoopingDelay/FxWSOLAStretch (`FxProcessors.cpp:241,312` chunk at ≤32), this adapter does **not** chunk (`FxProcessors.cpp:84-86`), so the "~20 ms" comment (`FxProcessors.cpp:78`) only holds on the sub-chunked engine path; direct/test calls at larger blocks glide 3–16× slower. Fix approach: chunk internally at ≤32 like the looper, or use a fixed per-internal-sample coefficient.
- **[NIT] Spread is periodic: param 1.0 ≡ 0.0.** `pitch_shifter.h:126-127`: `phaseR_ = phase_ + offR_`; at `spread→1`, `offR_→1` wraps to `phaseR_ ≡ phase_` → R output bit-identical to L (mono), duplicating spread=0. Max decorrelation is at 0.5; the knob's top half folds back toward mono. Suggest mapping 0..1 → offR_ 0..0.5 or documenting.
- **[OK-PINNED] The spread crossfade invariant holds.** `triR` is zero exactly at `phaseR_`'s own wrap points ({0, 0.5}); `halfR = phaseR + size_/2` wraps only under `1−triR = 0` — same construction as the L taps; `offR_=0` collapses to the upstream mono path algebraically. The documented crackle fix is sound.
- **[OK-PINNED] Extremes.** ratio ∈ [0.5, 2] → phase delta ≤ 1/128 (single-step wrap safe); `size_` ∈ [128, 2047] (cubic map); 16-bit `Compress` clips ±1, gains `tri+(1−tri)=1` → bounded; `SemitonesToRatio(±24)` LUT indices ∈ [104,152]×[0,255] in 257-entry tables. Latency 0 / tail floor 0.2 s both cover the ≤64 ms window delay.
- **Test suggestions**: spread≈0.4 continuity + L≠R decorrelation test (the phaseR_ fix has no direct pin); a spread=1≡spread=0 equivalence pin; size-step settle-time vs block size.

## 3. FxLoopingDelay — verdict: FINDINGS (inherited)

- **[OK-PINNED] All read indices in bounds, computed.** size_=128000, physical 128008 (tail 8), Hermite reads ≤ size_+2. Non-freeze (`looping_sample_player.h:99`): min integral = (0−4−31+128000) − max_delay 127936 = **29 ≥ 0**. Freeze (`:124,127`): the wrap-time constrain makes `loop_point_+loop_duration_ ≤ max_delay` as a same-parameter pair → min integral **60 ≥ 0**, max < 2·size_ so the single `if (integral >= size_) integral -= size_;` suffices; `phase_` overshoot ≤ one increment (≤8 at +24 st) only adds. The `frac16FromQ12` patch (`:53`) removes the upstream `x<<4` signed-overflow UB — verified bit-equivalent on the low 16 bits.
- **[OK-PINNED] Freeze write-gate & chunking.** `WriteFade(write=false)` fills `tail_[256]` without advancing head; resume crossfades over 256 samples — firmware semantics preserved. ≤32-sample chunks keep the WriteFade fast-path guard (`write_head_ < size_−size`) valid and the tail mirror maintained. Re-prepare/reset re-`Init` both buffers (tape cleared) and looper state.
- **[RISK] Freeze-toggle wet discontinuity (upstream-inherited).** Entering freeze: the loop branch's first sample has `gain = phase_/tail_duration_ ≈ 0` (`looping_sample_player.h:153`) — wet jumps from the full-amplitude delay-follow signal to ~0, fading in over 64 internal samples; leaving freeze jumps back to the live delay read with no fade. Wet amplitude step ≈ current signal level at high wet mix. Clouds hardware behaves identically; flagging as a known click source, not a regression.
- **[NIT] Scale mismatch**: WriteFade clips at ×32767 vs Write/ReadHermite ×32768 (`audio_buffer.h`) — upstream quirk, ~0.003% gain error, no action.
- **[OK-PINNED] Tail table**: 128000/32000 = 4.0 s exact; freeze → 12 s cap by design. Position knob glides via the 0.00005/sample one-pole (τ ≈ 0.63 s) — no stepped clicks.
- **Test suggestions**: freeze engage/release wet-jump measurement (currently untested anywhere); param coverage already pins frozen Size/Pitch.

## 4. FxWSOLAStretch + wsola_sample_player.h — verdict: FINDINGS

- **[RISK] Inline correlator CPU.** `FxProcessors.cpp:316-317` runs `LoadCorrelator + EvaluateSomeCandidates` after **every** ≤32-sample chunk. At default window 2048: `size_` ≈ 1638 sign bits → candidates = 1638/4+16 ≈ 425, each scanning `size_>>5` ≈ 51 words → ~22 K popcount-word iterations per chunk ≈ **22 M/s at 32 kHz internal** (≈0.1 core), unconditional. The firmware amortizes this across background time; correctness is unaffected (search completes in ~4 chunks, matching the `wsola_sample_player.h:96` comment). A perf regression marker would be prudent.
- **[OK-PINNED] Correlator scratch bounds.** `corr_[390]`, destination region [130,390). Destination sign bits = 2·window_size_/(stride·max(ratio,1.25)) ≤ 3277 → ≤103 words written; `EvaluateNextCandidate` reads ≤ `offset_words+num_words+1` ≤ 103 past 130 → max `corr_[233]` — comfortably inside. Negative search positions (`search_target_−window_size_` ≥ −111616 > −128000) normalized by `+buffer->size()`; `ReadLinear`'s single subtract suffices (integral < 2·size_ verified for all spans ≤ 2·window_size_ ≤ 8192).
- **[OK-PINNED] Startup patches verified by arithmetic.** `first_window_` (`wsola_sample_player.h:239-244`): window anchored at buffer position 0 reads sample t at time t+lag while the write head advances in lockstep at ratio 1 — it trails the head by the schedule lag and never reads unwritten memory; the gain ramp fades in real audio (pinned by `tests/parvati_fx_engine_continuity.cpp:143-243`). `Window::Start` `done_=false` (`window.h:73`) is required for any output (activation parity with `Grain::Start`). `sample_index = first_sample_ + phase_integral` ≤ (size_−1)+2·window_size_ < 2·size_ → single wrap-subtract safe.
- **[OK-PINNED] Adapter**: ≤32 chunking, freeze as write-gate only (documented), pitch clamped ±12 st in the smoother, window-size hysteresis (64-sample threshold, %4 quantize) — all upstream-faithful; `limit = size − 2·w·inv_ratio − 2·w ≥ 103424 > 0` at worst-case pitch/size.
- **Test suggestions**: startup regression exists; add a CPU-time-per-chunk marker and a freeze-toggle continuity measurement (shared with the looper).

## Rate assumptions (shared, item 6)

- **[OK-PINNED] Drift-free bridge math.** Persistent fractional phase gives exact long-run 32000 Hz at any host rate (48 k: invRatio 1.5 exact; 44.1 k: 1.378125 not dyadic → float error ~1e-7 ≈ 0.0002 cents — negligible). `m==0` → ZOH hold of `prevTail_` (continuity, no dropout); leading-sample blend via `phaseStart_` makes sub-chunk boundaries seamless.
- **[RISK] (context, bridge not in assigned set)** AA/recon filtering for host>32 k is 2×Svf at 14 kHz ≈ −8.7 dB at 16 k, −14 dB at 20 k — a 20 kHz component aliases to 12 kHz at only −14 dB; upsampling images ≥18 k attenuated ≈ −11 dB. Mild HF tradeoff, documented in `HostRateBridge.h:57-64`.

## Existing tests (grep `tests/`)

Covered: factory/type/latency/finite (`parvati_clouds_fx_test.cpp`), per-param output movement incl. looper frozen Size/Pitch (`fx_param_coverage_test.cpp:974-1035`), full-engine click bounds at 48k/128, 48k/256, 44.1k/256 + WSOLA startup splice (`parvati_fx_engine_continuity.cpp`), tail-table values (`render_quality_test.cpp:377-452`), diffuser sub-chunk diagnostics, param-text mapping. Gaps: the four suggestions above (spread≠0 pin, glide-vs-block-size, freeze-toggle transient, m==0 direct loop).

**No [BUG]-severity findings in the assigned files.**
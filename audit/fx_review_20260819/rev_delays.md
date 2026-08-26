All evidence gathered and arithmetic verified. Compiling the final report.

## Review

**Scope:** `Source/dsp/fx/fv1/Fv1Echo.{h,cpp}`, `Source/dsp/fx/fv1/Fv1ClockedDelay.{h,cpp}`, `Source/dsp/fx/HostRateBridge.h`. Read-only; support files (`Fv1Engine.h`, `FxTypes.h`, `FxChain.*`, `FxProcessors.cpp`, vendored `stmlib/dsp/filter.h`, `tests/`) read for verification only. No blockers found — all findings are [RISK]/[NIT].

- **Correct:** All fixed-point arithmetic, ring-buffer bounds, LUT indexing, tempo-target math, and both rate bridges' drift/m==0 handling verified by computation (details per algorithm below).
- **Fixed:** none (read-only review).
- **Blocker:** none.
- **Note:** Two tail/click [RISK]s in the delay pair and one unfiltered-decimation [RISK] in HostRateBridge; classified with triggers and fix approaches in the report below.

---

# FX DSP Review — "Delays + rate bridge" (audit/fx_review_20260819/rev_delays.md)

## 1. Fv1Echo (`Source/dsp/fx/fv1/Fv1Echo.{h,cpp}`) — verdict: FINDINGS

**[OK-PINNED] Numerics.** `fb14_ = q14(0.995f) = lround(0.995·8191) = 8150` → loop gain 8150/8192 = 0.994873 = −0.0446 dB/repeat (comment's −0.044 dB confirmed, Fv1Echo.cpp:30). Loop gain ≤ 0.994873·|damp| and `OnePoleLpFx` DC gain ≤ 1 (since `q14(a)+q14(1−a) ≤ 8192`, Fv1Engine.h) → stable at every Tone/Feedback corner; DC cannot lock. Ring bounds: `timeL_` max = 0.470·32768 = 15400.96 < 16384; `timeR_` clamped 16383 (Fv1Echo.cpp:27); `readFrac(16383)` reads `read(16383)/read(16384)` — legal for `DelayLine<16384>` (full-wrap slot). Interp gain `q14(1−fr)+q14(fr) ≤ 8192` → unity-bounded (exact 1.0 at fr=0.5).

**[RISK] Click on stepped Time/Spread changes (lens 5).** Fv1Echo.cpp:24–27 sets `timeL_/timeR_` instantly; taps read at the new distance with no glide/crossfade (Fv1Echo.cpp:44–45) → hard read-pointer discontinuity per param update at the chain's ~980 Hz cadence = zipper/clicks on Time/Spread knob turns or FX-mod automation. ClockedDelay's glide comment (Fv1ClockedDelay.cpp:139–152) documents this exact failure mode; Echo lacks the equivalent. Fix: Q.16 one-pole glide on `timeL_/timeR_` mirroring Fv1ClockedDelay.cpp:151–166.

**[RISK] Tail table underestimates 2–3× (lens 4).** The table uses loop period T (FxTypes.h:262–264), but the actual loop period is `timeL_+timeR_ = T·(2+p3)` (tapR→damp→fb→lineL_→timeL_→lineR_→timeR_, Fv1Echo.cpp:44–52). Computed: p0=0 (T=10 ms), p1=0.9 → g=0.8955 → table t60 = 0.01·ln(1e−3)/ln(0.8955) = 0.63 s; true (spread 0) = 1.25 s → a host bounce cuts the tail at ≈ −30 dB. Fix: `T*(2.0+p3)` in the Echo case (FxTypes.h).

**[NIT]** `std::clamp` passes NaN (Fv1Echo.cpp:18–21) → NaN reaches `readFrac`'s `(int)floor` (UB-ish). Unreachable today: `FxChain::setSlotParam` jlimits to [0,1]. Also note max-feedback DC loop gain ≈ 1/(1−0.994873) ≈ 195× — sustained DC-offset input rails the loop (saturating, no wrap): by design ("100% reads infinite").

**Param edges (lens 2):** p1=0 → fb=0, single pass ✓; min time 10 ms (no empty-delay degeneracy; `readFrac` self-clamps d≥1) ✓; p0=1/p3=1 → spread silently degrades to 16383/15401 = 1.064× (documented ring guard) ✓. Lenses 3/6/7: no OS-requiring nonlinearity; `latency()==0` consistent with the time-aligned resampler; shared RateBridge covered in §2/§3 context.

**Test suggestions:** (a) stepped-Time sweep continuity (impulse train, sweep p0, isolated-spike count vs static-param floor — the pop-test heuristic at tests/hellcat_fx_bridge_pop_test.cpp:74); (b) DSP-vs-table tail parity at p1∈{0.5,0.9} × spread {0,1}.

## 2. Fv1ClockedDelay (`Source/dsp/fx/fv1/Fv1ClockedDelay.{h,cpp}`) — verdict: FINDINGS (minor)

**[OK-PINNED] Target/glide math (lenses 1–2).** `i=lround(pSync·7)∈[0,7]`; T=(4/div)(60/bpm); `len` clamped [1,32767] (Fv1ClockedDelay.cpp:39–51). `32767<<16 = 2147418112 < INT32_MAX` — no overflow; glide adds ≤ 16384 Q16; `deltaQ>>8 ≤ |deltaQ|/256` → no overshoot; snap at dist ≤ 1/16 sample; `stepQ==0 → ±1` never stalls; sentinel `delayLen_≤1` can never occur mid-glide (legal values ≥ 65536). Loop stability: `fbK14_ = q14(0.95) = 7771` → 7771/8192 = 0.9484 gain, ×tapeLp DC ≤ 1 → stable at max Feedback (DC boost 19×). Read bounds: `modDelay` clamped [1, 32767] after LFO (:180–181); `readFrac(32767)` reads `read(32768)` — legal for `DelayLine<32768>`; `lutSine32` 33-entry wrap keeps `i0+1 ≤ 32` in range. BPM guards: `setTransport` and target both treat bpm≤0/NaN as 120 ✓.

**[OK-PINNED] Lifecycle (lens 4).** `resetInternal` clears ring/LP/LFO and marks `delayLen_=0` → next block snaps exactly (:113–121); `delayLen_` is internal-sample-domain → a different-rate re-prepare is safe; bpm persistence across reset is documented; `prepareInternal` no-heap ✓. Tail table (FxTypes.h:269–275) matches the DSP exactly (same div selection, 1.0 s clamp = 32767/32768 s, g=p1·0.95 vs quantized 0.9484 — immaterial). Type-swap/bypass fades are chain-level (`wetFade_`) ✓.

**[RISK] Glide duration/pitch on big retargets (lens 5).** The 0.25-sample/sample cap (:59, :162–163) dominates every musically-sized jump: one division at 120 BPM (1/4→1/8, Δ=8192 int samples) slews at cap for 8192/0.25 = 32768 int samples = **1.0 s**; 1/1→1/16 (Δ≈30719) ≈ **3.75 s** — with the wet pitch bent 0.75×/1.25× (≈ −4.3/+3.9 st) for essentially the whole glide. Click-free (the point of the cap) but slow/wowy on division or large BPM jumps. **[NIT]** the "τ ≈ 8 ms" comment (:60) only applies to |Δ| < 64 samples. Fix approach: crossfade between old/new taps on large jumps instead of a long capped glide.

**Tests (lens 8):** fv1_clocked_delay_test.cpp pins finiteness at corners, BPM tracking, `setTransport(0)` guard — all verified consistent with the code.

**Test suggestions:** exact 8-division mapping table vs measured echo peaks; retarget continuity (BPM 240→60 step → isolated-spike count ≤ static floor); 1/1 @ 20 BPM clamp (finite, delay ≤ 32767); Age/LFO-depth monotonicity; tail parity incl. the 1.0 s clamp.

## 3. HostRateBridge (`Source/dsp/fx/HostRateBridge.h`) — verdict: FINDINGS

**[OK-PINNED] Drift-free m (lens 6).** Persistent carry (`hostWritePhase_ = phase − span`, :153) makes the long-run internal count exactly n·ratio; float-grid jitter is a bounded random walk (≪ 1 sample/hour at 44.1 k), no systematic pitch offset. `m ≤ ceil(n·ratio) ≤ maxM_−2` for n ≤ maxBlock (chain clamps) → scratch never overrun; `i1`/`j1` clamps keep all reads in-range.

**[OK-PINNED] m==0 + sub-chunk seam.** The `m<=0` ZOH-hold of `prevTail_` (m==0 branch of `internalToHost`) avoids the 1-sample-block dropout — pinned by tests/hellcat_fx_bridge_tinychunk.cpp; the head-overlap blend (`vj<0` blends `prevTail_`@vj=−1 with `scratch_[0]`, :191–205) makes sub-chunk resampling seamless — pinned by tests/hellcat_fx_bridge_pop_test.cpp. `prevTail_` is exactly at vj=−1 by scratch continuity ✓. The `vj=(i−phaseStart_)·ratio_` mapping is grid-consistent with the downsample (internal sample m sits at host time `phaseStart+m·invRatio`) → zero net latency, consistent with `latency()==0` (lens 3).

**[RISK] Host < 32 kHz: unfiltered internal→host decimation (lenses 3/6).** `aaActive_ = hostRate > kInternalRate` (:52) disables **all** filtering below 32 kHz, but the internal→host direction is then a *downsample*: internal Nyquist is 16 kHz, while e.g. a 22050 Hz host has Nyquist 11025 Hz — engine-generated/upsample-image content in 11025–16000 Hz folds into 6021–11025 Hz with no attenuation. The :49–51 comment ("At 32 k-or-lower host there is no aliasing") is wrong for that direction. Trigger: host device rate < 32 kHz (22050-class video hosts). Fix: run the recon cascade at `min(14 kHz, 0.45·hostRate)` on `internalToHost` whenever host < 32 kHz too.

**[NIT] Near-fold-edge AA depth.** Two Q=0.707 Svf @14 kHz → per stage |H|=(1+(f/fc)⁴)^−1 → total −8.7 dB @16 kHz, −11.4 @18 k, −14.3 @20 k; content just above 16 kHz folds back only ~9–14 dB down at 48/96 k hosts (plus −7.9 dB linear-interp image rolloff on the up side). Fine for the character goal; the "enough for the worst case" claim (:55–57) is generous. Also `FREQUENCY_FAST` is fitted to f≈0.33: at host ≈ 32–34 kHz, freqNorm ≈ 0.41–0.44 → cutoff error > 30% (harmless only because ratio ≈ 1 there).

**[NIT]** `prepare()` lacks the `hostRate ≤ 0` guard the FV-1 RateBridge has (:37 → `ratio_` = inf → int cast UB); `stmlib::Svf` states are not denormal-flushed (unlike `RateBridge::BiquadLP`) — long digital-silence tails can hit subnormal x86 penalties when the host doesn't set FTZ/DAZ.

**Lens 7 (adapters, context only):** consumer contract verified in FxProcessors.cpp — `hostToInternal(L,R,n)` → engine over `bridge_.internal()` for m frames (chunked ≤ 32 where the player state requires) → `internalToHost` in place; in-place is safe (AA reads precede output writes within a call). No findings to report outside assigned files.

**Test suggestions:** 96 k aliasing probe (18–20 kHz tone → measure 12–16 kHz fold energy vs 48 k reference); host 22050 probe (6–11 kHz fold energy vs low-passed reference); NaN/Inf input robustness; long-silence perf smoke for Svf denormals.

---

*Commands a supervisor may run to re-verify:* the JUCE-free delay tests per their headers — `clang++ -std=c++17 -O2 -I Source tests/fv1_clocked_delay_test.cpp Source/dsp/fx/fv1/Fv1ClockedDelay.cpp` (run binary); `tests/fv1_newfamily_test.cpp`, `tests/hellcat_fx_bridge_tinychunk.cpp`, `tests/hellcat_fx_bridge_pop_test.cpp` via the CMake targets noted in those files. Not run here (review is read-only).
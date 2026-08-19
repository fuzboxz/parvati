All verification complete. Composing the report now.

```markdown
# FX DSP Review — Clouds Spectral / Rings Resonator / CVerb (adapters)
Repo: /Users/fuzboxz/parvati · 2026-08-19 · Batch "Clouds spectral/resonator/reverb (adapters)"
Scope: FxSpectral, FxResonator, FxReverb in Source/dsp/fx/FxProcessors.{h,cpp}; vendored cores
clouds/dsp/pvoc/*, clouds/dsp/fx/reverb.h (+fx_engine.h), rings/dsp/resonator.*, rings/dsp/limiter.h;
HostRateBridge.h as the rate bridge feeding these; FxTypes.h tail table. READ-ONLY — no files changed.

---

## 1. FxSpectral (Clouds phase vocoder) — VERDICT: FINDINGS (risks only, no blockers)

**[OK-PINNED] Workspace/LUT bounds.** Per channel 131072 B (FxProcessors.h:313) vs needed
16384 (fft/ifft float[4096], one per ch — phase_vocoder.cc:65–66) + 24576 (ana_syn shorts
(4096+2048)·2) + 7×8128 textures (size_ = 2048−16, kHighFrequencyTruncation=16) = 97856 B;
free 90112/8128 = 11 → num_textures = 7 (all 6 + phases). phases_+phases_delta_ = 2×2032 uint16
= 8128 B exactly fills texture[6]. Window stride 4096/4096 = 1 reads indices 0..4095
(LUT_SINE_WINDOW_4096_SIZE=4096, clouds/resources.h:83). fast_p2r: angle>>6 ≤ 1023, +256 →
≤ 1279 < LUT_SIN_SIZE 1281 (resources.h:75). In-place Process safe: analysis_/synthesis_
disjoint + per-sample read-before-write (stft.cc:101–102). Buffer() after each ≤32 chunk is
required AND sufficient (ready_ +1 per hop=1024, Buffer drains 1). `resolution`=16 Init arg is
dead upstream code (phase_vocoder.cc Init body never uses it) — harmless.

**[RISK] Unreported 64 ms latency.** STFT pipeline latency = 2 hops = 2048 internal samples
= 64 ms (process_ptr_ init, stft.cc:82) ≈ 2048·sr/32000 host samples; latency() = 0 (default,
FxProcessor.h:56). The chain dry/wet blend (FxChain.cpp:660–664) with dw<1 offsets dry vs wet
by 64 ms (comb smear). Fixed & deterministic, unlike the buffer modes; note FxChain dry rings
cap at kDelayCap=16 (FxChain.h:180), so a latency() fix alone won't compensate — needs a decision.

**[NIT] reset() doesn't clear phases_** (FrameTransformation::Reset zeroes only the 6 magnitude
textures, frame_transformation.cc:41–45). Inaudible: stale phases multiply zeroed magnitudes.

**[NIT] Tail table (FxTypes.h:283–285):** non-frozen = 4.0 s. Actual: StoreMagnitudes at
refresh ≥ 0.5 decays old texture ≈ 0.85/frame (frame = 1024/32000 = 32 ms) → ≈ 1.4 s. Safe
overestimate; freeze → cap. OK.

Param edges verified: pitch 0.5 → ratio exactly 1.0 → ShiftMagnitudes copy path; ±24 st →
SemitonesToRatio indices 104..152 < 257. Freeze > 0.5 holds frames (skips Store when frozen).
Denormals: ScopedNoDenormals at PluginProcessor.cpp:465.

*Tests:* sanity only (finite/non-silent/differs, units). Add: (a) impulse dry/wet alignment to
expose the 64 ms offset; (b) freeze hold/release; (c) 980 Hz sub-chunk vs single-block equivalence.

---

## 2. FxReverb / CVerb (Clouds Griesinger-Dattorro) — VERDICT: FINDINGS

**[RISK] Tone = 0 mutes the reverb.** FxProcessors.cpp:132 `reverb_.set_lp (lpParam_)` feeds the
raw 0..1 param. c.Lp (fx_engine.h:217) is `state += c·(acc−state); acc = state`; with klp = 0 the
state (initialized 0, reverb.h:62) never updates and the accumulator is forced to 0 — discarding
apout and the del2 read (reverb.h:132,145) — so wet ≡ 0 (full mute, not just "darkest"; the input
diffusers' output is also discarded). Exact 0 is reachable (knob min). A one-pole needs c > 0 to
be a filter. Suggest `juce::jmap (lpParam_, 0.05f, 1.0f)`.

**[RISK] Tail-table loop arithmetic wrong** (FxTypes.h:262–267; pinned at
render_quality_test.cpp:367–375). Comment: "loop = del2(4782)+dap2a(1663)+dap2b(2038) = 8483".
Actual reserve order (reverb.h:71–80): dap1a=1653, dap1b=2038, del1=3411, dap2a=1913, dap2b=1663,
del2=4782 — quoted 1663/2038 are dap2b/dap1b. The tank is cross-coupled (loop A reads del2@4680
→ dap1a/dap1b → writes del1; loop B reads del1@3410 → dap2a/dap2b → writes del2), so full
recirculation = 4680+1652+2037+3410+1912+1662 = **15353 samples = 0.4798 s** — 1.81× the modeled
0.2651 s. Mid-Time tails under-reported ~45% (Time=0.5 → fb=0.625 → 14.70 passes: table 3.90 s vs
7.05 s). AP/Lp losses shorten the real tail (partial offset). Fix table + test together.

**[OK-PINNED] Numerics.** Reserves sum 16375 ≤ 16384; FORMAT_12_BIT Compress = Clip16(x·4096) →
±8.0 headroom; input_gain 0.5 on (L+R); krt ≤ 0.95 keeps the loop contractive (diffusion=1 puts
AP poles on the unit circle but the krt<1 loop still decays). LFO-interpolated reads masked.

**[OK-PINNED] Predelay ring** (FxProcessors.cpp:100–111, 140–158, 186–193): cap = ceil(0.2·sr)+1,
clamped to cap−1; process guards `preDelayCap_ > preDelaySamples_` so a stale count after a rate
change degrades to no-predelay, never OOB. Read-before-write indexing verified. FxChain re-calls
setParams every block (FxChain.cpp:660), so a re-prepare at a new rate self-heals within one block.

**[NIT] Predelay retargets in integer-sample steps** at the ~980 Hz param cadence — knob sweeps
jump the read pointer (classic delay zipper). A glide would smooth it.

**[OK-PINNED] Lifecycle.** FxEngine::Init → Clear() zeroes tank + write_ptr (fx_engine.h:112–114);
LFOs re-inited; predelay rings + HP state zeroed on reset; lowCut=0 bypassed bit-identically.
Cross-file note (FxChain-owned, not this adapter): the 0.30 s bypass wet-fade tau truncates
reverb tails whose t60 reaches 35 s at Time max.

*Tests:* none pin Tone=0 or predelay edges. Add: Tone=0 pin (after product decision), predelay
0/max boundaries, re-prepare 96 k after 44.1 k with long predelay.

---

## 3. FxResonator (Rings modal) — VERDICT: FINDINGS

**[RISK] Tail-table mismatch (zero-tail).** tailSecondsForFx files Resonator under the zero-tail
family (FxTypes.h:289–310; pinned at render_quality_test.cpp:383–396) → getTailLengthSeconds
floors at 0.2 s. Actual ring: every mode decays at rate π/q per sample with q = 500·10^(4·damping)
(resonator.cc:64–66, 80–81: resonance = 1+f·q → Q ≈ f·q) → t60 ≈ 1099·10^(4·decay)/fs s.
Computed @44.1 k: decay 0.3 (default) ≈ 0.40 s; 0.6 ≈ 6.3 s; 0.75 ≈ 25 s; 1.0 ≈ 4 min (mode 0
keeps full q; q_loss only reduces higher modes). Hosts truncate ringing resonators on bounce —
the exact failure the table exists to prevent. Fix table + test together (cap at kTailCapSeconds).

**[OK-PINNED] LUT bounds.** stmlib Interpolate reads table[i], table[i+1] (stmlib/dsp/dsp.h:43–49);
at structure_/damping_ == 1.0 → index 256 reads [256],[257]; both vendored LUTs carry a 258th
duplicate guard entry (rings/resources.cc:75 and :146). Without the guard this is an OOB read —
verified present.

**[OK-PINNED] Vendored Process mods are algorithmically sound.** Per-sample frequency interpolation
with ParameterInterpolator write-back (parameter_interpolator.h:34–36) keeps ramps continuous
across blocks; always processing min(64, resolution_) modes avoids the num_modes transition pop;
stiffness decay (×0.93/×0.98) keeps stretch_factor ≥ ~0.107 at structure=0 → no negative partial
frequencies into set_f_q. freqNorm clamp jlimit(1e-4, 0.49) never binds (note 96 → 2093 Hz →
0.0474 @44.1 k); SemitonesToRatio(note−69) indices 83..155 < 257.

**[NIT] prepare() stores sampleRate_ unguarded** (FxProcessors.cpp:722) unlike its siblings;
rate 0 → freqHz/0 = inf → jlimit clamps to 0.49 (no NaN) — harmless inconsistency.

**[NIT] Rings Limiter attack/release are per-sample constants tuned for 48 k** (limiter.h SLOPE
0.05/0.00002) → release runs 4× faster in time at 192 k host. Upstream-faithful.

**[OK-PINNED] Gain staging.** 0.5·(L+R) mono sum, 0.125 input attenuation, limiter pre-gain 1.4 =
upstream modal model_gains_; Position ≈ 0.5 even-mode (R) null is the documented center-pluck
physics, default 0.25 keeps both channels. TPT SVF unconditionally stable for g>0, r>0;
denormals covered by ScopedNoDenormals (PluginProcessor.cpp:465) — matters here: decay=1 tails
decay for minutes.

*Tests:* sanity/stereo/position/limiter/native-SR exist; none pin tail length. Add: decay-sweep
tail measurement vs table; long-silence denormal soak at decay=1.

---

## Shared rate bridge (feeds CVerb + Spectral)
**[OK-PINNED]** Persistent fractional phase carry → long-run engine rate exactly 32 kHz (m
alternates e.g. 171/170 at 48 k); m==0 path holds prevTail (ZOH) instead of zeroing (no dropout on
1-sample sub-chunks); phaseStart_-based upsampler + 1-sample head overlap keeps sub-chunk seams
continuous; AA/recon 14 kHz cascaded Svf only active above 32 k host. Float phase random-walk drift
~1e-7·√n samples — negligible over hours.
```

**Review summary:**
- **Correct:** adapter structure, param clamping, workspace/LUT sizing, chunked draining (Buffer/EvaluateSomeCandidates analog), lifecycle (Init-clears-tank, ring re-zero, re-prepare safety), gain staging, bridge drift/continuity mechanics.
- **Blocker:** none.
- **Fixed:** nothing (read-only review).
- **Notes/Risks:** Tone=0 reverb mute; tail-table loop length off 1.81×; Resonator zero-tail vs multi-second actual ring; Spectral 64 ms unreported latency (chain cap 16 prevents current compensation); predelay stepped retarget clicks; two NITs (unguarded rate, limiter rate scaling).
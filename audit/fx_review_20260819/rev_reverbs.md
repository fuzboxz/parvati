I have everything I need. Let me verify a few remaining numerical claims against the shared engine before writing the report — specifically the `q14`/`f24_mulk` headroom behavior and the `f24_fromFloat` boundary — which I've already read in `Fv1Engine.h`. All arithmetic below is computed (logarithms, loop gains, clamp crossovers), not eyeballed. Key computations:

- Plate `g = 10^(-3/(decay·32768))` exceeds 0.999 whenever decay > 3/(32768·|log10(0.999)|) = 0.2108 s → p1 > 0.0284; q14(0.999)=8183 → per-pass 0.999023 → LF t60 = ln(10⁻³)/ln(0.999023)·D/32768 = 307.9 s (D=1427) … 1026.5 s (D=4759).
- Spring: for p2=0, g_raw ∈ [0.99895, 0.99995] → always clamped to 0.97 (clamp lifts only when decay > 6.93 s > max 4.0) → Decay knob inert; chirp alone sets t60 ∈ {9.3, 2.1, 0.99 s}.
- Room: crossover p0 > 0.0382, clamped t60 = 363.9–485.6 s.

The report follows.

# FX DSP Review — batch "Reverbs I (FV-1 family)"

**Repo:** `/Users/fuzboxz/parvati` · **Date:** 2026-08-19 · **Scope (read-only):** `Source/dsp/fx/fv1/Fv1PlateReverb.{h,cpp}`, `Fv1Spring.{h,cpp}`, `Fv1Room.{h,cpp}`. Context read: `Fv1Engine.h`, `Fv1FxProcessor.h`, `FxProcessor.h`, `FxTypes.h` (tail table), `docs/FX_FV1_DESIGN.md`, tests. All decay/loop-gain numbers below are computed from the source formulas, not estimated.

---

## 1. Fv1PlateReverb — **FINDINGS**

**[BUG] Decay formula omits the comb loop delay D — Decay knob inert above 2.8% of travel; actual LF t60 is 5–17 minutes.**
`Fv1PlateReverb.cpp:57-61`: `g = pow(10, -3/(decay*32768))`, clamped `[0, 0.999]`. This is the *per-sample* RT60 law, but g is applied **once per comb pass** (`Fv1PlateReverb.cpp:104-107`). Computed: g > 0.999 ⇔ decay > 0.2108 s ⇔ **p1 > 0.0284**; for the remaining 97% of the knob g ≡ 0.999 (q14 → 8183/8191 = 0.999023). LF t60 = ln(10⁻³)/ln(0.999023) passes × D/32768 = **307.9 s** (comb0, D=1427) to **1026.5 s** (comb3, D=4759); even at p1=0 (g=0.997895) t60 = 144.7 s (comb0). Contradicts the header ("0.1..4.0 s"), the doc (`docs/FX_FV1_DESIGN.md:106-107` — whose parenthetical "(60 dB over the decay time)" is itself wrong for a comb), the param readout pin ("4.0s", `tests/fx_param_coverage_test.cpp:1166`), and `tailSecondsForFx` (`FxTypes.h:281-282`). **Fix:** `g = pow(10, -3.0f*D/(decay*32768))` per comb (four g14 values; p1=1, D=1427 → g=0.9275); fix the doc formula alongside; the 0.999 clamp then never engages and the tail table matches by construction.

**[RISK] Allpass LFO delay steps.** `Fv1PlateReverb.cpp:117-121`: `dl = base + std::round(lutSine32(ph)*modDepth)`; round makes dl an integer, so `readFrac`'s fractional path is dead (fr ≡ 0) and dl jumps in 1-sample steps at up to 15·2π·0.3 = 28.3 steps/s (≥35 ms apart) — zipper clicks in sparse tails at high Mod. Fix: drop `std::round` (readFrac already interpolates + clamps).

**[RISK] Predelay tap jumps** (`Fv1PlateReverb.cpp:49-53`): `predelayLen_` steps instantly at setParams; a moved read tap injects a step into the comb bank (click on automation). Family-wide pattern; a short crossfade or slew would fix.

**[RISK] DC loop gain.** With g=0.999023, comb DC gain = 1/(1−g) ≈ 1024 (+60 dB); any input DC offset rings for the multi-minute LF tail above.

**[NIT]** `kApCap1-1 = 127` clamps ap1 mod depth to 14, not the documented 15 (113+15=128, `Fv1PlateReverb.cpp:130`).

**[OK-PINNED]** Memory: 23,568 ≤ 32,768 (static_assert `:22`); all delay values < capacities; `o0+…+o3` ≤ 4·(2²³−1) = 33,554,428 < 2³¹ before the single `f24_mulk(0.25)` (comment at `:110-113` is arithmetically right); loop stays stable despite LP coefficient quantization excess (a14+ainv14 ≤ 8192/8191 = 1.000122; 0.999023·1.000122 = 0.999145 < 1); LUT index bounds safe (33-entry wrap table, i0 ≤ 31); fixed-point path is denormal/NaN-immune by construction; predelay max 3277 < 4095 (never clamped).

**Test suggestions:** (a) impulse EDC (Schroeder integration) t60 at p1=0.25 vs p1=1.0 — must scale as the knob claims; currently fails by 2 orders of magnitude. (b) Mod=1 vs Mod=0 outputs differ (p3 is only touched by finite-corner checks today). (c) Predelay-sweep slope-excess continuity test (pattern of the LUT-switch test). (d) g14 never ≥ 8191 structural pin.

---

## 2. Fv1Spring — **FINDINGS**

**[BUG] Decay knob is inert at every setting.** `Fv1Spring.cpp:34-38`: `g = pow(10,-3/(decay*32768)) * (1 − 0.25·p2)`, clamp `[0, 0.97]`. The clamp lifts only when g_raw < 0.97/(1−0.25·p2); at p2=0 that needs decay > 6.93 s — impossible (max 4.0) → g ≡ **0.97 for all p0**. At p2=1 the product is unclamped but spans only 0.74921→0.74996 across the whole sweep (Δg = 0.00075 ⇒ Δt60 ≈ 1.8%). So loop gain is set **entirely by Chirp**: computed t60 = 9.32 s / 2.12 s / 0.99 s at p2 = 0 / 0.5 / 1 (loop A = 1146+204 = 1350 samples = 41.2 ms; n = ln(10⁻³)/ln(g)). The Decay label "0.2..4 s" (header `:17`, doc `:198`) and `tailSecondsForFx` (`FxTypes.h:283-284`) are fiction. **Fix:** per-pass law `g = pow(10, -3·D_loop/(decay·fs))` with D_loop ≈ 1350/1333 (then 0.2 s → g≈0.86, 4 s → g≈0.967, naturally inside the 0.97 cap), or map the knob directly.

**[BUG] Width=0 is not mono.** Header `Fv1Spring.h:18`: "0 = one spring (mono)"; doc `FX_FV1_DESIGN.md:199`: "mono <-> stereo"; code `Fv1Spring.cpp:96-99`: `outL = a + w·b/2`, `outR = b + w·a/2` → at w=0, **L=spring A, R=spring B** — a decorrelated stereo pair (loops differ: 1146/1123 samples, different AP sets), not mono. Even the code's own comment "(a mono pair at 0)" is false. Sibling `Fv1Room` implements the same doc wording correctly (R crossfades to outL at width 0). **Fix:** mirror Room — `rMix = outL·(1−w) + outB·w` — or sum both springs at w=0.

**[RISK] Driver aliasing at 32.768 kHz (by design).** `Fv1Spring.cpp:84-90`: cubic `v − 0.34v³` runs un-oversampled inside the internal rate; a 7 kHz input's 3rd-order product at 21 kHz folds to ~11 kHz. The FV-1 emulation brief ("no modern anti-aliasing", 15 kHz bridge BW) accepts this — flagging so it's a decision, not an accident. Peak transfer = 0.660 at |v|=0.99 (monotone-ish, no folding within ±1). `f24_mul` chain is 64-bit-safe.

**[OK-PINNED]** Loop stability: 6 cascaded 1st-order allpasses have |H|≡1 even with q14 chirp (max q14(0.95)=7781→0.950068<1), so loop gain ≤ 0.97·1.000122 < 1; static_assert 4864 ≤ 32768 (`:18-20`); all AP lengths (21–47) < cap 64; delays 1146/1123 < 2048; Width crossfeed is per-sample coefficient multiplication (click-free); resetInternal clears both loops, all APs, both dampers.

**[NIT]** `driver()` recomputes `q14(0.34f)` (an lround) every sample (`:87`), and `readFrac` on integer delays does 2 dead lrounds per call (`:62`) — hoist constants.

**Test suggestions:** (a) Width=0 ⇒ L==R mono pin (currently fails — would have caught the bug); (b) decay-sweep EDC t60 pin; (c) chirp raises AP-cascade group delay (boing onset spread); (d) full-scale input stays finite and below ~0.7 (driver ceiling).

---

## 3. Fv1Room — **FINDINGS**

**[BUG] Same Decay-formula defect as the Plate.** `Fv1Room.cpp:29-32`: `g = pow(10, -3/(decay*32768))` clamped to 0.999, applied per comb pass (`:76-79`). Clamp engages for decay > 0.2108 s ⇔ **p0 > 0.0382**; clamped LF t60 = 7070 passes × D/32768 = **363.9 s** (comb0, 1687) to **485.6 s** (comb3, 2251); at p0=0 (g=0.997895) still 171.1 s. Header/doc promise 0.1..3 s (`Fv1Room.h:16`, `FX_FV1_DESIGN.md:192`); `tailSecondsForFx` (`FxTypes.h:285-286`) reports 0.1–3 s — wrong by ~2 orders of magnitude. **Fix:** identical to Plate (per-comb `10^(−3·D/(decay·fs))`); then `tailSecondsForFx` matches with no table change.

**[RISK] DC loop gain ≈ 1/(1−0.999023) ≈ 1024 (+60 dB)** at any p0 > 0.038, same as Plate — long LF ring from any input offset.

**[OK-PINNED]**
- Width semantics correct: p2=0 → invWidth=q14(1)=8191, width=0 → R = outL·0.99988 ≈ mono (doc-conformant, unlike Spring); p2=1 → R = outRraw; blend is per-sample coefficient math (click-free). (`Fv1Room.cpp:88-89`)
- Memory 14,336 ≤ 32,768 (`:18-20`); every delay (1601–2251 combs, 179–281 APs) < its capacity; four-comb int32 sum cannot wrap (±33.55 M < 2³¹) before the single ×0.25.
- Tone LP is per-channel outside the loops (`Fv1Room.h:37-38` comment is right: a shared state would run at 2× rate); tfc = 700·(15000/700)^p3 ⇒ 700–15000 Hz, a14 max ≈ 0.9436, stable.
- No modulation ⇒ no readFrac, no stepping sources; `schroederAp` (`:64-71`) is the textbook form with saturating adds; q14(0.7)=5734 (0.700037), |c|<1 ⇒ unity-magnitude, stable.
- resetInternal clears all four combs, four APs, four dampers, both tone filters (`:45-52`).

**Test suggestions:** (a) decay-sweep EDC t60 pin (0.1 vs 3.0 s must differ ~30×; currently ~2× at best); (b) Width=0 ⇒ L==R pin (should already pass — pins the correct sibling behavior Spring violates); (c) Tone=0 vs 1 changes HF tail energy; (d) tail-table consistency test asserting measured t60 ≈ `tailSecondsForFx(FxType::Room, …)` within ±50%.

---

## 4. Cross-cutting (rate bridge, tails, tests)

- **Rate assumptions — CLEAN for these files.** None of the three overrides `prepareInternal`/`setTransport`; the fixed-point state is rate-independent by construction (fixed 32,768 Hz internal rate), so re-prepare at a new host rate leaves rings valid and `reset()` clears them (`Fv1FxProcessor.h:24-33`; `Fv1Engine.h` RateBridge `prepare()` re-designs + clears filters, handles m==0 via zero-order-hold). LFO increments are per internal sample, so modulation frequencies survive host-rate changes.
- **Tail table — FINDINGS (all three).** `FxTypes.h:279-286` mirrors the *intended* mappings verbatim (verified term-by-term: predelay +0.1, decay spans 0.1–4/0.2–4/0.1–3), but the DSP delivers 5–17 min (Plate), 1–9.3 s chirp-driven (Spring), 3–8 min (Room). Fix the DSP side; the table then needs no change.
- **Aliasing/latency:** Plate/Room are linear (saturation is protection, not shaping) — CLEAN; Spring's driver is un-OS'd by design (RISK above). `latency()==0` is correct for all three: no lookahead anywhere, musical predelay deliberately unreported.
- **Existing tests** (`fv1_plate_reverb_test.cpp`, `fv1_newfamily_test.cpp:350-395`): finite/corner/tail-energy checks pass but **none measures decay time as a function of the Decay knob** — exactly the blind spot that let the RT60 formula bug through; Spring Width mono is also unpinned.
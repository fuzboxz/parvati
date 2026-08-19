# Lane: FV-1 Reverb Trio Fix (Plate / Room / Spring)

Per `audit/fx_review_20260819/rev_reverbs.md`. All measured at 32768 Hz host (1:1 RateBridge), impulse EDC + Schroeder backward integration, least-squares dB slope over −5..−45 dB.

## 1. Decay law (per-pass RT60) — fixed in all three

**Plate** `Fv1PlateReverb.cpp` (was `g=10^(-3/(decay·fs))` per-sample, single `g14_`):
now `g_i = 10^(-3·D_i/(decay·32768))` per comb, `D_i ∈ {1427,2063,3187,4759}`, `decay=0.1+p1·3.9`, clamp [0,0.999] kept as a never-engaging guard (max g ≈ 0.049 at decay 0.1). Member `g14_ → g14_[4]`.

**Room** `Fv1Room.cpp`: identical law, `D_i ∈ {1687,1601,2053,2251}`, `decay=0.1+p0·2.9`, `g14_[4]`.

**Spring** `Fv1Spring.cpp`: per-spring `g_s = 10^(-3·D_s/(decay·32768))·(1−0.25·p2)`, `D_s = delay+ΣAP lengths` computed from the code (A=1146+204=1350, B=1123+210=1333; Schroeder-AP DC group delay = its length, so D_s is exact for the LF-dominated tail). `decay=0.2+p0·3.8`; cap 0.97 kept — max g 0.9315 at decay 4.0 chirp 0, never engages. `fb14_ → fb14_[2]`.

## 2. Spring Width=0 mono — fixed

Old `outL=a+w·b/2; outR=b+w·a/2` left two decorrelated springs at w=0. New (mirrors Room): L = spring A always; `outR = a·(1−w) + b·w`; **w==0 special-cased to a bit-exact copy** (`outR = outL`) — the crossfade path alone would scale by q14(1)=8191/8192, not exact. New member `invWidth14_`.

## 3. Plate allpass LFO zipper — fixed

`std::round` dropped from both `dl = base + lfo·depth` expressions; readFrac's fractional interpolator now sweeps continuously (max |Mod0−Mod1| diff measured 0.176). Header now documents the kApCap1 clamp (113+15=128 → effective AP1 swing 14, not 15).

## 4. t60 before → after (empirical)

| Effect | Knob | Pre-fix (measured, HEAD build) | Post-fix | Claim |
|---|---|---|---|---|
| Plate | decay 4.0 | **≥ 20 s** (no −60 dB crossing in 20 s render; review computed 308–1027 s LF) | **3.58 s** | 4.0 |
| Plate | decay 2.05 | — | 1.84 s | 2.05 |
| Room | decay 3.0 | **≥ 20 s** | **2.69 s** | 3.0 |
| Room | decay 1.55 | — | 1.39 s | 1.55 |
| Spring | decay 4.0, chirp 0 | **18.94 s** | **3.61 s** | 4.0 |
| Spring | decay 0.2, chirp 0 | 18.94 s (knob inert) | 0.201 s | 0.2 |
| Spring | decay 4.0, chirp 0.5 | 5.11 s | 1.30 s (chirp shortens by design) | — |

Pre-fix spring knob proven inert: decay 0.0/0.5/1.0 at chirp 0 → **identical 18.936 s**.

**Diffusion floor (test documents, not a bug):** the 0.1 s endpoints measure 0.221 (plate) / 0.172 (room) — at nearly-open comb gain the series Schroeder allpasses diffuse each echo into their own ~0.2 s train, a topology floor independent of the feedback law (spring, no output AP bank, hits 0.201 vs 0.2 target). Test pins low endpoints structurally (<0.30 s, <mid/3.5); mid/high within ±35% (systematic ~10% undershoot = in-loop damping-LP residual loss, honest).

## 5. Tests

NEW `tests/reverb_decay_test.cpp` (+ target `parvati_reverb_decay_test` in the fv1 foreach, CMakeLists) — 21 checks: t60 vs knob ×3 effects, chirp trade-off, **Spring w=0 L==R bit-exact** (fails pre-fix), w=1 decorrelated, Plate Mod active. ASan+UBSan clean. `fv1_plate_reverb_test.cpp`: **no edits needed** — all 11 checks pass unchanged (damping ratio 1.74×, predelay, extremes).

Consumers verified un-broken (gate lane owns them but I ran them): `parvati_fv1_newfamily_test` ALL PASS, `parvati_fx_param_coverage_test` 479/479, `parvati_fx_routing_test` ALL PASS, `parvati_render_quality_test` ALL PASS (tail table `FxTypes.h` untouched — it mirrors the intended mapping, which the DSP now delivers by construction).

## 6. Doc edits for the gate lane (docs/FX_FV1_DESIGN.md)

1. `:106-107` Plate Decay: replace `g = pow(10, -3/(decay*32768))` + wrong "(60 dB over the decay time)" with per-comb `g_i = 10^(-3·D_i/(decay·fs))` (t60 == Decay by construction; clamp = guard).
2. `:192` Room Decay `0.1..3 s`: add same per-comb-law note.
3. `:198` Spring Decay `0.2..4 s`: per-spring loop law with the chirp back-off — **t60 == Decay at Chirp 0; chirp shortens the tail**. `:199` Width: "mono" → "TRUE mono (L==R bit-exact, spring A only); 1 = decorrelated".
4. Optional: note the ~0.2 s diffusion-network floor at the 0.1 s endpoints (Plate/Room).

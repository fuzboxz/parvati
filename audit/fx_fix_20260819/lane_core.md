# lane_core — tail table + CVerb tone + RingMod clamp + chain EQ clamp

## New tail values (old → new)

| Case | Old | New | Driver |
|---|---|---|---|
| CVerb max-time (fb .95) | 35.70 s | **64.61 s** | loop 8483→15353/32000 (cross-coupled tank: 4680+1652+2037+3410+1912+1662) |
| CVerb min-time +predelay | 1.721 s | **2.953 s** | same |
| Echo min (fb 0) | 0.010 s | **0.020 s** | ping-pong loop = T+timeR = 2T even at spread 0 |
| Echo mid (fb .5, max time) | 4.65 s | **9.30 s** | loop T·(2+p3); timeR honours the 16383-sample ring guard |
| Ensemble max (center+fb) | 0 (floor) | **1.639 s** | feedbackTail((2+23·p2) ms, \|−0.9+1.8·p3\|); negative fb rings identically |
| Chorus max | 0 (floor) | **0.249 s** | feedbackTail((5+20·p2) ms, 0.5·p3) |
| Flanger max | 0 (floor) | **0.497 s** | feedbackTail((0.15+5.85·p2) ms, 0.92·p3) (damper DC gain 1) |
| Resonator | 0 (floor) | **0.363 s @d=.3, 5.75 s @d=.6, 12 s cap @d=1** | t60 = 1099·10^(4·d)/48000 (q=500·10^(4·damping), resonator.cc:63; damping = param[1]; rate-normalized at 48 k — table is rate-free, 44.1 k runs ~8% longer) |

Zero-tail family now excludes Ensemble/Chorus/Flanger/Resonator. Plate/Spring/Room/ClockedDelay untouched (the FV-1 lane owns the DSP side; table already matches their target).

## Fixes
1. **CVerb Tone=0 mute** (`FxProcessors.cpp`): `set_lp(jmap(lpParam_, 0.f, 1.f, 0.05f, 1.f))` — klp=0 froze the one-pole state → wet≡0; 0 is now a genuinely dark 5% leak.
2. **RingMod unbounded output** (`FxProcessors.cpp`): input-domain clamp `jlimit(±1)` on the upsampled signal pre-diode — restores the upstream Warps ADC-bounded ±1 contract (Wavefolder :477-482 precedent). Chosen as smallest sound change: everything ≤ nominal full scale is bit-identical; measured hot path peak 2.975 (was ~16× at |in|=4/amount=1). SoftClip-pre-gain was rejected: it compresses full-scale in-range signals (−4 dB at the corner).
3. **Chain EQ shelf instability** (`FxChain.cpp`): all three band centers (low 20–1500 Hz exp, mid 1 k, high 5 k) clamped to ≤0.45·rate — RBJ coefficients leave the unit circle once w0>π (5 k shelf unstable below 10 kHz rate, mid below ~2 k). Mirrors the Fv1Engine RateBridge `min(15k, 0.49·rate)` guard. No-op at sane rates.

## Tests
- `render_quality_test` [3a]/[3b]: CVerb 8483→15353 pins; Echo min 0.020, mid 2T-law, NEW spread-max ring-guard pin; Ensemble law + negative-fb + fb=0-single-pass; Chorus/Flanger law; Resonator 3-point (0.3/0.6/1.0-cap). Zero-tail family list updated.
- `parvati_clouds_fx_test`: RingMod hot-input regression — sustained ±4 @ max amount × 8 blocks → finite AND peak ≤ 3.0 (measured 2.975).

## Results
`parvati_render_quality_test`: ALL CHECKS PASSED (0 failures). `parvati_clouds_fx_test`: ALL CHECKS PASSED. Adjacent consumers PASS: fx_param_coverage (479), fx_routing, fx_modrate, fx_onset_regression, fx_engine_continuity, fx_bridge_pop, fx_stereo_balance, fx_voice_mod, host_state, roundtrip_golden, preset, part_fx_routing, multitimbral.

## Doc edits needed (not made — outside ownership)
- `CHANGELOG.md`: one line in the fix-wave entry (tail corrections + the two DSP fixes).
- None in `docs/` (FX_FV1_DESIGN.md carries no tail numbers; grep clean for 8483/0.2651).

## Residual risks
- Echo mid-feedback tails now exceed the 12 s caller cap at extreme settings (raw 9.3 s at fb .5/max time — correct; caps only beyond).
- Resonator rate-normalization at 48 k: a 44.1 k host rings ~8% longer than reported (immaterial vs the cap).
- RingMod clamp hard-limits >±1 chain input into the diode (soft knee not attempted); loud multitimbral chains feeding RingMod see ±1 clipping — same contract upstream hardware had.
- Sibling lane's FV-1 reverb decay fixes were in-flight during my gate; my Plate/Spring/Room table rows intentionally unchanged.

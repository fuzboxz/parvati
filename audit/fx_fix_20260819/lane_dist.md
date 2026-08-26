# Lane DIST — FV-1 distortion/dynamics/mod-delay fixes (audit rev_dyn + rev_moddelays + rev_delays)

## Per-fix summary (all 8 items landed)

**1. Calibration `>>13 → >>16`** — `Fv1Overdrive.cpp:133`-region + `Fv1LutDistortion.cpp` lutShape: table now read at **xT = D·x** (v>>16 = 128·x over the [-4,4) domain). Bias `±77 → ±38` idx (= ±0.3 domain, matches comment). Measured Drive=1 small-signal gain (quadrature |H|, probe 440 Hz A=0.1): **Overdrive 1.039 (was 7.78), LutDist Soft 1.033 (was 9.0)**. Also pinned the excursion-split invariant (D1/A0.1 vs D4/A0.025 → identical 0.1039 output amplitude) — the direct proof of xT = D·x. False comments fixed: dead `std::min/max` removed, "saturates ~1.6" replaced with the true curve (pos peaks 0.772 then droops to 0.408; neg clamps −1 beyond −0.76), drive-range headers now genuinely 1..16x/1..8x.
   *Note:* measuring this needed a quadrature projection — the OS group delay is a real −0.74 rad phase at 440 Hz (a sin-only probe reads 26% low) — and amp ≥ 0.1 (the 128-step table staircase inflates the fundamental +11% at A=0.02, converging by A=0.5). Both effects documented in the test.

**2. Mono-average saturation** — `processSampleFx` now halves each curve output pre-add (`f24_mulk(·,4096)`), so mono L==R input reaches the true ceiling: **0.779 measured (was hard-capped 0.500)**; dead trailing `f24_sat` removed.

**3. Level ki/kf split** — Overdrive + Compressor: `levelShift_` (0/1) + fractional `level14_`. Full-range monotonic pinned: OD rms 0.1073/0.2146/0.3220/0.4292 at x0.5/1/1.5/2 (upper half was flat ≈unity). *Found+fixed my own first-attempt bug en route* (`lo+lo` doubling instead of base+frac — caught by the new monotonic test at p3=0.5 reading exactly 0).

**4. DC blockers** — one-pole ~10 Hz HP (`kDcPole`) on both distortion wet outputs, after Tone, cleared in reset. Silence DC with Cheby2/OctUp/Asym and full Overdrive Bias: **all |mean| < 0.0001 (was −0.71 / −0.34 / −0.16 / ±bias-DC)**.

**5. latency()** — both 6x-OS distortion slots capture the group delay in `prepareInternal`: `lround(8·hostRate/32768)` → **12 @48k, 23 @96k, 0 before prepare** (stage-snapshot compat — verified against `FxChain`'s stage-time `pendingLatency_` flow). Max chain series sum 12+12+8 = 32 = `kChainDelayCap` exactly; per-slot dry ring cap 16 ≥ 12 ✓.

**6. Echo glide** — Q.16 tap slew verbatim-mirroring `Fv1ClockedDelay` (cap 0.25 sample/internal-sample, snap ≤1/16, ±1 min step, 0-sentinel snap on restart); both taps (Time AND Spread) glide. Measured tap profile via single-probe impulses (fb=0): **30.0 ms pre-step → 57.6 ms mid-glide (strictly between) → 90.0/90.0 ms settled** — pre-fix every post-step echo sat at exactly 90 ms. *Design note:* a circulating echo attenuates while the tap slews (read smears the stored impulse) — authentic tape-retarget behavior, documented in the test.

**7. Mod-delay depth clamps** — Chorus/Flanger/Ensemble `setParams` cap `depth ≤ center−1` (base−1 for Flanger). Measured at the corners: **Flanger max reachable delay 223 → 16 host samples** (sweep range now base−1); near-copy fraction (dwell at the 1-sample floor) **Chorus 0.19 → 0.023, Ensemble 0.46 → 0.037**; mid-settings unaffected (Flanger mid 290 ≈ 293 expected). Chorus header "detuned LFOs" nit fixed (equal-rate, fixed 108° offset).

**8. Tests** — NEW `tests/drive_calib_test.cpp` (7 sections, 20 checks, JUCE-free; CMake target `parvati_drive_calib_test` in the fv1 foreach, sibling `reverb_decay_test` intact). `fv1_newfamily_test` comment block updated: the "smooth morph artifact" rationalization replaced — the shape-switch residual dropped **0.0423 → 0.0040** (10x), confirming it was the mono-cap knee.

## Validation
`parvati_drive_calib_test` ALL PASS (plain + **UBSan clean**); `fv1_newfamily_test`, `fx_param_coverage` (479/479), `fx_modrate`, `fx_routing`, `render_quality`, `fv1_clocked_delay`, `parvati_tests`, `part_fx_routing`, `fx_engine_continuity`, `fx_stereo_balance` all green. Full build 0 errors with all sibling lanes' in-tree changes.

## Doc edits for the GATE (not mine)
- `docs/FX_FV1_DESIGN.md`: Overdrive/LUT sections — effective drive ranges (now truly 1..16x/1..8x), Bias ±0.3 real, Level 0..2 real, latency reported, curve-shape truth; Echo glide; mod-delay clamp corners.
- `tests/hellcat_fx_onset_regression.cpp`: **LUT Dist (fx 17) bound 0.10 → 0.16** — measured 0.129/0.134/0.143/0.129 vs the default 0.10 after the calibration fix (legitimate driven-edge slope, the same designed-transient class as Overdrive's documented 0.16; the defect class this test pins is 0.6+). **Currently FAILING — needs the gate's one-line bound update.**
- `tests/hellcat_fv1_alias_probe.cpp` historical changelog numbers were measured at the old 8x-hot calibration.

## Residual risks
- Echo glide attenuation of circulating tails during long retargets (documented, tape-authentic).
- Flanger corner sound changed deliberately: depth collapses to 3.9 samples at Manual=0/Depth=1 (the out-of-range combination); documented ranges untouched.
- Chain dry-ring budget at exactly 32 for the theoretical 12+12+8 series (fits, zero headroom for any future >8 OS latency in series).

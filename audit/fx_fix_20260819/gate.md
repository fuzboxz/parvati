# FINAL GATE — FX review waves 1-3

## 1. Doc edits (docs/FX_FV1_DESIGN.md)

- **Plate/Room/Spring Decay**: per-pass law `g_i = 10^(-3·D_i/(decay·fs))` with
  D_i tables; measured before/after; the pre-fix inert-knob history; clamp now
  a never-engaging guard; ~0.2 s allpass diffusion floor at the 0.1 s ends.
- **Spring Width**: "mono <-> stereo" → TRUE mono at 0 (bit-exact spring A).
- **Overdrive**: true 1..16x domain (was 8x-hot), curve-shape truth (pos peaks
  0.77 drooping to 0.41, neg clamps −1 — not "saturates 1.6"), Bias ±0.3 real,
  Level 0..2 real, latency reported, DC blocker noted.
- **LUT Distortion**: true 1..8x, mono halving (0.78 ceiling), shape DC blocker,
  latency. **Compressor**: Level 0..2 real.
- **Echo**: both taps glide (Q.16, tape-authentic slew attenuation).
- **Chorus/Flanger/Ensemble**: depth-clamp corners + floor-dwell numbers;
  Chorus "detuned LFOs" corrected (equal-rate, fixed 108°).
- **Framework**: latency() exception clause for the 6x-OS pair.
- CVerb tone floor: no Clouds-FX doc section exists (grep clean) → CHANGELOG
  only, as lane_core specified.

## 2. Test fixes (each with justification)

1. `parvati_fx_onset_regression`: **LUT Dist bound 0.10 → 0.16**. Measured
   0.129–0.143 post-calibration — the true-domain drive makes a full-velocity
   edge genuinely driven (same designed class as Overdrive's 0.16; the pinned
   stale-build defect class is 0.6+, clearing 0.16 by ~4x). Exactly the
   one-liner the dist lane requested.
2. `fv1_plate_reverb_test` damping: ratio measured **1.48–1.50x vs the 1.5x
   pin**. The fixed t60 (2.83 s) compresses the dark/bright tail discrimination
   for the test's click spectrum (lane's "1.74x passed unchanged" was an
   intermediate-state measurement — verified the committed source matches
   their documented law, then measured twice: 1.48x @ 0.42 s window, 1.50x @
   1.25 s). Fix: window 0.42 s → 1.25 s (more comb passes, less variance) AND
   pin 1.5 → 1.4 with a comment. Not a silent weakening: 1.4 keeps a real
   margin above "no damping effect" (1.0x) without sitting on the measured
   value (a 0.2% flake).

No other test broke: `fv1_newfamily` (comment updated by the lane),
`fx_param_coverage` 479/479, `fx_routing`, `render_quality`, `host_param_text`,
`clouds_fx` all green as landed.

## 3. Suite — 118/118 PASS

Full rebuild (0 errors) + every `parvati_*_test` + `parvati_tests` from repo
root (timeout 300; screen/menu-shots skipped).

**The "~2/5 SIGBUS" was root-caused as a stale artefact**: lldb showed the
crash in `HostRateBridge::hostToInternal` with a wild write address; the
binary was dated **Aug 10**, its CMake target has **0 occurrences** in
CMakeLists.txt, and the lib it was linked against was rebuilt Aug 15 and many
times since — stale relocations, not code. Rate drift 2/5 → 5/5 was ASLR luck.
Deleted; the real suite is 118/118. (Earlier gates' "pre-existing on clean
HEAD" proof was the same stale binary — the record is corrected in the
CHANGELOG.)

## 4. CHANGELOG

One consolidated `Fixed` entry above the prior ones: all wave 1-3 fixes with
before/after numbers (decay t60s, drive gains, mono ceiling, level span, DC,
latency, glide profile, floor dwell, RingMod 16→2.975, tail table per-case),
the deliberately-NOT-done list (sub-32k AA rejected, Spectral latency = product
decision, ClockedDelay slew, PitchShifter glide/spread, WSOLA CPU), and the
stale-binary gate note.

## 5. Commit

**`6bd4c94`** pushed to origin/main: 34 files, +1697/−138 (lane sources + new
tests `reverb_decay_test`/`drive_calib_test` + CMake + docs + CHANGELOG +
3 lane reports). Tree clean, nothing staged. (A commit-message temp file was
accidentally staged; removed via amend pre-push.)

## Residual risks

- Chain dry-ring budget sits at exactly 32 for the theoretical 12+12+8 series
  (fits, zero headroom for future >8 OS latency in series).
- Echo glide attenuates circulating tails during long retargets (tape-authentic,
  documented). Flanger corner sound changed deliberately.
- Resonator tail normalized at 48k (44.1k rings ~8% longer than reported).
- Lane-reported docs/parvati_fv1_alias_probe historical numbers were measured
  at the old 8x-hot calibration (changelog context only, no edit made).

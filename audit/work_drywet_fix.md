# Parallel dry/wet fix — work report

## Patch (Source/dsp/fx/FxChain.cpp, renderParallel, both parallel topologies)

Per audit/drywet_investigation Bug B: computed `actDw[a] = dwCur_a * fade_a`
(per-slot EFFECTIVE dw) alongside the mean; the wet sum now scales each
branch's own wet (`sumWL += wl * actDw[a]`); the output blend drops the
shared mean from the wet path (`out = dL*dry + sumWL*inv`). The mean `W`
still drives ONLY the dry gain (`dry = 1 - W`), so the equal-gain character
at dw=1 is unchanged and bypass/fade (activeCount exit) semantics are
untouched. 21 insertions / 7 deletions incl. the explanatory comment.

## Regression tests (tests/fx_routing_test.cpp, new B10 section)

**B10a** — Parallel12to3, branch A = Echo 100 ms fb 0, B = Echo 300 ms fb 0
(param values from the real mapping `ms = 10*47^p0`: 0.598/0.883), click,
windowed peaks per branch:
- POST-FIX: A dw 1→0 falls **223.2 dB** (floor −240 dB = true silence);
  B **bit-identical** across the sweep (0.00 dB delta).
- PRE-FIX (revert-probe via stash): A delta **exactly −6.02 dB (leak)**, B
  delta **−6.02 dB (cross-branch gain jump)** — the investigation's numbers
  reproduced, proving the fix and the pin.

**B10b** — A = Echo fb 0.85 dw 0 from the start, B = plate; echo-path peak
isolated by differencing a twin chain with fb 0 (plate + dry cancel
exactly): **0.000000e+00** post-fix. (This check also passes pre-fix — the
shared-mean leak is common to both renders — B10a is the discriminator;
kept as the feedback-path pin.)

## UX note (Source/ui/ParamHelp.cpp)

`fx{N}_drywet` += "Per-slot; a delay placed before a reverb/echo keeps
sounding in that module's tail for a while after drying it out — move it
later for an immediate cut."

## Validation (repo root)

- parvati_fx_routing_test — ALL CHECKS PASSED (0 failures)
- parvati_fx_param_coverage_test — 479/479 (0 failures, 3 drifts)
- parvati_fx_modrate_test — ALL PASS
- parvati_fx_onset_regression_test — PASSED
- parvati_render_quality_test — ALL CHECKS PASSED
- parvati_multigui_test — ALL CHECKS PASSED
- parvati_paramhelp_parity_test — ALL CHECKS PASSED

## Commit

`fd5dc78` "fix(dsp): parallel-topology dry/wet — per-branch blend instead of
shared mean" (NOT pushed, per instructions). CHANGELOG Fixed entry added
with the measured numbers + the series-behaviour note.

## Residual risks

- The dry gain keeps the mean W: with one branch at dw 0 and one at 1, dry
  stays at 0.5 (the unchanged equal-gain design); anyone expecting dry=1
  when a single branch is full-wet would see the same pre-fix behaviour.
- B10b passes both pre- and post-fix (documented above); the leak class is
  pinned by B10a alone.

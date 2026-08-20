# UI-FIX GATE — final report

## 1. Discrepancy resolution (keyboard lane's 4 [19] failures)
Stale-binary ordering, as suspected. The keyboard lane ran `parvati_multigui_test` while the preview lane's display files were mid-edit/rebuild; its stash-isolation already showed the failures tracked those files. Post full rebuild from repo root: `parvati_multigui_test` plain = **0 failures**, `--windowed` (real `addToDesktop` peer — the Standalone path) = **0 failures**, including all four [19] preview pins (osc shape → gen 1→2, osc param → 2→3, env attack → ADSR refreshed, filter cutoff → response refreshed). No code conflict existed; both lanes' changes compose.

## 2. Coexistence + suite (all from REPO ROOT, post full rebuild)
Full `cmake --build build -j8`: **0 errors**.

| Test | Result |
|---|---|
| parvati_multigui_test (plain) | ALL CHECKS PASSED (0 failures) |
| parvati_multigui_test --windowed | ALL CHECKS PASSED (0 failures) |
| parvati_keyboard_view_test | PASS (0 failures) |
| parvati_seq_stepper_test | ALL CHECKS PASSED (43/43) |
| parvati_layout_minwidth_test | PASS (0 failures) |
| parvati_layout_overlap_test | PASS (0 failures) |
| parvati_ipad_hig_sizing_test | ALL CHECKS PASSED |
| parvati_lifecycle_test | ALL CHECKS PASSED |
| parvati_ui_mirror_test | ALL CHECKS PASSED |
| parvati_apvts_test | ALL CHECKS PASSED |
| parvati_host_param_text_test | ALL CHECKS PASSED |
| parvati_editor_test | EDITOR TEST: ALL CHECKS PASSED |

(CWD-relative note: multigui/ui_mirror reference `presets/FACTORY` by relative path — repo root is the correct invocation; the earlier `build/`-cwd failures both lanes saw were this artifact.)

## 3. Release Standalone
`cmake --build build_release --config Release --target Parvati_Standalone -j8`: **[100%] Built target**; binary stamped 2026-08-20 05:50 (carries all three fixes).

## 4. Commit
**`3fa83ce`** pushed to origin/main — 22 files: lane sources (PluginEditor, Envelope/FilterResponse/OscPreviewDisplay, KeyboardView, ParvatiTheme, SeqLengthStepper), tests (editor_test [19], keyboard_view_test [8], new seq_stepper_test + CMake), CHANGELOG entry (root cause + fix + evidence per item), 3 lane reports. Tree clean, nothing staged.

## Residual risks
- [19] windowed half needs a window server (local OK; headless CI still passes — windowed is opt-in).
- `keyBlack` ≈ `backgroundBase` on dark themes (sharps read by contrast/shape, as before).
- `parentHierarchyChanged` re-arms a running timer on any ancestor change — cheap, no behavior change observed.

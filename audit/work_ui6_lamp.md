# work_ui6_lamp — indicator-lamp border "a tiny bit" thicker

## Change
- `ParvatiLookAndFeel.h`: new `public: static constexpr float kLampBorderWidth = 2.5f;` on `ParvatiModuleLamp` — ONE value shared by paint + both tests.
- `ParvatiLookAndFeel.cpp` (`paintButton` only):
  - Stroke **1.5f → 2.5f** (`g.drawEllipse (r, kLampBorderWidth)`).
  - Resting ring colour **outline → outline.brighter(0.25f)** for contour legibility; hover-brighten (0.8/0.20) and disabled-alpha paths unchanged.
  - Dot geometry/diameter logic untouched.

## Tests
- `mod_matrix_ui_test`: new pin `kLampBorderWidth ∈ [2.0, 3.0]` ("slightly-thicker value") beside the existing dot-diameter parity block.
- `workspace_padding_test`: the same pin in the lamp section (one value, two pins).

## Validation (repo root)
- Build: `cmake --build build -j8 --target parvati_mod_matrix_ui_test parvati_workspace_padding_test parvati_multigui_test` — 0 errors.
- Runs: **mod_matrix_ui ALL PASS · workspace_padding ALL PASS · multigui (EDITOR) ALL PASS**.

## Commit
`2749fd4` "style(ui): slightly thicker indicator-lamp borders" (4 files, +22/−4, incl. CHANGELOG). **NOT pushed.** Nothing staged. (An in-flight `M Source/ui/PatchPage.cpp` from a sibling lane is left untouched/unstaged.)

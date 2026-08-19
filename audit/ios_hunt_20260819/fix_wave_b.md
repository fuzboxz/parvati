# iOS hunt 2026-08-19 — fixes (keys: lifecycle + touch + perf; completed by the
# orchestrator after the wave-B workers died mid-run — their verified edits
# were kept, the remainder finished + validated here)

## lifecycle (F-ios-lc-2, F-ios-lc-3, F-ios-touch-3)
1. **Reference-counted process-global editor side-effects (F-ios-lc-3).**
   File-static `sLiveEditorCount` in PluginEditor.cpp: ctor increments FIRST;
   `setScreenSaverEnabled(false)` + `ParamControl::setTapAssignActive(false)`
   fire only on the 0->1 transition; the dtor's counterparts (screensaver
   re-enable, tap-assign clear) fire only when the closing editor is the LAST
   one (count==1 pre-decrement); the decrement is the dtor's final statement.
   AUv3 processes host several Parvati instances (AUM) — closing editor A can
   no longer undo editor B's global state. Pinned by parvati_lifecycle_test
   [1] (two editors, destroy one, screensaver state + tap-assign preserved).
2. **Parked zoom trio out of focus traversal (F-ios-touch-3).** The three
   never-placed zoom buttons stayed "visible" at 0x0 extent, so Tab on an iPad
   hardware keyboard could focus an INVISIBLE button (Space would zoom;
   musical typing dead). setVisible(false) at construction (the '...' popup
   is the real UI since W9). Pinned by lifecycle test [2] (full focus-cycle
   walk asserts no zero-extent focusable).
3. **Hidden pageSelector_ tab bar out of everything (same class).** The
   0-depth TabbedButtonBar still created SYNTH/FX TabbedButtons at 0x0 —
   focusable AND collected by the HIG sweep. Hiding the BAR (one call;
   per-button hides get undone by the bar's layout pass) removes the subtree
   from visibility walks, traversal and hit-testing; setCurrentTabIndex still
   works.
4. **Interruption semantics (F-ios-lc-2).** releaseResources() was EMPTY;
   hosts tear down render resources on interruption/route change and JUCE only
   clears notes on a sample-RATE change — voices gated at the interruption
   resumed gated, note-offs lost in the window = STUCK NOTES after a phone
   call. releaseResources() now runs engine_.resetAllVoices() (the loaders'
   seam: deferred audio-thread kill, voice-activity only) +
   midiCollector_.reset(). Pinned by lifecycle test [3] (hold, release/
   prepare cycle, zero active voices, patch bytes intact, new note sounds).

## touch (F-ios-touch-1, F-ios-touch-2 — orchestrator-completed)
5. **Mod-bar nav scrollers 30x30 -> 44pt hit bands (F-ios-touch-1).**
   kNavW 30->44 (public CentralModBar::kNavHitW, pinned == 44 in the HIG
   contract test); the chevron glyph stays visually small inside the band.
   They are the ONLY way to scroll the overflowing mod-source band.
6. **SeqLengthStepper: the NUMBER is the control (F-ios-touch-2, closes the
   STOPPED T9a item).** The two ~32x20 -/+ buttons (provably unable to reach
   44pt in the 72x64 grid cell) are REMOVED. The whole cell is one tap target
   (invisible full-cell TextButton + non-intercepting number label) opening a
   1..16 picker of 44pt rows (T7 idiom: SafePointer actions, themed via
   setLookAndFeel, kPopupRowHeight pinned == 44). Desktop keyboard parity
   (up/down/+/- nudge) preserved. Pinned by lifecycle test [4] (pick writes
   the param, nudge decrements, clamps hold) — driven through the generator-
   page seam (setActiveGenerator(MOD_SRC_SEQ_1), the same chain a pill click
   drives).
7. **Mod-matrix row buttons starved to 12x44 / 0x44 (found by the new sweep).**
   The proportional combo floors consumed the row before the right-side
   removeFromRight(44)s, which silently clamped. resized() now reserves the
   FIXED action targets first (Mute/Clear/value) and lets the combos flex
   (slider keeps a 40pt floor; combos hard-floor at 44).
8. **layout_minwidth_test [5] content sweep (the gate).** Every effectively-
   visible Button below the header strip must be >= 44x44 across the 560-1800
   width sweep. Pre-fix inventory: nav 30x30, M 12x44, Clear 0-30x44, stepper
   -/+ ~32x20, SYNTH/FX tab ghosts 0x0. Post-fix: 0 undersized, 13-15 audited
   per width; allowlist intentionally EMPTY (nothing accepted as debt).

## perf (F-ios-perf-1/2/3 — worker edits verified + test gap fixed here)
9. **iOS oversampling gate (F-ios-perf-1).** Measured 96-voice worst case:
   8x = 0.93x realtime on an M-series core => guaranteed overrun on A12-class
   iPads. The Settings combo offers only 1x/2x on JUCE_IOS; setStateInformation
   clamps restored 4x/8x to 2x (desktop unchanged). Pinned by perf_smoke_test
   [F-ios-perf-1] (portable 2x<2.5*1x ratio pin + per-platform item count).
10. **Thermal awareness (F-ios-perf-2).** Pure thermalActionForLevel mapping
    {Nominal,Fair,Serious,Critical}->{None,Hint,StrongHint}; an allocation-free
    ~1 Hz NSProcessInfo.thermalState sampler (objc_msgSend from plain C++) on
    the 60 Hz timer stores an atomic thermalHint_ the editor can surface.
    NOTHING auto-changes sound. (Editor-label surfacing = documented 3-line
    follow-up; PluginEditor.cpp was owned by the lifecycle lane.)
11. **Display timers visibility-gated (F-ios-perf-3).** EnvelopeDisplay /
    FilterResponseDisplay / OscPreviewDisplay / FxSlotVisualizer / FxMatrixView
    / ModMatrixView now start/stop their 30 Hz timers in visibilityChanged()
    (pages are unparented when not current) — battery hygiene for long AUv3
    sessions.
12. **perf test sanitizer-awareness (fixed here).** The absolute "0.5x
    realtime" sanity is skipped under ASan/TSan (instrumented builds are
    2-20x slower); the portable RATIO pin still runs everywhere.

## Validation
- Full release suite: 68 PASS / 0 real failures (settings_roundtrip is an
  argument-taking harness, rc=2 = usage).
- ASan+UBSan: lifecycle / ios_file_flow / param_thread / perf_smoke /
  layout_minwidth all clean.
- TSan: parvati_concurrency_test full runs x3 — rc=0, ZERO warnings.
- iOS simulator configure: plist graft verified (CFBundleDocumentTypes +
  UTExportedTypeDeclarations present; AUv3 id patch intact).

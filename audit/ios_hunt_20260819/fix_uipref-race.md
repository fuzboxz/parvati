# Fix uipref-race — F-ios-lc-1 (HIGH, crash class): UI-preference state race

Lane: iOS hunt 2026-08-19 remediation. KEY `uipref-race`.
Files owned: Source/PluginProcessor.h, Source/PluginProcessor.cpp (UI-pref
family only), tests/concurrency_test.cpp (one new section). No other files
touched (sibling agents concurrently own PluginEditor.cpp / PresetBrowser.h /
FactoryPresetInstaller.* / CMakeLists.txt / parvati_tests.cpp — left alone).

## The bug (verified against the working tree at b889205 + current)

`getStateInformation`/`setStateInformation` read/wrote the UI-preference
members with NO synchronization:
  uiThemeName_ (juce::String — refcounted heap string, torn copy = UAF class)
  uiLanguage_  (juce::String), uiZoom_ (double), uiTooltips_/uiSmoothing_ (bool),
  uiOversampling_ (int), uiFontMode_ (int legacy)

Writers run on the message thread (SettingsPanel.cpp:39 setUiTheme;
PluginEditor setUiLanguage/setUiZoom; setParameterSmoothing; setOversamplingFactor;
rebuildOsLatencyProbe read uiOversampling_). AUv3 hosts call
get/setStateInformation on NON-message threads (AUM/GB session saves +
background autosaves — juce_audio_plugin_client_AUv3.mm getFullState path).
A theme switch concurrent with a host save = torn refcounted String (UAF) or
torn double persisted as garbage zoom.

## The fix (no behavior change — synchronization only)

- Added `mutable std::mutex uiPrefsLock_` next to the member group.
- All 6 getters / 6 setters now lock+copy (getters: `noexcept` dropped —
  locking + String copy are throwing operations; the old `noexcept` on a
  String-returning getter was already a latent terminate-on-OOM).
- getStateInformation: ONE lock acquisition snapshots all 7 values to locals,
  serialized from the locals (juce::String copies made under the lock).
- setStateInformation: tree properties parsed to locals first, then ONE lock
  acquisition commits all 7.
- Direct member accesses in the .cpp all rerouted:
  ctor :176 → setOversamplingFactor (getUiOversampling());
  restore tail → engine_.setParameterSmoothing (getUiSmoothing()) /
                 setOversamplingFactor (getUiOversampling());
  setParameterSmoothing → setUiSmoothing (locked setter);
  setOversamplingFactor → setUiOversampling (locked setter);
  rebuildOsLatencyProbe → reads getUiOversampling() into a local (its single
  caller passes the same value it just persisted — consistent by construction).
- Grep proof: `uiThemeName_|uiLanguage_|uiZoom_|uiTooltips_|uiSmoothing_|uiOversampling_|uiFontMode_`
  in PluginProcessor.cpp now hits ONLY lines inside the two explicit
  lock_guard scopes (serialize snapshot :1502-1508, restore commit :1555-1561).

## Deterministic test — tests/concurrency_test.cpp new section [7]

"UI-pref state save/restore vs message-thread setters (F-ios-lc-1)":
- Thread A (host stand-in) loops getStateInformation into a MemoryBlock.
- Main thread storms setUiTheme/setUiLanguage/setUiZoom/setUiTooltips/
  setUiOversampling alternating — time-bounded (>= 250 ms AND >= 4000 iters)
  so overlap is machine-independent (release run measured 1267 saves inside
  the storm; TSan run 22).
- Asserts: saves > 0; setter/getter round-trips (theme/language/zoom); FULL
  get->set state round-trip preserves theme+language+zoom into a second
  processor; renders finite audio after the storm.

## Evidence

1. Release build + run (build_release, -j4, target parvati_concurrency_test):
   CONCURRENCY TEST: ALL CHECKS PASSED (0 failures) — incl. [7] all ok.
2. FULL-REPO TSan (own dir build_ios_fx_uipref-race, PARVATI_ENABLE_TSAN=ON):
   `TSAN_OPTIONS=halt_on_error=0 ./parvati_concurrency_test` → rc=0,
   **0 ThreadSanitizer warnings** (whole binary: my section + every prior
   section), [7] passes with 22 overlapping saves.
3. PRE-FIX failure-class proof (repo code could not be reverted in-place —
   sibling agents own neighboring files; a faithful minimal TSan repro of the
   exact unfixed shape was compiled with -fsanitize=thread):
   - FIXED=0 (unlocked String/double read on thread A vs reassign on thread B):
     `WARNING: ThreadSanitizer: data race` (repro_unfixed).
   - FIXED=1 (mutex-guarded, the shipped shape): no warning (repro_fixed).
   The unfixed accessors were exactly the FIXED=0 shape (plain member
   read/assign, no lock) — grep of `git show b889205:Source/PluginProcessor.h`
   confirms lines 92-105 had no synchronization.
4. Regression: parvati_host_state_test + parvati_editor_test rebuilt and PASS
   (the state round-trip and editor surfaces exercise the changed accessors).

## Residual risks

- Mutex acquisition cost on the getters is an uncontended lock (~tens of ns);
  worst observed call frequency is the editor's 30 Hz timer reading a few
  prefs — negligible.
- `noexcept` removal on the getters is ABI-relevant only for in-header callers
  compiled separately (none rely on noexcept semantics; whole tree rebuilds
  clean).
- The TSan full-repo run used the tree WITH sibling in-flight edits (their
   compile error at 01:47 was fixed by 01:52); zero warnings covers my change
   and theirs as of that build.

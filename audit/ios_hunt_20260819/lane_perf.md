# iOS/iPadOS Perf Hunt — lane `ios-perf` (2026-08-19)

Repo /Users/fuzboxz/parvati @ b889205 (post-W11). Read-only hunt; measurements
via self-built harnesses linked read-only against build_release artifacts
(objects/binary in /tmp, no shared-dir builds, no tree writes outside this file).

Method notes: desktop (M-series, -O3 -DNDEBUG) numbers give RELATIVE cost of
platform-independent shared code; absolute iOS numbers depend on the SoC
(A12 = deployment-target 14.0 floor) and are marked UNKNOWN-NEEDS-DEVICE where
they matter. Harnesses: /tmp/ios_bh_perf_harness.cpp (OS-factor + idle render),
/tmp/ios_bh_scan_harness.cpp + /tmp/ios_bh_scanbreak_harness.cpp (preset scan).

## F-ios-perf-1: No iOS gate on filter-oversampling — default 2x, UI offers 8x on iPad; 8x ≈ realtime even on desktop silicon at worst-case polyphony
- Source/PluginProcessor.h:399 (`int uiOversampling_ { 2 }` — no JUCE_IOS branch); Source/PluginProcessor.cpp:170-176 (default applied at construction); Source/ui/SettingsPanel.cpp:156-166 (combo offers Standard 1× / High 2× / Maximum 4× / Ultra 8× on every platform); Source/AmbikaVoice.cpp:576/644 (the per-voice processSamplesUp/down + filter at 40×factor samples per internal block).
- severity: high (audio overrun / dropout class on iPad)
- class: perf
- evidence: measured with the repo's own engine (96 voices = 6 parts × 16 slots, dense chord, 48 kHz, 256-sample blocks, release build): OS 1x = 0.13× realtime, 2x = 0.26× (2.0× the 1x cost), 4x = 0.48× (3.7×), 8x = 0.93× (7.1×). Scaling to an A12-class core (≈2.5–4× slower than this M-series core for this float/filter workload — UNKNOWN-NEEDS-DEVICE for the exact factor): 8x ⇒ ~2.3–3.7× realtime (guaranteed overrun at max polyphony), 4x ⇒ ~1.2–1.9× (overrun at 96 voices), 2x ⇒ ~0.65–1.04× (borderline at absolute worst case, fine at typical ≤32 voices). The 96-voice worst case is reachable from the UI (6 parts × Voices 16 + long releases). No JUCE_IOS/PARVATI_IOS gate exists on the default, the ceiling, or the combo items anywhere (grep: only the plist/device-family gates use PARVATI_IOS).
- deterministic_check: (a) portable regression: extend tests/perf_smoke_test-style harness asserting render cpu-ratio(8x, 96 voices) < 1.5× and cpu-ratio(2x, 96 voices) < 0.8× on CI hardware (catches future per-voice cost regressions that push iOS over the line); (b) once a gate lands, a static check that SettingsPanel's item list on JUCE_IOS excludes 8x and the ctor default is 1x/2x (grep-level scanner in tools/, like check_async_this).
- verdict: REAL BUG (platform-appropriateness), remediation is a product decision (default + ceiling) → recommend: JUCE_IOS default 2x retained only if a load-based guard lands, else 1x; cap the iOS combo at 2x (4x opt-in "experimental"); keep desktop unchanged.

## F-ios-perf-2: Zero thermal-state awareness — no observer, no backoff, no user hint on iOS
- grep over Source/, CMakeLists.txt, ios/: no `thermalState`, `NSProcessInfo`, `ProcessInfoThermalState`, `didReceiveMemoryWarning` anywhere (only the CPU% readout exists: Source/PluginEditor.cpp:3394-3397 statusLoadLabel_).
- severity: medium (sustained-session dropout with no signal; not a crash)
- class: perf
- evidence: sustained 6-part multitimbral play at 2x OS keeps an iPad core near its budget (F-1 numbers); iOS responds by throttling (and eventually jetsam-ing foreground apps' CPU), which manifests as intermittent crackle the user cannot attribute. The app already has the two ingredients a backoff needs: an audio-load probe (getAudioLoadCurrent) and a live oversampling setter (setOversamplingFactor, MT-safe staged). Nothing observes ProcessInfo.processInfo.thermalState.
- deterministic_check: once implemented — a unit test that the thermal observer's decision function maps {nominal, fair, serious, critical} → {no-op, suggest, downgrade} deterministically (pure function, no device needed); on-device validation listed as UNKNOWN-NEEDS-DEVICE.
- verdict: BY-DESIGN-ABSENT → backlog recommendation, NOT a silent fix. Suggested shape: slow timer (≤1 Hz) reading thermalState on iOS only; at .serious surface a transient status "Thermal: reduce Filter Quality"; never auto-change sound-affecting state without user opt-in except at .critical where the audio thread is already failing.

## F-ios-perf-3: ~8–12 display components poll at 30 Hz with no visibility/page gate while the editor is open
- Source/ui/EnvelopeDisplay.cpp:36, FilterResponseDisplay.cpp:46, OscPreviewDisplay.cpp:101, FxSlotVisualizer.cpp:68 (×3 cards), FxMatrixView.cpp:631, ModMatrixView.cpp:558 — all startTimerHz(30) at construction; none override visibilityChanged()/check isShowing() (grep: no such gate in those files). The editor's own timer IS adaptive (30 Hz busy / 4 Hz idle — Source/PluginEditor.cpp:3548-3575). The processor's 60 Hz DeferredParamTimer (Source/PluginProcessor.cpp:168,86-100) keeps running with the editor closed (AUv3 background) doing a reap walk over 6 chains + 96 voices + an empty drain.
- severity: low (hygiene/battery; not a malfunction)
- class: perf
- evidence: every 30 Hz callback is change-only (eps-gated fetches + conditional repaint — verified in FxSlotVisualizer.cpp:82-110, EnvelopeDisplay.cpp:44-70, FilterResponseDisplay.cpp:48-66), so idle paint cost is ~zero; JUCE dispatches all juce::Timer callbacks from ONE timer thread (coalesced wakeups, not N wakeups), and juce::Timer is not display-linked (no ProMode/120 Hz forcing). Residual cost: ~10 components × ~7 param fetches × 30 Hz ≈ 2k atomic/APVTS reads/s + the wakeup cadence — measurable only in long battery-sensitive sessions. The editor-closed 60 Hz processor timer costs ~100 atomics/tick (negligible but always-on for the AUv3 process lifetime).
- deterministic_check: none meaningful headlessly for battery; a code-level scanner asserting each *Display class either gates its timer on isShowing() or documents why not (grep for visibilityChanged in the class) once remediated.
- verdict: BY-DESIGN (accepted pattern) with a cheap improvement available — start/stop the per-display timers in visibilityChanged() (the TabbedComponent already unparents non-current pages, which fires it).

## F-ios-perf-4: PresetBrowser first scan is fine (14–18 ms warm) but scanRecursiveInto is unbounded; a large/linked USER tree walks it ALL on the message thread
- Source/ui/PresetBrowser.h:176-195 (scanRecursiveInto — no depth bound, no entry-count cap, no symlink refusal); first scan cost measured with the repo's real tree: 14–18 ms warm / 358 .PRO parses (cached rebuild 0.07 ms; parse count 358 confirmed via debugParseCount). iOS cold-cache estimate ≈ 50–150 ms one-time at the first preset-menu tap per editor instance (UNKNOWN-NEEDS-DEVICE) — acceptable, W10 cache working as designed.
- severity: low–medium (message-thread stall proportional to tree size; app-managed tree is small today)
- class: perf | files
- evidence: my scan harness accidentally pointed userDir at ~/Documents and measured 3,866–5,131 ms for the SAME 358-file factory tree — the entire cost was scanRecursiveInto walking a big user tree. The app's userDir is <shared-container>/Parvati/USER (Source/PluginProcessor.cpp:1127-1133), which the app manages; exposure is user-copied folder structures (Files app into the mirrored Documents, then anything that lands under USER/) and desktop users who move a big library in.
- deterministic_check: editor_test extension: build a USER tree nested 200 levels deep (and one with 5,000 files), call buildMenu, assert scan completes < 100 ms — pre-fix the deep one can stack-overflow / the wide one stalls; post-fix (depth cap ~8, entry cap, symlink skip) both pass. Also assert the factory scan stays < 50 ms against presets/ (regression pin).
- verdict: REAL BUG (latent, low likelihood / high impact when hit).

## F-ios-perf-5: FactoryPresetInstaller walks all ~365 embedded resources (stat each) on EVERY process start; first launch writes 1.7 MB synchronously in the AUv3 constructor
- Source/ui/FactoryPresetInstaller.cpp:65-77 (call_once per process), :104-121 (writeIfMissing per resource unconditionally — deliberate self-heal, comment documents the dead-gate history), :126-160 (template sync); called from the processor ctor Source/PluginProcessor.cpp:161.
- severity: low (instantiate-time latency only; correctness is fine and self-healing is intentional)
- class: perf | lifecycle
- evidence: steady state = ~365 stats + template diff at each first-construction-per-process (AUv3: once per host process). On iOS flash that is plausibly 30–150 ms inside plugin instantiate (UNKNOWN-NEEDS-DEVICE); first launch additionally writes 365 files (~1.7 MB) on the message thread. A version-marker file (write "installed=vN" after a completed pass; skip to a single stat when present and version matches) reduces steady state to 1 stat while keeping the self-heal (marker deleted on version bump).
- deterministic_check: installer unit test counting filesystem ops via a temp-dir shim: second construction with marker ⇒ ≤ 2 stats; corrupt-marker / missing-file cases still self-heal (write-if-missing re-runs).
- verdict: BY-DESIGN (self-healing) with a cheap optimization; LOW.

## Verified-OK (no finding)
- Idle render CPU: measured 0.0017× realtime (no notes, OS 2x, 2.13 s audio in 3.6 ms) — the fast-bypass path (Source/dsp/fx/FxChain.cpp:598-604: no active slot + no latency + master no-op + no EQ ⇒ transparent dry copy) and per-voice idle gating do their job; pinned behaviourally by tests/idle_silence_test.
- Memory footprint vs iOS extension limits: static tables ≈ 0.5 MB (resources_data.cpp 18.7 KB + clouds/warps resource tables), binary __TEXT 5.6 MB (includes embedded factory presets ~704 KB + logo), worst-case FX heap = 18 slots × 512 KB (FxLoopingDelay/FxWSOLAStretch bufMem_ — Source/dsp/fx/FxProcessors.h:151-201) ≈ 9.2 MB + 96 voices' state/OS objects ⇒ ≈ 12–18 MB total worst case — far below any AUv3 extension jetsam budget. No leak-shaped growth (retired processors/OS objects are parked in bounded rings reaped at 60 Hz).
- Startup editor construction: no scan at construction (scan is lazy at the first preset-menu tap — PresetBrowser ctor does no I/O); W10 cache prevents per-open rescans within an editor instance.

## Count summary
6 findings: 1 high (F-1), 1 medium (F-2), 2 low–medium/low (F-4, F-5), 2 informational/verified-OK sections (F-3 low, plus the verified-OK block). No crash-class bug found in this lane.

## TOP 3 remediation priorities
1. F-ios-perf-1 — iOS gate on oversampling (default + ceiling): the only finding that can actually drop audio on target hardware at default settings with high polyphony; needs a product decision (suggest: iOS combo caps at 2×, default stays 2× only with the F-2 load guard, else 1×) + the portable cpu-ratio regression pin.
2. F-ios-perf-2 — thermal observer + user-visible hint (iOS-only, ≤1 Hz poll, pure-function decision unit-tested); pairs with F-1 as the runtime guard.
3. F-ios-perf-4 — bound scanRecursiveInto (depth ≤8, entry cap, symlink skip) + the editor_test timing pin; smallest change, removes the only unbounded message-thread walk in the preset path.

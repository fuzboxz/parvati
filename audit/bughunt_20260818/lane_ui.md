# Bug Hunt 2026-08-18 — Lane: UI lifetime & threading (reviewer, read-only)

Scope: Source/PluginEditor.{cpp,h}, Source/ui/*.{cpp,h}, Source/ui/FactoryPresetInstaller.*.
JUCE internals verified against the vendored modules at ~/JUCE (juce_FileChooser.cpp,
juce_Component.{h,cpp}, juce_PopupMenu.cpp, juce_AudioProcessorValueTreeState.cpp,
juce_audio_plugin_client_VST3.cpp / _AUv3.mm / _AU_1.mm).
Grep surface confirmed: NO MessageManager::callAsync, no ThreadPool/std::thread/std::async in
Source/ — the async surface is juce::Timer (7 sites), FileChooser completions (6, all
allowlisted), DialogWindow/launchAsync (TuningEditor, MulExportDialog), AsyncUpdater
(FxSlotCard, FxFlowDiagram), PopupMenu showMenuAsync (all SafePointer-guarded).

## F-ui-1: ParamControl/NoteStepControl APVTS listeners mutate GUI state on the audio thread
- file:line — Source/PluginEditor.cpp:560-573 (ParamControl::parameterChanged), :574-586
  (refreshStepEnabled → slider_->setEnabled :584 + repaint :585), :767-800
  (applyModSourceTint → comboBox_->setColour :791), :812-906 (refreshModRing →
  slider_->getProperties().set :898 + slider_->repaint); registration at :399, :462,
  :484-486. Source/ui/NoteStepControl.cpp:80-92 (slider_->setValue + repaint, no thread
  check); registration :32.
- severity: high (crash/UAF-class: audio-thread → GUI object mutation; debug jassert, release
  data race)
- evidence: APVTS listeners fire synchronously on whatever thread writes the parameter.
  Two verified delivery paths put that thread INSIDE processBlock / host render:
  1) In-plugin MIDI path: PluginProcessor.cpp:314 runs midiParamMap_.handleBuffer() inside
     processBlock; its setter (PluginProcessor.cpp:148-155) calls
     p->setValueNotifyingHost(...) → AudioProcessorParameter::sendValueChangedMessageToListeners
     (juce_AudioProcessorParameter.cpp:59-63) → APVTS ParameterAdapter::parameterValueChanged →
     listeners.call (juce_AudioProcessorValueTreeState.cpp:148-159). MidiParameterMap.cpp:263+
     maps NRPN addresses to ordinary patch params — including mod-matrix amounts (the INT8
     comment names "mod amounts") — so a hardware-style NRPN tweak of mod1_amount fires
     refreshModRing() on every registered mod-dest knob ON THE AUDIO THREAD while the editor
     is open.
  2) Host automation: VST3 processParameterChanges calls setValueAndNotifyIfChanged →
     setValueNotifyingHost from process() (juce_VST3.cpp:827-834, 3534-3537); AUv3 host
     setValue does sendValueChangedMessageToListeners on the render thread
     (juce_audio_plugin_client_AUv3.mm:1436-1443); AU SetProperty the same
     (juce_audio_plugin_client_AU_1.mm:1180-1187).
  refreshStepEnabled/applyModSourceTint/refreshModRing then call Component::setEnabled /
  setColour / NamedValueSet::set / repaint from that thread. Component::repaint asserts
  message-thread-only in this JUCE (juce_Component.cpp:1877-1881,
  JUCE_ASSERT_MESSAGE_MANAGER_IS_LOCKED); in release these writes race the paint thread
  reading the same slider properties/flags.
  The codebase already knows this contract: ParvatiAudioProcessor::parameterChanged defers
  off-message-thread arp/seq/fx/part_select edits to the 60 Hz drain
  (PluginProcessor.cpp:469-498, comment: "host automation on the render thread, or the
  CC/NRPN map inside processBlock"); FxFlowDiagram::parameterChanged thread-checks and
  defers (Source/ui/FxRoutingBar.cpp:239-248); FxSlotCard::parameterChanged always defers
  via AsyncUpdater (Source/ui/FxSlotCard.cpp:613-619). ParamControl and NoteStepControl are
  the two listeners missing the guard.
- deterministic_check: extend tests/editor_test.cpp (construct/destroy pattern at :51, :1011):
  open an editor; from a std::thread simulating the audio thread (as
  tests/concurrency_test.cpp:471-475 does for the processor), call
  proc.getApvts().getParameter("mod1_amount")->setValueNotifyingHost(...) while the editor
  is alive. Debug build: JUCE_ASSERT_MESSAGE_MANAGER_IS_LOCKED fires inside
  Component::repaint (reachable from ParamControl::refreshModRing). TSAN/ASAN build:
  data race on the slider's NamedValueSet concurrently read by paint. Repeat with
  "seqnote_step0" (NoteStepControl) and "seq_length_1" (refreshStepEnabled).

## F-ui-2: MulExportDialog borrows the launching editor's LookAndFeel into a desktop window that can outlive the editor → UAF paint
- file:line — Source/ui/MulExportDialog.cpp:258-261 (content->setLookAndFeel
  (&parent->getLookAndFeel()) with the comment "The parent outlives the modal dialog"),
  :274 (o.launchAsync()); trigger Source/PluginEditor.cpp:4370-4378 (openSaveMultiDialog →
  MulExportDialog::launch (this, ...)); teardown Source/PluginEditor.cpp:3159-3198
  (~ParvatiEditor destroys lnf_; no dismissal of open DialogWindows).
- severity: high (crash/UAF)
- evidence: the DialogWindow created by LaunchOptions::launchAsync is its own desktop
  window; nothing ties its lifetime to the launching editor (no related component, no
  parent watch). If the host closes the plugin window while the .MUL export fallback
  dialog is open (or an AUv3 view controller is torn down mid-dialog), ~ParvatiEditor
  runs, member lnf_ is destroyed — but the still-open dialog content retains the raw
  setLookAndFeel(&lnf_) pointer. Every subsequent paint/hover of the dialog
  (drawComboBox/drawButtonText through getLookAndFeel()) reads freed memory. The Done
  callback itself IS SafePointer-guarded (PluginEditor.cpp:4371-4376), so only the
  rendering side dangles. The codebase's own precedent documents and fixes exactly this
  hazard: TuningEditor.h:73-93 ("the dialog is its OWN desktop window (launchAsync), so it
  can OUTLIVE the editor … which would leave the dialog painting through a freed L&F. The
  content therefore OWNS a ParvatiLookAndFeel COPIED from the parent's active theme");
  TuningEditor.cpp:386-394 + dtor :256-263. MulExportDialog predates/lacks that pattern —
  the "parent outlives the modal dialog" comment is an unverified assumption, not a
  guarantee JUCE provides.
- deterministic_check: tests/editor_test.cpp pattern: createEditor(); build a
  parvati::mul_export::Setup that needsFallback(); MulExportDialog::launch (editor, setup,
  names, {}); then `delete editor;` and pump the message loop (or explicitly repaint the
  currently-modal component). ASAN build reports use-after-free in
  ParvatiLookAndFeel::drawComboBox/paint paths; without ASAN the same repro in Debug
  usually trips the ~LookAndFeel "still using it" jassert path once a weak ref remains.

## F-ui-3: Editor teardown does not dismiss open popup menus — brief window where MenuWindows paint through the destroyed ParvatiLookAndFeel
- file:line — Source/PluginEditor.cpp:3159-3198 (~ParvatiEditor has no
  juce::PopupMenu::dismissAllActiveMenus()); menus holding &lnf_: PluginEditor.cpp zoom
  overflow popup (`m.setLookAndFeel (&lnf_)`, onClick of zoomOverflowButton_ ~:2440),
  Source/ui/FxSlotCard.cpp:206-207 (`menu.setLookAndFeel (&getLookAndFeel())` + submenus
  :213), and every ComboBox dropdown (JUCE ComboBox::showPopupMenu internally does
  menu.setLookAndFeel(&getLookAndFeel()) → &lnf_).
- severity: low (latent)
- evidence: PopupMenu L&F is picked up ONLY via PopupMenu::setLookAndFeel
  (juce_PopupMenu.cpp:1422-1425 findLookAndFeel → menu.lookAndFeel.get();
  Options::withTargetComponent does NOT set it — juce_PopupMenu.cpp:2131-2139). The
  MenuWindow then holds a raw Component::setLookAndFeel pointer to lnf_. JUCE partially
  self-heals: MouseSourceState runs a 20 Hz timer whose windowIsStillValid() compares the
  WeakReference target to options.getTargetComponent() and dismisses when the target died
  (juce_PopupMenu.cpp:806-815, 1478-1518). But an OS-driven paint of the open menu inside
  that ≤50 ms window (or before the first timer tick) calls getLookAndFeel() on freed
  memory. Calling PopupMenu::dismissAllActiveMenus() (juce_PopupMenu.cpp:2380-2393) at the
  top of ~ParvatiEditor — as many JUCE plugins do — closes the window deterministically.
- deterministic_check: headless: open editor; invoke the zoomOverflowButton_'s popup via
  its showMenuAsync seam (or open a ComboBox popup); `delete editor;` immediately; then
  force one dispatch/paint of remaining desktop components. Debug+ASAN build flags the
  freed-lnf read in drawPopupMenuBackground if the dismiss timer has not yet fired.

## F-ui-4: Process-global ParamControl statics corrupt the surviving editor's state in multi-instance use
- file:line — Source/PluginEditor.cpp:139-144 (static tooltipsEnabled_/modDragActive_/
  tapAssignActive_/tapSelectedSource_/transientStatusText_/transientStatusFrames_),
  :151-155 (paramControlRegistry() function-local static spanning instances), :3181
  (~ParvatiEditor → ParamControl::setTapAssignActive (false)), timer drain at
  timerCallback (:3287+ `ParamControl::tickTransientStatus()`), :708-716
  (setTapAssignActive clears selection + affordance globally).
- severity: medium (visible glitch/state corruption across simultaneous instances)
- evidence: with two live editors (multi-instance host, or Standalone + AUv3 in the shared
  app-group process — the documented SharedContainer setup): (a) closing editor A while
  its [MAP] is engaged calls setTapAssignActive(false), which flips the global flag and
  sweeps the GLOBAL registry — editor B's knobs lose the assign affordance while B's
  modAssignButton_ remains visually toggled ON (per-button state is never re-synced; there
  is no cross-instance notification); (b) both editors' 30 Hz timers drain the single
  global transientStatusFrames_ counter, halving every transient status's lifetime.
  The statics are message-thread-only (no data race), so this is a state-consistency bug,
  not a crash. ModMatrixHighlight::instance() has the same sharing but is explicitly
  documented + SafePointer-guarded as accepted cosmetic (Source/ui/ModMatrixHighlight.h:6-15).
- deterministic_check: tests pattern (tests/multigui_test.cpp exists for multi-editor): open
  two editors on one processor-independent pair; ParamControl::setTapAssignActive(true)
  via editor A's modAssignButton_ triggerClick; destroy editor A; assert editor B's
  ParamControl::tapAssignActive() == true (currently false) and/or B's button toggle state
  matches the static. For the drain: postTransientStatus("X", 90) with both timers running
  at 30 Hz and assert the text survives ≥ 90 ticks of wall time (currently ~45).

## F-ui-5: ParamControl context menu's LookAndFeel claim does not hold in this JUCE — menu renders through the default L&F
- file:line — Source/PluginEditor.cpp:1336-1338 (comment: "withTargetComponent(this) so the
  menu inherits the editor's ParvatiLookAndFeel (themed colours + font) instead of the
  default L&F") vs juce_PopupMenu.cpp:1422-1425 + 2131-2139 (menu L&F comes only from
  PopupMenu::setLookAndFeel; withTargetComponent stores only a WeakReference target for
  positioning/deletion-watch). Contrast the codebase's correct usages: zoomOverflowButton_
  popup (PluginEditor.cpp ~:2440) and FxTypeCombo (Source/ui/FxSlotCard.cpp:206-213)
  explicitly call setLookAndFeel; JUCE's own ComboBox does too.
- severity: low (latent, cosmetic — unthemed Reset/Randomize popup; the comment documents
  an invariant the code does not establish, and it masks what would be the fix for F-ui-3
  hygiene: an explicit setLookAndFeel would at least make the theming deterministic)
- evidence: verified mechanism from the vendored source; MenuWindow::setLookAndFeel
  resolves menu.lookAndFeel.get() which is only ever set by PopupMenu::setLookAndFeel, so
  the withTargetComponent-only context menu falls back to the default LookAndFeel. No
  lifetime consequence (the default L&F is process-owned), purely rendering/doc drift.
- deterministic_check: headless: build the ParamControl menu via its popupTooltipWindow_
  path is not needed — assert instead in a themed unit check: create editor; capture the
  MenuWindow created for a ParamControl popup (ModalComponentManager currently-modal) and
  check getLookAndFeel() resolves to ParvatiLookAndFeel (currently it will not).

## Verified clean (no finding)
- Allowlist re-verified (tools/check_async_this_allowlist.txt, all 6 entries):
  FileChooser::finished moves the callback out (std::exchange) and resets pimpl BEFORE
  invoking it (juce_FileChooser.cpp:269-278), and ~FileChooser cancels pending ops
  (:130-133) — so the member-owned choosers (PluginEditor.cpp:4218/4263/4303/4345,
  TuningEditor.cpp:181/192) can never fire after their owner dies; destroying the chooser
  INSIDE its own callback (all sites set fileChooser_/chooser_ = nullptr at the end) is
  safe post-exchange, and no site touches `fc` afterwards (TuningEditor copies results and
  loads file text before the reset). Reassigning fileChooser_ while a dialog is open
  destroys the old chooser → cancels its pending completion. The allowlist is accurate.
- Teardown order in ~ParvatiEditor (PluginEditor.cpp:3159-3198): stopTimer first;
  releaseAllNotes + noteCallback nulled before keyboardView_ dies; theme listener removed
  and setLookAndFeel(nullptr) BEFORE lnf_/themeManager_ member destruction — ParamControls
  only INHERIT the L&F (no explicit pointer), so children re-resolve to the default L&F.
  lnf_ is destroyed before generatedPages_ (reverse declaration order) but that window is
  synchronous on the message thread — no paint can interleave.
- Non-owned reparenting model is sound under JUCE 9: parents do NOT delete children
  (juce_Component.h:62-67); verified for GroupPager (page_), SynthWorkspace/FxWorkspace
  Viewports (setViewedComponent(..., false) / releaseActiveEditor), pageSelector_ tabs
  (addTab(..., false)), and the declaration-order discipline for
  modMatrixView_/fxMatrixView_/fxSlotCards_/fxRoutingBar_ vs their hosting workspaces
  (PluginEditor.h comments match the actual member order). patchPage_ (declared first →
  destroyed last) outlives the Global ParamPage it hosts non-owned; ParamPage's
  external-decoration contract is respected by that order.
- graphCategoryBindings_ raw-pointer closures (PluginEditor.cpp:~2525-2560) are destroyed
  before the ParamPages owning the displays (declared later than generatedPages_) and are
  only invoked from applyAllColoursFromTheme (ctor/theme change) — no async path.
- All popup menu item actions, MulExportDialog strategy callback, TuningEditor change
  callback, and PresetBrowser leaves are SafePointer-guarded
  (PluginEditor.cpp:2358-2361, 4371-4376; Source/ui/PresetBrowser.h addLeaf;
  Source/ui/PatchPage.cpp:754-765; Source/ui/FxSlotCard.cpp:216-221).
- ModMatrixView/FxMatrixView: 30 Hz timers stopped in dtors; highlight-bus subscriptions
  SafePointer-guarded AND unsubscribed; ModMatrixRow removes every MouseListener/Slider/
  ComboBox listener and clears the slider L&F in its dtor (ModMatrixView.cpp:334-345);
  the mute-restore APVTS writes in ~ModMatrixView/~FxMatrixView happen while
  generatedPages_ (ParamControls) is still alive.
- FxSlotCard/FxFlowDiagram parameterChanged correctly defer via AsyncUpdater /
  thread-check (the pattern F-ui-1 says ParamControl should copy).
- KeyboardView: releaseAllNotes + focusLost/keyStateChanged stuck-note guards;
  per-source note map cleaned in mouseUp; no `this` escapes into async contexts.
- Timers (EnvelopeDisplay, OscPreviewDisplay, FilterResponseDisplay, FxSlotVisualizer,
  editor 30/4 Hz, ParamControl long-press 450 ms): every one is a private juce::Timer
  member whose owner stops it in its own dtor (or ~Timer does); no cross-object `this`
  capture beyond the owner.
- Editor/processor teardown: processor's DeferredParamTimer is stopped/reset in
  ~ParvatiAudioProcessor before engine teardown (PluginProcessor.cpp:183-187); editor
  destruction does not reach the processor; undo/undoSafe paths are processor-side.

## Count summary
5 findings: 2 high (F-ui-1 audio-thread GUI mutation in ParamControl/NoteStepControl
listeners; F-ui-2 MulExportDialog borrowed-L&F UAF), 1 medium (F-ui-4 multi-instance
global-static state corruption), 2 low (F-ui-3 open-popups-at-teardown L&F window;
F-ui-5 context-menu L&F claim/doc drift). Allowlist entries (6/6) re-verified accurate;
async-this capture surface beyond the allowlist: none unguarded found.
```

---

## 10-line summary

1. No `callAsync`/`std::thread`/`ThreadPool` anywhere in Source/ — async surface is Timers (7), FileChoosers (6, allowlisted), DialogWindows, AsyncUpdaters, async PopupMenus; every async menu/dialog callback is SafePointer-guarded.
2. **F-ui-1 (high)**: `ParamControl::parameterChanged` (PluginEditor.cpp:560) mutates GUI (`slider_->setEnabled`, `setColour`, slider properties, `repaint`) with no thread guard, while two verified paths deliver it on the audio thread: the in-processBlock NRPN map (PluginProcessor.cpp:314 → :148-155 `setValueNotifyingHost`) and VST3/AU/AUv3 host automation (`processParameterChanges` → `setValueNotifyingHost`). Debug jasserts; release races paint.
3. `NoteStepControl::parameterChanged` (NoteStepControl.cpp:80-92) has the same defect (`slider_->setValue` + `repaint`).
4. The codebase's own correct patterns prove intent: processor defers arp/seq/fx/part_select (PluginProcessor.cpp:469-498), `FxFlowDiagram` thread-checks (FxRoutingBar.cpp:239-248), `FxSlotCard` defers via AsyncUpdater.
5. **F-ui-2 (high)**: `MulExportDialog::launch` sets the dialog content's L&F to the launching editor's `lnf_` ("The parent outlives the modal dialog" — false; it's an independent desktop window). Host closing the editor mid-dialog → every later paint reads freed `ParvatiLookAndFeel`. TuningEditor already documents+fixes this exact hazard with an owned L&F copy.
6. **F-ui-3 (low)**: `~ParvatiEditor` doesn't dismiss open popup menus; JUCE's 20 Hz target-death timer dismisses them ≤50 ms after the target dies, leaving a narrow paint-through-freed-lnf window for combo dropdowns and the two explicitly-setLookAndFeel popups.
7. **F-ui-4 (medium)**: process-global `ParamControl` statics (registry, tapAssign, transient status) corrupt a surviving editor in multi-instance use: closing editor A force-clears B's [MAP] mode while B's button stays ON; dual 30 Hz timers double-drain transient statuses.
8. **F-ui-5 (low)**: `showContextMenu`'s `withTargetComponent` does not theme the menu in this JUCE (L&F only via `PopupMenu::setLookAndFeel`) — comment/code drift, cosmetic.
9. All 6 allowlist FileChooser entries re-verified accurate against the vendored JUCE (callback moved out before invoke; dtor cancels; safe in-callback self-destruct).
10. Verified-clean: editor teardown order (timer stop, L&F reset before member destruction), non-owned reparenting under JUCE 9's parents-don't-delete-children model, graph-tint binding lifetimes, matrix-view subscriptions/listeners, KeyboardView stuck-note guards, all preview-timer dtors.

Output artifact: /Users/fuzboxz/parvati/.pi/subagents/artifacts/fbdfdc39_reviewer_0_output.md
# Lane W10 — Bug hunt of commit 0a6b9e8 ("W10 preset-menu cache + FX seeding seam")
Parent: 444fda4. Reviewed at HEAD = 0a6b9e8. JUCE = 9.0.0 at ~/JUCE.
(Reviewer had no write tool; artifact persisted by supervisor from the agent's response.)

## F-w10-1: Arrow-key navigation on the focused FX type combo changes fx{N}_type WITHOUT seeding
- Source/ui/FxSlotCard.cpp:157-231 (FxTypeCombo overrides only showPopup()), :189 (only onUserPick_ call), :385 (seam install); ~/JUCE juce_ComboBox.cpp:455-467 (keyPressed), :299-306 (nudgeSelectedItem), :289-292, :64/:443 (non-editable combos take keyboard focus)
- severity: high
- evidence: keyboard up/down on the focused combo goes through ComboBox::keyPressed → nudgeSelectedItem directly — no popup opens, the showPopup override never runs, onUserPick_ never fires, yet the attachment writes fx{N}_type. User selects Reverb by keyboard → enabled=0/drywet=0/params=0 (the exact "silent effect" class the seeding exists to fix).
- deterministic_check: headless editor test — setCurrentTopPage(1), locate FX1 typeCombo, focus, combo->keyKeyPress(down); assert fx1_type advanced AND fx1_enabled==0 (seed missed) pre-fix; post-fix assert seed landed.

## F-w10-2: Popup re-pick of the CURRENTLY selected type re-seeds and clobbers user knob values
- Source/ui/FxSlotCard.cpp:186-191 (item.action seeds unconditionally), :518-521 (7 seed writes); juce_ComboBox.cpp:273 (setSelectedId early-outs when id unchanged)
- severity: medium
- evidence: item action runs onUserPick_ BEFORE setSelectedItemIndex; when the picked item IS the current one, JUCE skips the write but the 7 seed writes already executed — user's tweaked knobs snap back to engagement defaults with no type change. stepType guards `if (nxt == cur) return;` — the popup lacks the same-index guard.
- deterministic_check: seed+write fx1_type=Overdrive, set fx1_param1=7, execute the popup item action for the CURRENT index; assert fx1_param1 stays 7 (pre-fix: 50).

## F-w10-3: mtime self-heal misses same-ms / timestamp-preserved external writes
- Source/ui/PresetBrowser.h:141-151 (watchedDirsChanged, ms-resolution juce::Time compare), :245, :156/:169 (recordDir before findChildFiles — safe), :55
- severity: medium
- evidence: juce::Time truncates to ms; external write within the same ms after a scan leaves cache stale with equal mtimes; also mtime-preserved restores (rsync -a / iCloud materialize). editor_test [17]'s 25 ms sleep is not deterministic on 1 s-granularity filesystems (HFS+/FAT).
- deterministic_check: setLastModificationTime(dir, recordedTime) after adding a file → buildMenu → assert debugScanCount stays 1 (documents the residual); positive control: distinct mtime → rescan happens.

## F-w10-4: Directories absent at scan time are never watched — external dir creation invisible
- Source/ui/PresetBrowser.h:141-151 (only recorded paths checked), :120 (map cleared per rescan), :156/:169 (only existing dirs recorded; the 4 roots are never recorded themselves)
- severity: low
- evidence: a USER root that did not exist at scan 1 and is created externally mid-session never surfaces until a save invalidates.
- deterministic_check: construct PresetBrowser with nonexistent USER dir; scan; create USER+preset externally; buildMenu → debugScanCount stays 1, label absent (documents the hole pre-fix).

## F-w10-5: Stale documentation after the W10 rework
- tests/undo_property_test.cpp:12-20 (still narrates listener-side seeding + removed W7 guard), Source/ui/FxSlotCard.cpp:4, Source/ui/FxSlotLabels.h:4 (definition-location comments contradict reality — they live in FxSlotLabels.cpp)
- severity: low; deterministic_check: N/A (docs).

## F-w10-6: editor_test run-log evidence was stale (superseded: supervisor re-ran post-commit, PASS)
- audit log predated the W10 commit; supervisor's 18:38 baseline re-ran editor_test with the new [12]/[17] sections — ALL CHECKS PASSED.

---
Verified good (no action): invalidate() wiring on all 3 save paths (.PRO :4275, .parvati :4315, afterMultiSaved :4398 incl. async MulExportDialog :4380); loads correctly do NOT invalidate; record-before-list scan ordering (over-invalidate only); [this]-capture destruction order in FxTypeCombo (typeAttach_ destroyed before typeCombo_; popup actions SafePointer-guarded); no other fx{N}_type APVTS writers (NRPN address space only maps Ambika patch/part bytes — MidiParameterMap.cpp:64-66; preset-load staging bypasses APVTS — HellcatPreset.cpp:834-841); wheel path inert (scrollWheelEnabled=false default in JUCE 9); a11y value interface read-only; test FX-page hunts correct (tab unparenting handled, no page-switch residue).

Count: 6 findings — 1 high (F-w10-1), 2 medium (F-w10-2, F-w10-3), 3 low.

# FX Scout Brief — Per-Part FX Section for Parvati

A precise architecture brief for adding a Parvati-exclusive per-part FX section
(FX mode + 3 FX slots + separate FX mod matrix). All citations are `file:line`
against the `fx` branch at HEAD. This document is the planning substrate — it
describes the *current* shape and the *recommended* integration points.

> NOTE on magic bytes: the task description referenced magic `"PARV"`, but the
> actual engine-state blob magic is `"PVST"` (`SynthEngine.cpp:252`). YAML/`.parvati`
> files have no magic — they carry `format: parvati-patch|parvati-multi`.

---

## Glossary (key structs/classes and roles)

| Symbol | File:line | Role |
|---|---|---|
| `kNumParts` / `kNumVoices` | `SynthEngine.h:24,27` | 6 Parts, 6 voices (one voice per voicecard). |
| `Part` | `SynthEngine.h:88-189` | One multitimbral part: `patchBytes[112]`, `partBytes[84]`, arp, seq, routing atomics, `PolyAllocator`, `voiceIndices`. **FX state would live here.** |
| `PolyAllocator` | `SynthEngine.h:38-75` | Firmware-faithful per-part voice allocator (mono/poly/cyclic/chain). |
| `AtomicByteArray<N>` | `SynthEngine.h:79-86` | Per-byte atomic uint8 array (MT writes / AT reads). |
| `SynthEngine` | `SynthEngine.h:191-307` | `juce::Synthesiser` owning 6 `AmbikaVoice`, 6 `Part`, 6 mono `voiceCardBuffers_`. Owns transport + allocation + capture/restore. |
| `AmbikaVoice` | `AmbikaVoice.h:38-264` | Bridges one firmware `Voice` into JUCE; renders at 39216 Hz, Lagrange-resamples to host rate. Exposes `getModulationSource(idx)` (line 167). |
| `PatchParamDescriptor` | `ParameterLayout.h:17-30` | One APVTS param: `paramID`, `byteOffset`, `isPart/isSigned/isArp/isOption/isSequencer`, `choices`, min/max/default. |
| `MOD_SRC_*` | `dsp/patch.h:156-193` | 31 modulation-source enum values (Env/LFO/Seq/Op/Perf/Util/Const). **Shared by synth + FX.** |
| `MOD_DST_*` | `dsp/patch.h:195-218` | 20 synth modulation destinations (FX will have its own destination set). |
| `ModMatrixView` | `ui/ModMatrixView.h:31-138` | The 14-slot synth mod-matrix UI (scrollable active-row list + drag-drop). |
| `CentralModBar` | `ui/CentralModBar.h:23-75` | Full-width pill strip (31 mod sources + Note-Seq sentinel); click=select generator, drag=assign. |
| `SynthWorkspace` | `ui/SynthWorkspace.h:21-104` | 3-row SYNTH content: OSC\|MIX\|FILTER top, CentralModBar middle, active-editor\|ModMatrix bottom. |
| `ParamPage` | `PluginEditor.h:172-289` | A generated grid of `ParamControl`s, partitioned into group panels. Editor-owned. |

---

## 1. Audio path / FX insertion point

### Current signal flow (file:line)

1. **Voice render (internal → host rate).** `AmbikaVoice::renderNextBlock` (`AmbikaVoice.h:44`, `AmbikaVoice.cpp`) renders the firmware `Voice::ProcessBlock()` at the **internal 39216 Hz** rate (`dsp/constants.h:53`), then Lagrange-resamples to the **host rate** (`AmbikaVoice.h:243-246`, `kMaxChunk`/`kLookahead`). Output is **mono**, added to the destination buffer. The analog filter + VCA are the final gain stage (no master limiter).

2. **Per-voicecard routing.** `SynthEngine::renderVoices` (`SynthEngine.cpp:995-1021`) overrides `juce::Synthesiser::renderVoices`. It clears the 6 mono `voiceCardBuffers_[]` for the sub-block, then routes each voice to its FIXED voicecard buffer: `vc = av->getVoiceCard()`, `av->renderNextBlock(voiceCardBuffers_[vc], ...)`. The master `outputAudio` is **left untouched** here (JUCE's own additive master sum is effectively bypassed).

3. **Main-bus + aux mixing.** `PluginProcessor::processBlock` (`PluginProcessor.cpp:149-263`), after `engine_.renderNextBlock`, does:
   - **Main bus**: sums ALL six `voiceCardBuffers_` into L+R with `kMainMixHeadroomGain = 0.5f` (-6 dB) (`PluginProcessor.cpp:21,227-235`), then applies a 15 Hz DC blocker per channel (`PluginProcessor.cpp:237-246`).
   - **Aux buses** (VC1..VC6): raw copy of the matching `voiceCardBuffers_[vc]` (`PluginProcessor.cpp:250-258`).

### Part → voicecard mapping

- `Part::voiceAllocation` (`SynthEngine.h:150`): a 6-bit bitmask over firmware voicecards.
- `SynthEngine::rebuildVoiceAllocation` (`SynthEngine.cpp:341-415`): first-wins assignment; `voice i == voicecard i` (identity). Builds each `Part::voiceIndices` (vector of voicecard indices owned by that part). CHAIN (mode 4) can claim extra free cards.
- `voiceCardForIndex(i)` (`SynthEngine.cpp:988-993`): identity — voice i maps to voicecard i.
- **Key fact for FX**: to sum a part's voicecards, iterate `Part::voiceIndices` and add each `voiceCardBuffers_[vc]`.

### FX insertion — recommendation

The FX runs at **HOST sample rate** (the voicecard buffers are host-rate, sized in `prepare` at `SynthEngine.cpp:70-78`). Two viable insertion points:

- **Option A (recommended): per-part FX in `SynthEngine`, post-render.** Add a new method `SynthEngine::renderPartFx()` called from `PluginProcessor::processBlock` **after** `engine_.renderNextBlock` and **before** the main-bus sum. It iterates each part, sums that part's `voiceCardBuffers_[vc]` (via `voiceIndices`) into a per-part **mono sum**, runs the part's FX chain (stereo dry/wet), and writes a per-part **stereo FX output buffer**. The processor's main-bus sum then reads from the FX output buffers (replacing the current raw-voicecard sum). The **aux buses remain raw voicecard taps** (unchanged) — only the main mix goes through FX.

  *Why SynthEngine, not PluginProcessor*: SynthEngine owns `Part` (where FX state/mod-matrix live), owns `voiceIndices` (the part→voicecard mapping), and is the only place that knows the part structure. Putting FX state in `Part` keeps it adjacent to `patchBytes`/`partBytes` and the threading model.

- **Option B: FX in `PluginProcessor::processBlock`.** The processor already does the main-bus sum. It could sum per-part voicecards → FX → main. But it would need to reach into `SynthEngine` for FX state + voiceIndices, blurring ownership.

**Recommendation: Option A.** New per-part stereo FX output buffers owned by `SynthEngine` (sized in `prepare` like `voiceCardBuffers_`); exposed via a getter; mixed by the processor. The FX DSP + mod-matrix evaluation lives in `SynthEngine` alongside the existing MT→AT staging.

**Block granularity**: `renderVoices` is called per **sub-block** (JUCE splits at MIDI events), but the main-bus mix + FX can run once over the **full block** in `processBlock` (after `renderNextBlock` completes and all voicecard buffers hold the full block). This matches the existing DC-blocker pass (also full-block). The FX should NOT run inside `renderVoices` (sub-block, and it's a `juce::Synthesiser` override that must stay voice-only).

---

## 2. APVTS parameter system

### Descriptor table (`ParameterLayout.h:17-30`, `ParameterLayout.cpp:190-434`)

Every APVTS param is a `PatchParamDescriptor`. The table is built once in `getPatchParamDescriptors()` (`ParameterLayout.cpp:197`). The APVTS `ParameterLayout` is generated from it (`createParvatiParameterLayout`, `ParameterLayout.cpp:420-436`): `AudioParameterChoice` if `choices != nullptr`, else `AudioParameterInt`.

### Routing (`PluginProcessor.cpp`)

`parameterChanged` (`PluginProcessor.cpp:265-291`) dispatches by descriptor flags:
- `d.isArp` → `applyArpParameter` (`PluginProcessor.cpp:299-312`)
- `d.isOption` → `applyOptionParameter` (`PluginProcessor.cpp:314-343`)
- `d.isSequencer` → `applySequencerParameter` (`PluginProcessor.cpp:345-358`)
- else → byte bridge: `d.isPart ? applyPartByte : applyPatchByte` (`PluginProcessor.cpp:289-291`)

Non-patch-byte params (`isOption`/`isArp`/`isSequencer`) have **`byteOffset = -1`** and `parvatiValueToPatchByte` returns 0 for them (`ParameterLayout.cpp:455-457`). They are still full APVTS params (get controls + attachments + persist in the APVTS state tree), routed specially.

### Examples of the Parvati-only (non-byte) pattern to mirror

- `vca_curve` (`ParameterLayout.cpp:370-379`): `isOption=true`, `byteOffset=-1`, choice. Routed in `applyOptionParameter` (`PluginProcessor.cpp:318`). **Global** (all voices) — not per-part.
- `filter_card` (`ParameterLayout.cpp:394-411`), `filter_drive` (`ParameterLayout.cpp:415-432`): same pattern.
- `part_select` (`ParameterLayout.cpp:384-392`): `isOption`, `byteOffset=-1`, Int 1..6; routed to `onPartSelect`.
- Arp params (`ParameterLayout.cpp:353-365`): `isArp=true`, routed to the engine's per-part arp setters via `pendingConfig_` + seqlock.

### Part-switching / per-part storage

`onPartSelect` (`PluginProcessor.cpp:376-385`): sets `currentPart_`, `engine_.setCurrentPart`, then `loadPartIntoApvts(newPart)` + `syncAllParamsToEngine`.

`loadPartIntoApvts` (`PluginProcessor.cpp:393-435`): for each descriptor (skipping `isOption` — those are global), reads from engine storage and writes to the APVTS:
- `isArp` → reads `part.readPendingConfig()` fields
- `isSequencer` → reads `part.readPendingConfig()` seq fields
- else → reads `patchBytes`/`partBytes` byte → `parvatiPatchByteToValue`

**Edits always route to the current part** (e.g. `applyPatchByte` writes `parts_[currentPart_]`, `SynthEngine.cpp:162`), so switching parts only needs to *load* the new part's stored values.

### Pattern to add FX params

FX params are Parvati-only (no patch byte) AND **per-part** (unlike `vca_curve` which is global). Recommended approach:

1. **New descriptor flag**: add `isFx = false` to `PatchParamDescriptor` (`ParameterLayout.h:17`). FX descriptors: `paramID` like `fx1_type`/`fx1_drywet`/`fx1_param1`.../`fx2_*`/`fx3_*`, `byteOffset = -1`, `isFx = true`.
2. **Per-part FX storage**: add an FX-storage struct to `Part` (`SynthEngine.h:88`), e.g. `FxState fxState;` holding the FX params + FX mod-matrix slots + slot order/topology, stored as atomics or `AtomicByteArray`-style.
3. **Routing**: add `applyFxParameter` (mirroring `applyOptionParameter`), dispatched in `parameterChanged` when `d.isFx`. It writes the current part's FX storage + sets an `fxDirty_` flag (AT-applied, see §6).
4. **`loadPartIntoApvts`**: add an `isFx` branch reading the current part's `fxState` (mirrors the `isArp`/`isSequencer` branches). This is what makes the FX params follow the part selector.
5. **`syncAllParamsToEngine`** (`PluginProcessor.cpp:368-371`): already iterates all descriptors → `applyParameterToEngine`; FX descriptors route through the new `isFx` branch there too.

The FX mod-matrix slot params (variable count, not capped at 14) cannot all be statically declared APVTS params if the count is unbounded. See §3 for the FX matrix param strategy.

---

## 3. Mod matrix

### Synth matrix (capped at 14)

- `ambika::dsp::kNumModulations = 14` (`dsp/patch.h:221`).
- Params: `mod{1..14}_source` / `_dest` / `_amount` (`ParameterLayout.cpp:282-289`), laid into patch bytes 50..91 (stride 3).
- `ModMatrixView::slotParam(slot, suffix)` = `"mod" + (slot+1) + suffix` (`ModMatrixView.cpp:583-585`).
- Hardcoded 14: `ModMatrixView.cpp:16` (`static_assert(kNumModulations == 14)`), `rows_[14]` (`ModMatrixView.h:132`), `muted_[14]`/`stashedAmount_[14]` (`ModMatrixView.h:119`).
- Active = `amount != 0` OR muted (`ModMatrixView.h:54`). Free slot = `firstFreeSlot` (`ModMatrixView.cpp`).
- `assignNextFreeSlot(src, dst, amt)` (`ModMatrixView.cpp:780-799`): finds first `amount==0 && !muted` slot, writes source/dest/amount through APVTS (`setValueNotifyingHost`), refreshes.
- Drag payload: `"parvatiModSrc:<enum>"` (`ModMatrixView.cpp:200`, emitted by `ModSourceDragGrip`; also by `CentralModBar` pills).
- Mod-source catalog: `ModSourceCatalog.h` — 31 sources + a `kNoteSeqSentinel` (-1, bar-only). 7 clusters (Env/Lfo/SeqArp/Mod/Perf/Util/Const).
- `ModDestMap` (`ui/ModDestMap.h`): maps `paramID → MOD_DST_*` and aggregates per-dest amounts for rings/highlight. **Synth-specific** (knob-centric); FX would need an analogous FX-dest map or reuse the drag machinery against FX param knobs.

### How mod sources are evaluated (the FX challenge)

Sources are computed **per-voice** in `Voice::LoadSources()` (`dsp/voice.cpp:215-271`), stored in `modulation_sources_[MOD_SRC_*]` (uint8, 0..255). The matrix is applied per-voice in `Voice::ProcessModulationMatrix()` (`voice.cpp:295-338`). Sources update **once per 40-sample internal block** (~1 ms) — they are control-rate, not sample-rate.

The FX stage runs at the **PART** level (sum of voicecards), but sources come **from the synth voices**. `AmbikaVoice::getModulationSource(idx)` (`AmbikaVoice.h:167-170`) exposes a single voice's mod-source value.

**To obtain source values for the FX stage**, the engine must sample the synth voices' mod sources. Cleanest approach: in `SynthEngine::renderPartFx` (Option A, §1), read the **most-recently-active** voice's `getModulationSource(idx)` for the part (each voice knows its `partIndex_`). Since sources are control-rate (~1 ms granularity) and the FX runs at host block rate, a per-block sample is sufficient (linear-interp optional). A small per-part atomic mirror of mod sources could be written by each voice in its block commit if sample-accurate FX modulation is desired (open question, see end).

### FX mod matrix — what it needs to mirror

- **Variable slots (no 14 cap)**: the synth matrix is fixed at 14 because it lives in 14 patch-bytes. The FX matrix has no such constraint. Two strategies:
  - (a) **Dynamic APVTS params**: add a fixed *max* (e.g. 16) FX-slot triplets to the descriptor table with distinct IDs (`fxmod1..16_*`), and the UI hides inactive ones (like the synth matrix already does — `ModMatrixView` hides `amount==0` rows). This keeps everything in APVTS (persist/automate/undo for free) but caps at a fixed max.
  - (b) **Engine-side FX matrix** stored in `Part::FxState` (not APVTS), edited via a separate non-APVTS UI binding. More flexible (truly unbounded) but loses APVTS automation/undo for FX-matrix amounts.
  - **Recommendation: (a)** with a generous max (e.g. 16). It reuses the entire APVTS + `ModMatrixView` machinery verbatim and keeps FX-matrix modulation host-automatable.
- **FX destinations**: a new `FX_DST_*` enum (dry/wet of each slot, per-effect params) separate from `MOD_DST_*`. The `FxMatrixView`'s dest combo shows FX destinations; drag-drop targets are the FX slot/param knobs.
- **Separate from synth matrix**: completely separate param IDs (`fxmodN_*`) + a separate `FxMatrixView` instance. The synth `ModMatrixView` and `FxMatrixView` never share state.
- **Sources identical**: reuse `MOD_SRC_*` choice array (`makeModSources`, `ParameterLayout.cpp:68`) + `ModSourceCatalog` + `CentralModBar` drag payload. No new source work needed.

---

## 4. Serialization

### Binary blob (`SynthEngine::captureState`/`restoreState`, `SynthEngine.cpp:254-303`)

Structure (magic `"PVST"`, version 1):
```
"PVST" (4) | version=1 (1) | currentPart (1)
for each of 6 parts:
  patch[112] | part[84] (arp/seq overlaid from pendingConfig_) | midiChannel (1) | keyrangeLow (1) | keyrangeHigh (1) | voiceAllocation (1)
```
Read by `restoreState` (`SynthEngine.cpp:284-303`): validates magic + version==1, loads each part's bytes, `stageArpSeqFromPartBytes`, restores routing, `setCurrentPart` + `resetAllVoices` + `markAllocationDirty`.

This blob rides inside the host state as base64 property `engine_state` on the APVTS ValueTree (`PluginProcessor.cpp:791-815`). `getStateInformation`/`setStateInformation` (`PluginProcessor.cpp:791-891`).

**FX integration (binary)**: bump version to **2**. In `captureState`, after the routing bytes for each part, append the FX state (FX params + FX matrix slots + topology/order). In `restoreState`, after the version check (`SynthEngine.cpp:291`), branch on version: v1 (legacy) → FX defaults; v2 → read FX bytes. The version==1 strict check at `SynthEngine.cpp:291` must become `<= 2` (or branch). Backward compat: old v1 blobs load with FX init defaults.

### YAML (`.parvati` multi, `ParvatiPreset.cpp:497-548`)

`serializeParvatiMulti` emits:
```
format: parvati-multi
version: 1
parts:
  - channel: / keyzone_low: / keyzone_high: / voice_allocation:
    params: { ...all non-option descriptors... }
options: { vca_curve, filter_card, filter_drive }
```
`applyParvatiMulti` (`ParvatiPreset.cpp:552-640`) writes routing + per-part params (byte/arp/seq) + global options.

Helpers: `partParamsMap` (`ParvatiPreset.cpp:400-446`) builds the per-part `params:` map (skips `isOption`); `isSerializable` (`ParvatiPreset.cpp:341-345`) only excludes `part_select`.

**FX integration (YAML)**: add a per-part `fx:` block inside each `parts:` entry (after `params:` or alongside routing). Emit FX params + FX matrix slots. On apply, read `fx:` (absent in old files → FX defaults). The YAML parser is a hand-rolled indentation-driven subset (`ParvatiPreset.cpp:21-101`); nested maps under `fx:` are supported by the existing recursive parser. Unknown keys are ignored (`ParvatiPreset.cpp:474`, forward-compat).

### Ambika `.PRO`/`.MUL` — FX naturally excluded ✓

`saveProgramFile` (`PluginProcessor.cpp:480-530`) and `saveMultiFile` (`PluginProcessor.cpp:632-690`) iterate descriptors and **skip `isArp || isOption`** (`PluginProcessor.cpp:493-494, 660`). FX descriptors (`isFx`) would be added to this skip condition (`d.isArp || d.isOption || d.isFx`) so they never touch the Ambika byte format. Similarly `loadProgramFromBytes`/`loadMultiFile` only write patch/part struct bytes — FX is untouched. **No FX leaks into `.PRO`/`.MUL`.** Confirmed by the existing precedent: `vca_curve`/`filter_card`/`filter_drive` are already correctly dropped this way (`ParameterLayout.cpp:417` comment).

---

## 5. UI structure

### Header layout (`PluginEditor.cpp:2982-3083`)

The header (`kHeaderH = 40`, `PluginEditor.h:481`) holds a centred cluster (`PluginEditor.cpp:3045-3065`):
```
[patchCaption_] [presetBrowser_] [gap] [globalButton_] [partCaption_] [partCombo_] [gap] [multiButton_]
```
Widths: patchCapW=48, presetW=220, partCapW=40, partComboW=88, gapW=6, multiW=64, globalW=64.

**Requirement**: header becomes `Part [Part 1] [Synth] [FX] [Multi]`. This means inserting a **[Synth]/[FX] mode toggle** between `partCombo_` and `multiButton_` (replacing or sitting beside `globalButton_`). The toggle is a header button (like `multiButton_`/`globalButton_` — `setClickingTogglesState`, `PluginEditor.cpp:2376,2398`), not an APVTS param (it's a view-mode selector, like the Multi/Global overlays).

### Page selector + overlay mechanism (`PluginEditor.cpp:2105,2349-2413`)

- `pageSelector_` is a `juce::TabbedComponent` with **one hidden tab** (SYNTH; `setTabBarDepth(0)`, `PluginEditor.cpp:2352`). Its content is `synthWorkspace_` (`PluginEditor.cpp:2354`).
- Overlays: `multiPage_` (Multi/Setup) and `globalPage_` (Global ParamPage) are direct children, `setVisible`-toggled, mutually exclusive (`PluginEditor.cpp:2371-2413`), sized to the content area (`PluginEditor.cpp:3078-3084`).

### SynthWorkspace 3-row layout (`SynthWorkspace.cpp:139-211`)

```
TOP:    OSC (40%) | MIXER (20%) | FILTER (40%)   [ParamPages, reparented non-owned]
MIDDLE: CentralModBar (kBarHeight=38)             [workspace-owned]
BOTTOM: active-editor-host (LEFT 50%) | ModMatrixView (RIGHT 50%)
```
- Main-row pages: `setOscillators`/`setMainLeft`/`setMainRight` (`SynthWorkspace.cpp:51-83`).
- Active editor: `registerGeneratorPage(modSrc, page, groups)` + `setActiveGenerator` reparents ONE generator ParamPage at a time (`SynthWorkspace.cpp:85-93,110-135`). The CentralModBar pill-click swaps it (`SynthWorkspace.cpp:43-52`).
- ModMatrixView: `setModMatrixView` hosts the editor-owned view as a direct (non-owned) child (`SynthWorkspace.cpp:95-105`).

### What to clone/specialize for FX

- **`FxWorkspace`** (clone of `SynthWorkspace`): same 3-row skeleton but:
  - TOP row: **3 FX-slot panels** (FX1/FX2/FX3), each showing slot type + dry/wet + effect params. These are ParamPages (editor-owned, reparented) generated from the `fx*_` descriptors.
  - MIDDLE row: **reuse the SAME `CentralModBar`** — the modulator editors (Env/LFO/Seq/Arp) work identically and modify the same synth-side values. Click a generator pill → swap the bottom-left active editor (same generator ParamPages as SYNTH, since modulators come from synth).
  - BOTTOM row: LEFT 50% = active-generator editor (shared with SYNTH), RIGHT 50% = **`FxMatrixView`** (clone of `ModMatrixView`).
  - The FX-slot order + series/parallel topology needs a UI control (drag-to-reorder on the slot panels + a topology toggle). This is new UI not present in the synth.
- **`FxMatrixView`** (clone of `ModMatrixView`): variable-slot list, FX destinations (not `MOD_DST_*`), FX-matrix param IDs (`fxmodN_*`). Drag-drop reuse: same `"parvatiModSrc:<enum>"` payload; drop targets are the FX slot/param knobs.
- **Mode toggle [Synth]/[FX]**: a header button (or a 2-tab `pageSelector_`). The cleanest: keep `pageSelector_` as the workspace host but swap its content (`synthWorkspace_` ↔ `fxWorkspace_`) on mode toggle, OR add a real 2nd tab. Given `setTabBarDepth(0)` currently hides the lone bar, promoting it to a visible 2-tab bar (`[SYNTH][FX]`) is the most idiomatic. The modulator editor is **shared** (reparented between the two workspaces), not duplicated — both workspaces' active-editor-host reparent the SAME editor-owned generator ParamPages.

### Static layout constants (`PluginEditor.h:478-485`)

```
kBarHeight=34, kHeaderH=40, kPageTabsH=28, kKeyboardH=104, kVoiceStripH=22
CentralModBar::kBarHeight = 38 (CentralModBar.h:28)
```
Window: default 1280×620, min from `barPreferredWidth()` (`PluginEditor.cpp:2573-2578`), `setResizeLimits(minWidth, 600, 1800, 1100)`.

---

## 6. Threading / concurrency model

Parvati uses a strict **message-thread (MT) writes / audio-thread (AT) reads** discipline with several primitives (`SynthEngine.h:79-189`):

| Mechanism | Where | Use |
|---|---|---|
| `AtomicByteArray<N>` | `SynthEngine.h:79-86` | Per-byte atomic for `patchBytes[112]`/`partBytes[84]`. MT writes (`applyPatchByte`), AT reads (`pushPartBytesToVoices`). |
| `std::atomic<T>` | routing fields (`midiChannel`, `keyrangeLow/High`, `voiceAllocation`, `voiceCount_`) | MT writes, AT reads. |
| **seqlock** (`pendingSeq_` + `writePendingConfig`/`readPendingConfig`) | `SynthEngine.h:130-161` | `PendingConfig` (arp/seq): MT sole writer, AT sole reader. Even=stable, odd=mid-write; AT retries. |
| **dirty flags** (MT set, AT `exchange(false, acq_rel)`) | `frameDirty_`, `configDirty_`, `optionsDirty_`, `allocationDirty_`, `resetAllVoicesPending_` | MT stages a change + sets flag; AT services it at the top of `processTransport` (`SynthEngine.cpp:760-840`) and applies single-threaded. |
| Staged atomics | `pendingVcaExp_`, `pendingSmoothing_`, `pendingFilterDrive_` | Option values staged by MT setters (`setVcaExponential` etc.), read by AT when `optionsDirty_` is serviced (`SynthEngine.cpp:780-791`). |

The AT service loop is in `SynthEngine::processTransport` (`SynthEngine.cpp:760-840`), called every block before `renderNextBlock` (`PluginProcessor.cpp:195`).

### FX param writes — must follow the SAME pattern

FX params are written on the **MT** (APVTS listener `parameterChanged` → `applyFxParameter`), read on the **AT** (the FX render in `renderPartFx`). Recommended:

1. **Per-part FX storage** in `Part` (`SynthEngine.h:88`): use `AtomicByteArray`-style atomics (or `std::atomic` per field) for FX params, mirroring `patchBytes`. The FX mod-matrix slot params similarly stored atomically.
2. **Dirty flag**: add `std::atomic<bool> fxFirty_ { false };` to `Part` (mirrors `frameDirty_`). `applyFxParameter` writes the current part's FX storage + sets `fxDirty_`.
3. **AT application**: in the FX render path (or in `processTransport`'s service loop), `fxDirty_.exchange(false, acq_rel)` → copy staged FX params into the active FX DSP objects single-threaded, then render. This is exactly the `optionsDirty_`/`frameDirty_` pattern.

Because the FX mod-matrix sources are read from synth voices (§3), and voice mod-source writes are AT-only, the FX matrix evaluation is naturally AT-safe (no cross-thread access to `modulation_sources_`).

---

## 7. Build / test conventions

- **Source globbing**: `Source/*.cpp/*.cc/*.h` is `GLOB_RECURSE`'d with `CONFIGURE_DEPENDS` (`CMakeLists.txt:~145-150`). **New FX files under `Source/` are auto-picked up** (no manual CMake edit needed for the plugin target).
- **Debug build dir**: `build/` (default `CMAKE_BUILD_TYPE=Debug`, artefacts in `build/Parvati_artefacts/Debug/`). Develop against Debug only (`CONTRIBUTING.md`).
- **Test pattern**: each test is its own executable — `add_executable(parvati_<name>_test tests/<name>.test.cpp)` + `target_link_libraries(... PRIVATE Parvati)` + `target_include_directories(... PRIVATE Source)`. Built by default. Run: `./build/parvati_<name>_test`.
  - **Closest analog for FX serialization**: `tests/parvati_preset_test.cpp` (YAML round-trip + forward-compat; pattern: `setParam`, serialize→apply into a 2nd processor, compare all descriptor raw values). Mirror for an `fx_preset_test`.
  - **Closest analog for FX host-state**: `tests/host_state_test.cpp` (binary `getStateInformation`/`setStateInformation` round-trip across 6 parts; `renderOnce` helper). Mirror for FX state round-trip.
  - **Concurrency**: `tests/concurrency_test.cpp` + `tests/mt_harness.h` (`parvati_test::runConcurrent`) — spin a real AT while MT mutates. FX param writes must be added to the MT op set here. Run under TSAN (`PARVATI_ENABLE_TSAN=ON`).
- **CONTRIBUTING notes** (`CONTRIBUTING.md`): develop against Debug; add a test per feature; run ASAN/UBSAN + clang-tidy before non-trivial changes; keep diffs minimal; update `CHANGELOG.md`.

---

## Open design questions / risks (decisions for a human)

1. **FX block-rate vs sample-rate modulation.** Synth mod sources update at **control rate** (~1 ms, once per 40-sample internal block). FX params are typically smoothed sample-rate. Should FX param modulation (from the FX matrix) be sample-rate (smooth) or block-rate (cheap, step)? Sample-rate is more natural for FX (e.g. a tremolo on dry/wet) but requires holding a per-part snapshot of mod sources at host-block granularity.

2. **Mono vs stereo FX.** Voicecard buffers are **mono**; the main bus is stereo. A part's sum is mono. FX (reverb/chorus) are inherently stereo. Decision: run FX **stereo** (duplicate the mono sum to L+R input, produce stereo wet), or keep mono until the final main mix? Stereo is strongly recommended for spatial FX, but it changes the per-part buffer shape (mono sum → stereo FX I/O).

3. **How mod source values are sampled for the FX stage.** Per-voice (`getModulationSource`) but FX is per-part (sum of voices). Options: (a) sample the most-recently-triggered voice's sources; (b) maintain a per-part atomic mod-source mirror that every active voice writes each block (adds a write per voice per block); (c) average/max across the part's voices. (a) is cheapest and likely sufficient; (b) is most accurate; (c) is unusual. This affects the FX matrix smoothness and CPU.

4. **FX matrix slot count / strategy.** Fixed-max APVTS params (e.g. 16 slots, recommendation §3) vs. engine-side dynamic storage. Fixed-max keeps host automation/undo/serialize free but caps the count. Confirm the desired max and whether FX-matrix amounts need host automation.

5. **Aux bus behavior with FX.** Aux buses (VC1..6) are raw per-voicecard taps. FX is per-part on the main mix only. Confirm aux buses stay **dry** (no FX) — this is the clean choice (aux = individual voicecard output), but a user might expect post-FX per-part aux. Recommend keeping aux dry.

6. **Binary blob version-bump backward/forward compat.** v1→v2: old hosts reload v1 with FX defaults (safe). But a v2 blob loaded by an *older* Parvati (that expects v1) will hit the strict `version==1` check (`SynthEngine.cpp:291`) and reject the whole blob (losing all parts). Mitigation: keep the version check `== expected` but append FX as a *trailing* block read only when present (length-prefixed), so a v2 blob's v1 prefix still parses on older builds. Decide whether strict-version or length-prefix is the compat strategy.

7. **Shared modulator editor between SYNTH and FX workspaces.** Both workspaces reparent the SAME editor-owned generator ParamPages. Confirm the active-generator selection is workspace-scoped (SYNTH and FX can show different editors independently) or shared (one selection). If shared, the reparent must move on mode toggle. If independent, the generator pages must be **duplicated** (which doubles APVTS attachments — undesirable). Recommend: **shared pages, single active selection** (the mode toggle just changes which workspace hosts them), accepting that SYNTH↔FX swaps the visible generator.

8. **FX DSP implementation.** JUCE `juce::dsp` (Reverb, Chorus, Delay, etc.) runs at host rate natively — ideal for the FX stage. Decide which effects populate the 3 slots and whether the effect type is a per-slot choice (FX1 type = Reverb/Chorus/Delay/...). Each effect's parameters become the `fxN_param*` descriptors.

9. **FX order + series/parallel topology persistence.** Slot order (reorderable) + topology (series/parallel) is new state. It needs to live in `Part::FxState` and round-trip through both binary + YAML. Decide the data shape (e.g. an order array + a topology enum per adjacent pair, or a small routing graph).

10. **`setParameterSmoothing` / oversampling interplay.** The existing smoothing/oversampling options are voice-scoped. FX has its own smoothing needs (knob zipper noise). Decide whether FX params reuse the global `ui_smoothing` toggle or get their own.

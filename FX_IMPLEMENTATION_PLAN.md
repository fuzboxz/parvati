I have everything verified. Now writing the complete implementation plan as my deliverable.

# Implementation Plan

## Goal
Add a Parvati-exclusive per-part FX section (3 reorderable stereo FX slots with series/parallel topology, a separate 16-slot FX mod matrix sharing the synth's mod sources, per-part state, and a placeholder FX UI) — fully integrated into the engine/DSP, parameter bridge, serialization (`.parvati` carries it; `.PRO`/`.MUL` drops it), and a header `[Synth]/[FX]` mode toggle.

---

## CONTRACT (implement verbatim — every layer agrees on these)

```cpp
// ===== Source/SynthEngine.h additions (inside the file, NOT dsp/patch.h) =====
// Parvati-exclusive FX. Ambika knows nothing about these.

constexpr int kNumFxSlots       = 3;    // FX1/FX2/FX3
constexpr int kNumFxMatrixSlots = 16;   // separate FX mod matrix (no 14 cap)
constexpr int kNumFxSlotParams  = 4;    // generic params 1..4 per slot

// Effect type per slot (drives fx{N}_type choice).
enum class FxType : uint8_t {
    None = 0, GainPan = 1, Delay = 2, Reverb = 3, Chorus = 4,
    Count
};
// choice list string: { "None", "Gain+Pan", "Delay", "Reverb", "Chorus" }

enum class FxTopology : uint8_t { Series = 0, Parallel = 1 };
// choice list string: { "Series", "Parallel" }

// FX mod-matrix destinations (drives fxmod{N}_dest choice). Distinct from MOD_DST_*.
enum FxModDestination : int {
    FX_DST_NONE = -1,
    FX_DST_FX1_DRYWET = 0, FX_DST_FX1_P1, FX_DST_FX1_P2, FX_DST_FX1_P3, FX_DST_FX1_P4,
    FX_DST_FX2_DRYWET,     FX_DST_FX2_P1, FX_DST_FX2_P2, FX_DST_FX2_P3, FX_DST_FX2_P4,
    FX_DST_FX3_DRYWET,     FX_DST_FX3_P1, FX_DST_FX3_P2, FX_DST_FX3_P3, FX_DST_FX3_P4,
    FX_DST_LAST
};   // 15 destinations

// Per-part FX storage. MT writes (engine setters called by applyFxParameter),
// AT reads (renderPartFx). Each field is atomic; fxDirty_ (release-store by MT,
// acq_rel-exchange by AT) publishes a frame of writes. EXACTLY the frameDirty_/
// optionsDirty_ pattern (SynthEngine.h:118-126, SynthEngine.cpp:770-791).
struct PartFxState
{
    std::atomic<uint8_t> slotType   [kNumFxSlots] {};               // FxType
    std::atomic<uint8_t> slotEnabled[kNumFxSlots] {};               // 0/1
    std::atomic<uint8_t> slotDryWet [kNumFxSlots] {};               // 0..127
    std::atomic<uint8_t> slotParam  [kNumFxSlots][kNumFxSlotParams] {}; // 0..127
    std::atomic<uint8_t> topology { 0 };                            // FxTopology
    std::atomic<uint8_t> orderIdx { 0 };                            // 0..5 (perm of {0,1,2})
    std::atomic<uint8_t> modSource[kNumFxMatrixSlots] {};          // MOD_SRC_* index
    std::atomic<uint8_t> modDest  [kNumFxMatrixSlots] {};          // FxModDestination
    std::atomic<int8_t>  modAmount[kNumFxMatrixSlots] {};          // -63..+63
    std::atomic<bool>    fxDirty_ { false };
};

// orderIdx 0..5 -> permutation. Add a free function:
//   std::array<int,3> fxOrderPermutation (uint8_t idx);  // table of 6 perms
// perms: 0:{0,1,2} 1:{0,2,1} 2:{1,0,2} 3:{1,2,0} 4:{2,0,1} 5:{2,1,0}

// Add to struct Part (SynthEngine.h:88):   PartFxState fxState;
```

```cpp
// ===== PatchParamDescriptor flag (Source/ParameterLayout.h:17) =====
// add field:   bool isFx = false;
// FX descriptors: byteOffset = -1, isFx = true, isPart = false.

// ===== FX APVTS param IDs (all isFx; ranges) =====
// fx{1,2,3}_type      choice  {None,Gain+Pan,Delay,Reverb,Chorus}  def None
// fx{1,2,3}_enabled   Int 0..1                                   def 0
// fx{1,2,3}_drywet    Int 0..127                                 def 0   (0=fully dry)
// fx{1,2,3}_param{1..4} Int 0..127                               def 0
// fx_topo             choice {Series,Parallel}                   def Series
// fx_order            Int 0..5                                   def 0
// fxmod{1..16}_source choice makeModSources() (reuse)            def 0
// fxmod{1..16}_dest   choice makeFxDests()  (NEW)                def 0
// fxmod{1..16}_amount Int -63..63                                def 0
//   Total: 21 (slots) + 2 (topo/order) + 48 (matrix) = 71 new params.

// ===== Engine signatures (Source/SynthEngine.h, class SynthEngine) =====
void prepare (double sampleRate, int blockSize);   // EXTEND: size fxOutputBuffers_ + chains
void renderPartFx (int numSamples);                // AT: per-part FX into fxOutputBuffers_
const std::array<juce::AudioBuffer<float>, kNumParts>& getFxOutputBuffers() const noexcept;
// MT setters (write current part's fxState + set fxDirty_), mirror setArpMode:
void setFxSlotType (int slot, uint8_t v);
void setFxSlotEnabled (int slot, uint8_t v);
void setFxSlotDryWet  (int slot, uint8_t v);
void setFxSlotParam   (int slot, int idx, uint8_t v);
void setFxTopology    (uint8_t v);
void setFxOrder       (uint8_t v);
void setFxModSlot     (int slot, uint8_t src, uint8_t dest, int8_t amount);
```

---

## Tasks

### Phase 0 — FX DSP core (new files, standalone, compiles independently)
**Files to CREATE:**
- `Source/dsp/fx/FxProcessor.h` — abstract base:
  ```cpp
  class FxProcessor {                 // one effect instance per slot
  public:
    virtual ~FxProcessor() = default;
    virtual void prepare (double sampleRate, int maxBlock) = 0;
    virtual void reset() = 0;
    // in-place stereo block; dry/wet is applied by the chain, NOT here.
    virtual void process (float* L, float* R, int numSamples) = 0;
    virtual FxType type() const = 0;
  };
  std::unique_ptr<FxProcessor> createFxProcessor (FxType t);  // factory
  ```
- `Source/dsp/fx/FxProcessors.h` / `.cpp` — the 4 placeholder effects:
  - `FxGainPan` (param1=gain 0..1→ -12..+12 dB; param2=pan 0..1→ L..R).
  - `FxDelay` — `juce::dsp::DelayLine<float>` (param1=time 0..1→0..1 s, param2=feedback 0..1, param3=stereo spread). prepare must `juce::dsp::ProcessSpec{rate, block, 2}`.
  - `FxReverb` — `juce::dsp::Reverb` (`juce::dsp::Reverb::Parameters` from param1..4: roomSize/damping/wetLevel/width).
  - `FxChorus` — `juce::dsp::Chorus<float>` (param1=rate, param2=depth); `prepare` + `process` via `juce::dsp::AudioBlock`/`ProcessContextReplacing`.
  - Each holds `double rate_` and a `setParams(float p[4])` that maps the 4 generic params. `process` operates on stereo pairs in-place. All zero-alloc on the audio thread (prepare reserves).
- `Source/dsp/fx/FxChain.h` / `.cpp` — owns 3 `std::unique_ptr<FxProcessor>` (rebuilt when a slot's `FxType` changes), plus per-slot enabled/dryWet(0..1)/4-params (held as `float`, AT-local), topology + order permutation.
  ```cpp
  class FxChain {
  public:
    void prepare (double rate, int maxBlock);
    void setSlotType (int slot, FxType t);        // rebuilds that slot's processor
    void setSlotEnabled (int slot, bool e) noexcept;
    void setSlotDryWet (int slot, float dw) noexcept;     // 0..1
    void setSlotParam  (int slot, int idx, float v) noexcept; // 0..1
    void setTopology (FxTopology t) noexcept;
    void setOrder (const std::array<int,3>& ord) noexcept;
    bool anyEnabled() const noexcept;             // fast bypass for the dry path
    void process (const float* inL, const float* inR,            // mono sum (duplicated)
                  float* outL, float* outR, int numSamples);     // stereo out
  };
  ```
  - **Series**: order permutation applied; each slot: `wet = process(dry); out = dry*(1-dw) + wet*dw`. Disabled slot = passthrough.
  - **Parallel**: each slot processes a copy of the input; mix outputs (simple equal-gain sum scaled by 1/activeCount, then dry/wet blend).
  - **Bypass**: if `!anyEnabled()`, `process` copies in→out (dry), so the part's main contribution is the dry summed signal (audibly-identical to the pre-FX path).

**Acceptance:** `cmake --build build -j 8` compiles (new files are auto-globbed, `CMakeLists.txt:135-139`). No behavior change yet (nothing calls them).

---

### Phase 1 — Engine integration (render path + per-part FX state)
**Files to EDIT:**
- `Source/SynthEngine.h`
  - Add the CONTRACT enums/constants + `PartFxState` (above) near the top (after `kNumVoices`, ~line 27; before `struct Part`).
  - Add `PartFxState fxState;` member to `struct Part` (`SynthEngine.h:88`, alongside `partBytes`).
  - Add to `class SynthEngine`: private `std::array<FxChain, kNumParts> fxChains_;`, `std::array<juce::AudioBuffer<float>, kNumParts> fxOutputBuffers_;` (2 channels each), `std::array<std::array<uint8_t, ambika::dsp::MOD_SRC_LAST>, kNumParts> lastModSources_;` (per-part source snapshot for tails). Public: `renderPartFx(int)`, `getFxOutputBuffers()`, and the 7 MT setters (above).
- `Source/SynthEngine.cpp`
  - **`prepare`** (`SynthEngine.cpp:70-78`): after sizing `voiceCardBuffers_`, size `fxOutputBuffers_[p].setSize(2, maxBlock)` and call `fxChains_[p].prepare(sampleRate, blockSize)`; zero `lastModSources_`.
  - **MT setters** (mirror `setArpMode` `SynthEngine.cpp` / `setArpDirection` inline `SynthEngine.h:266`): each writes `parts_[currentPart_].fxState.<field>.store(v, relaxed)` then `fxState.fxDirty_.store(true, release)`. `setFxSlotType` also needs the AT to rebuild the processor — the AT servicing (below) reads `slotType` and calls `chain.setSlotType`.
  - **`renderPartFx(int numSamples)`** (new; called from processor after `renderNextBlock`): for each part `p`:
    1. Service `fxDirty_`: `if (fxState.fxDirty_.exchange(false, acq_rel))` → read all fxState atomics into local `float`/enums and push to `fxChains_[p]` (`setSlotType` only if changed vs a cached last-type, `setSlotEnabled/Param/DryWet`, `setTopology`, `setOrder(fxOrderPermutation(orderIdx))`, and rebuild mod-matrix target arrays). Single-threaded on AT.
    2. Build the per-part **mono sum** of `voiceCardBuffers_[vc]` (iterate `parts_[p].voiceIndices`) into a scratch mono buffer (reuse a member `AudioBuffer<float,1>` sized in prepare). If `voiceIndices` empty → write silence.
    3. Sample mod sources: find first **active** voice in `voiceIndices` (`getAmbikaVoice(vi)->isVoiceActive()`); if found, `for src in 0..MOD_SRC_LAST: s[src] = av->getModulationSource(src)` and store into `lastModSources_[p]`; else reuse `lastModSources_[p]` (tails modulate). `getModulationSource` is `AmbikaVoice.h:130`.
    4. Evaluate the FX mod matrix (block-rate): for each of 16 slots with `modAmount != 0`, `modOffset[dest] += amount/63 * (srcValue/255)`. Apply offsets to the corresponding slot dryWet/param target values (clamp 0..1) — i.e. recompute effective params AFTER the dirty-flag push, combining base + mod. Call `fxChains_[p].setSlot*` with effective values (only if changed).
    5. Duplicate the mono sum to L+R input; `fxChains_[p].process(inL,inR,outL,outR,numSamples)` into `fxOutputBuffers_[p]` (channel 0=L, 1=R). When `!anyEnabled()`, chain copies dry.
- `Source/PluginProcessor.cpp`
  - **`processBlock`** (`PluginProcessor.cpp:149-263`): insert `engine_.renderPartFx (numSamples);` immediately after `engine_.renderNextBlock(...)` (line ~198) and before the main-bus sum. Then change the main-bus sum (lines 227-235) to read from `engine_.getFxOutputBuffers()` instead of `voiceCardBuffers_` — for each part `p`, add `fxOut[p]` ch0→main L, ch1→main R (when stereo), with `kMainMixHeadroomGain`. **Aux buses (250-258) stay on `voiceCardBuffers_` (dry) — unchanged.** DC blocker unchanged.

**Acceptance:** builds; existing tests green. Audibly: with all `fx*_enabled=0`, output equals the prior dry mix (chain bypass = dry copy). New FX params not yet wired to UI, so defaults hold.

**Risk:** Float reassociation (sum-of-per-part-sums vs sum-of-voicecards) is not bit-identical but audibly identical — acceptable. Verify the `parvati_realtime_test` / `idle_silence_test` still pass (they check silence/idle behaviour, unaffected since enabled=0 by default).

---

### Phase 2 — Parameters + APVTS bridge (FX params flow MT→engine)
**Files to EDIT:**
- `Source/ParameterLayout.h:17` — add `bool isFx = false;` to `PatchParamDescriptor`.
- `Source/ParameterLayout.cpp` (descriptor table, `getPatchParamDescriptors()` ~line 197):
  - Add `static const auto kFxTypes = juce::StringArray {"None","Gain+Pan","Delay","Reverb","Chorus"};`, `static const auto kFxTopologies = juce::StringArray {"Series","Parallel"};`, `static const auto kFxDests = makeFxDests();` (new helper: builds from `FX_DST_*` labels), and reuse `kModSources` (already `makeModSources()`, line 68).
  - Add an `addFx` lambda (mirror `addArp`, line 345) setting `byteOffset=-1, isFx=true`, with explicit `defaultValue`. Append, before `return d;`:
    - for slot s in 1..3: `fx{s}_type` (choice kFxTypes, def 0), `fx{s}_enabled` (Int 0..1), `fx{s}_drywet` (Int 0..127), `fx{s}_param1..4` (Int 0..127).
    - `fx_topo` (choice kFxTopologies, def 0), `fx_order` (Int 0..5, def 0).
    - for m in 1..16: `fxmod{m}_source` (choice kModSources), `fxmod{m}_dest` (choice kFxDests), `fxmod{m}_amount` (Int -63..63).
  - `parvatiValueToPatchByte` (`ParameterLayout.cpp:460`) already returns 0 for `byteOffset<0`; FX descriptors have `byteOffset=-1` so they're safe (no patch-byte write). Verify the early-return covers `isFx`.
- `Source/PluginProcessor.cpp`:
  - **`parameterChanged`** (`:265`): add `if (d.isFx) { applyFxParameter(d, newValue); return; }` after the `isSequencer` block (`:286`).
  - **`applyParameterToEngine`** (`:301`): add the same `isFx` branch (reads APVTS raw → `applyFxParameter`). This makes `syncAllParamsToEngine` (`:368`) seed FX on init.
  - **`applyFxParameter`** (new, mirrors `applyOptionParameter` `:314`): decode `d.paramID` by prefix/structure → call the matching `engine_.setFx*` setter. Map: `fx{N}_type`→`setFxSlotType(N-1,v)`, `fx{N}_enabled`→`setFxSlotEnabled`, `fx{N}_drywet`→`setFxSlotDryWet`, `fx{N}_param{K}`→`setFxSlotParam(N-1,K-1,v)`, `fx_topo`→`setFxTopology`, `fx_order`→`setFxOrder`, `fxmod{M}_source/_dest/_amount`→`setFxModSlot(M-1, src, dest, amt)` (read the three sibling params for the matrix slot, or call `setFxModSlot` with the just-changed field + current siblings).
  - **`loadPartIntoApvts`** (`:413`): the existing loop skips `isOption` (`:418`). Add an `isFx` branch (before the byte-read else): read `p.fxState.*` atomics → `apvts.getParameterAsValue(id) = value` (reverse the encode: type/enabled/topo/order are direct; drywet/param are 0..127 ints; fxmod source/dest/amount direct). Do NOT add `isFx` to the `isOption` skip — FX is per-part.
- `Source/PluginProcessor.h` — declare `void applyFxParameter (const PatchParamDescriptor&, float);`.

**Acceptance:** builds; `paramIndex_` (`:39-42`) auto-indexes the new params. All tests green. FX params now persist in the APVTS state tree and follow the part selector (switching parts loads/stores fxState). FX is still silent unless `enabled` set programmatically.

**Risk:** `setFxModSlot` must read sibling source/dest/amount for a slot when only one of the three changes — store all three atomically per call by reading the current fxState for the other two (or have `applyFxParameter` re-read all three APVTS siblings for that fxmod slot and write them together). Decide: re-read siblings in `applyFxParameter` (cleanest, avoids torn matrix-slot state).

**Dependency:** Phase 1 must compile first (the engine setters must exist).

---

### Phase 3 — Serialization (`.parvati` carries FX; `.PRO`/`.MUL` drops it; binary v2)
**Key simplification (verified):** because FX state is APVTS params with `isFx`, the **`.parvati` YAML round-trip is nearly free** — `partParamsMap` (`ParvatiPreset.cpp:400`) already serializes every `isSerializable && !isOption` descriptor from per-part engine storage. FX descriptors are `isFx` (not `isOption`), so they are included automatically once `partRaw` knows how to read them. No separate `fx:` block needed (deliberate simplification of the design's `fx:` block — same data, flat in `params:`).

**Files to EDIT:**
- `Source/ParvatiPreset.cpp`:
  - **`partRaw`** (`:359`): add an `isFx` branch reading `part.fxState.*` (slot type/enabled/drywet/param/topo/order/fxmod source/dest/amount) by `paramID`. Mirror the `isArp`/`isSequencer` dispatch style.
  - `currentParamsMap` (`:389`) uses `currentRaw` (reads APVTS directly) — FX params are in APVTS, so single-part `.parvati` patches carry FX automatically. ✓
  - **`applyParvatiMulti`** (`:552`): the per-part `params:` loop dispatches by `isArp`/`isSequencer`/else-byte. Add an `isFx` branch that writes `part.fxState.*.store(...)` + sets `part.fxState.fxDirty_` (once per part, after the loop — mirror `stagedArpSeq`/`configDirty_` pattern at `:642`).
  - **`applyParvatiPatch`** (`:462`): writes via APVTS (`getParameterAsValue`); FX params are normal APVTS params → applied automatically. ✓ (Verify the loop doesn't skip `isFx`.)
- `Source/SynthEngine.cpp` — **binary host-state blob** (`captureState` `:254`, `restoreState` `:284`):
  - Bump version `1 → 2` in `captureState` (`out.writeByte(1)` `:261` → `2`). After each part's routing bytes (`:279`), append a **length-prefixed FX block**: write `uint32 fxLen` then `fxLen` bytes capturing all fxState fields (slot types/enabled/drywet/params[3×4], topo, order, 16×{src,dst,amount}). Use a fixed layout (no length variance) but still emit the length prefix for forward-safety.
  - `restoreState` (`:284`): change the strict `version==1` check (`:291`) to accept `1` and `2`. For v1: core only, leave fxState at defaults. For v2: after reading routing bytes per part, read the `uint32 fxLen` + FX block into `part.fxState.*` + set `fxState.fxDirty_`. Keep the strict-reject for unknown versions (caller falls back to legacy APVTS restore, per the existing comment `SynthEngine.h:289` / `restoreState` contract).
- `Source/PluginProcessor.cpp` — **`.PRO`/`.MUL` drop FX**: add `|| d.isFx` to the three skip conditions: `:467`, `:510`, `:655` (each currently `d.isArp || d.isOption`). FX never touches Ambika bytes. ✓ (Verified precedent: `filter_drive` is already dropped this way.)

**Acceptance:** builds; new + existing serialization tests green. `.PRO`/`.MUL` round-trip unchanged (FX dropped). `.parvati` multi/patch round-trip FX state across all 6 parts.

**Dependency:** Phase 2 (fxState + descriptors) must exist.

---

### Phase 4 — UI (header `[Synth]/[FX]` toggle, FxWorkspace, FxMatrixView)
**Files to CREATE:**
- `Source/ui/FxMatrixView.h` / `.cpp` — **clone of `ModMatrixView`** (`Source/ui/ModMatrixView.h/.cpp`) with mechanical changes:
  - Class name `FxMatrixView`; slot count 16 (not 14); `muted_[16]`, `stashedAmount_[16]`.
  - `slotParam` prefix `"fxmod"` (returns `"fxmod"+(slot+1)+suffix`).
  - Remove the `static_assert(kNumModulations==14)` (`ModMatrixView.cpp:16`); use `kNumFxMatrixSlots`.
  - Dest combo uses FX destinations (the `fxmod{N}_dest` choice list = `makeFxDests()`), not `MOD_DST_*`.
  - Amount range stays -63..63 (reuses the bipolar L&F + drag-drop verbatim). Drag payload `"parvatiModSrc:<enum>"` unchanged → drop feedback works unchanged.
- `Source/ui/FxWorkspace.h` / `.cpp` — **clone of `SynthWorkspace`** (`Source/ui/SynthWorkspace.h/.cpp`) with:
  - TOP row: 3 FX-slot panels (FX1/FX2/FX3) instead of OSC/MIX/FILTER. New setters `setFxSlotPage(int slot, ParamPage*)` (3 panels at ~33% each).
  - MIDDLE: the **SAME** `CentralModBar` (reuse the existing component instance — modulators come from synth). `setModBar`/shared. (Simplest: FxWorkspace owns its own `CentralModBar` instance with identical pill set; the generator-editor registration is shared — see below.)
  - BOTTOM-LEFT: shared active-generator host (same API `registerGeneratorPage`/`setActiveGenerator`).
  - BOTTOM-RIGHT: `setFxMatrixView(FxMatrixView*)` (non-owned, like `setModMatrixView`).

**Files to EDIT:**
- `Source/PluginEditor.h`:
  - Add members: `std::unique_ptr<FxWorkspace> fxWorkspace_;`, `std::unique_ptr<FxMatrixView> fxMatrixView_;`, 3 FX-slot `ParamPage* fxSlotPages_[3]{};` (editor-owned via `generatedPages_`), header toggle `juce::TextButton fxModeButton_ { "FX" };` + `juce::TextButton synthModeButton_ { "Synth" };` (or a single toggle). Add `bool fxModeActive_ = false;`.
- `Source/PluginEditor.cpp`:
  - **`Section` enum** (`:74`) + `sectionForId` (`:78`): add `Section::Fx` and `Section::FxMatrix`; map `id.startsWith("fx_")||id.startsWith("fx1")||...||id.startsWith("fx3")` → `Section::Fx` (per-slot params); `id.startsWith("fxmod")` → `Section::FxMatrix`. Put these BEFORE the `"mod"` rule (so `fxmod` isn't caught by... actually `fxmod` doesn't start with `mod`, safe, but keep order tidy). Note: must NOT let `"fx..."` fall into existing prefixes.
  - **Page-gen spec table** (`:2115`): add FX-slot page specs (one ParamPage holding fx1_/fx2_/fx3_ groups, or three pages) + skip `Section::FxMatrix` (like `ModMatrix` early-continue at `:2145`).
  - **Page-gen loop** (`:2260`): route `Section::Fx` pages into `fxWorkspace_->setFxSlotPage(...)`; capture generator pages (ENV/LFO/SEQ/ARP/MODIFIERS) for **both** workspaces (shared). Build `fxMatrixView_` and `fxWorkspace_->setFxMatrixView(...)`.
  - **Shared generator editor**: register the SAME generator pages into `fxWorkspace_` (the same `envPage`/`lfoPage`/etc. pointers). The mode toggle reparents the active generator into whichever workspace is visible (single active selection per design decision §UI).
  - **Header layout** (`:3045-3065`): insert the `[Synth]/[FX]` toggle between `partCombo_` and `multiButton_`. Add `synthW`/`fxW` widths to `clusterW`; place `synthModeButton_` + `fxModeButton_` after `partCombo_` (before the gap + `multiButton_`). Format target: `Part [Part 1] [Synth] [FX] [Multi]`.
  - **Mode toggle wiring**: onClick swaps `pageSelector_` content — promote `pageSelector_` to a real 2-tab bar (`setTabBarDepth(kPageTabsH)`, add tabs "SYNTH"→synthWorkspace_, "FX"→fxWorkspace_) OR swap the single tab's content on click. Recommend: **make `pageSelector_` a visible 2-tab bar** (least special-casing; reuses JUCE's tab mechanism). When the Multi/Global overlay is shown, hide/take-precedence as today.
  - **`resized`**: keep `pageSelector_` filling `area`; both workspaces size themselves in their own `resized()`.
  - **Teardown order**: declare `fxMatrixView_` and `fxWorkspace_` with the same non-owned-hosting discipline as `modMatrixView_`/`synthWorkspace_` (`PluginEditor.h:547-570` comment): `fxMatrixView_` declared BEFORE `fxWorkspace_` (reverse-destruction detaches before delete).

**Acceptance:** builds; `parvati_editor_test` (layout sanity) green; the FX tab shows 3 slot panels + mod bar + matrix; toggling Synth/FX swaps the workspace; switching parts reloads FX params. Modulator pills edit the same synth values in both modes.

**Risk:** Sharing one generator page between two hosts requires reparenting on mode switch — verify no double-parent / dangling. The cleanest: only the **visible** workspace hosts the active generator; on mode toggle, reparent it into the newly-visible workspace. Test under the editor coverage tool (`tools/editor_test.cpp`).
**Dependency:** Phase 2 (fx* param IDs must exist before pages can be generated).

---

### Phase 5 — Tests + CHANGELOG
**Files to CREATE:**
- `tests/fx_preset_test.cpp` — mirror `tests/parvati_preset_test.cpp`: set fx* params on part 0 + part 3, `serializeParvatiMulti` → apply into a 2nd processor → assert all fx*/fxmod* raw values match across parts; assert a `.parvati` patch round-trips the current part's FX. Assert `.PRO`/`.MUL` save/load leaves fxState at defaults (FX dropped).
- **CMakeLists.txt** (`:~395`, mirror the `parvati_preset_test` target):
  ```cmake
  add_executable(parvati_fx_preset_test tests/fx_preset_test.cpp)
  target_include_directories(parvati_fx_preset_test PRIVATE Source)
  target_compile_features(parvati_fx_preset_test PRIVATE cxx_std_17)
  target_link_libraries(parvati_fx_preset_test PRIVATE Parvati)
  ```

**Files to EDIT:**
- `tests/host_state_test.cpp` — add FX assertions: set fx params on several parts, `getStateInformation`→`setStateInformation` into a 2nd processor, assert fxState round-trips (binary v2). Assert a v1 blob (hand-crafted / old) loads with FX defaults.
- `tests/concurrency_test.cpp` — add FX param mutations to the MT op set (toggle `fx1_enabled`, change `fx1_drywet`, add an `fxmod` slot, change `fx_order`) while the AT runs. Run under TSAN (`PARVATI_ENABLE_TSAN=ON`, per `CONTRIBUTING.md`).
- `CHANGELOG.md` — add an "FX section" entry.

**Acceptance:** `cmake --build build -j 8` builds all; `./build/parvati_fx_preset_test`, `./build/parvati_host_state_test`, and the full suite pass; concurrency test clean under TSAN.

---

## Files to Modify (summary)
| File | Change |
|---|---|
| `Source/dsp/fx/FxProcessor.h` | NEW — abstract effect base + factory |
| `Source/dsp/fx/FxProcessors.h/.cpp` | NEW — GainPan/Delay/Reverb/Chorus placeholders |
| `Source/dsp/fx/FxChain.h/.cpp` | NEW — 3-slot chain, series/parallel, dry/wet |
| `Source/SynthEngine.h` | CONTRACT enums + `PartFxState`, `Part::fxState`, `fxChains_`, `fxOutputBuffers_`, `renderPartFx`, getters/setters |
| `Source/SynthEngine.cpp` | `prepare` FX sizing; MT setters; `renderPartFx`; binary v2 capture/restore |
| `Source/ParameterLayout.h` | add `isFx` flag |
| `Source/ParameterLayout.cpp` | FX descriptors (71 params) + `makeFxDests` |
| `Source/PluginProcessor.h/.cpp` | `applyFxParameter`; `parameterChanged`/`applyParameterToEngine`/`loadPartIntoApvts` branches; `processBlock` FX render + main-bus source swap; `.PRO`/`.MUL` `||d.isFx` skips |
| `Source/ParvatiPreset.cpp` | `partRaw` isFx branch; `applyParvatiMulti` isFx branch |
| `Source/ui/FxMatrixView.h/.cpp` | NEW — clone of ModMatrixView (16 slots, fxmod prefix, FX dests) |
| `Source/ui/FxWorkspace.h/.cpp` | NEW — clone of SynthWorkspace (3 slot panels, shared bar+editor) |
| `Source/PluginEditor.h/.cpp` | `[Synth]/[FX]` toggle; `Section::Fx/FxMatrix`; FxWorkspace/FxMatrixView wiring; shared generator editor; header layout |
| `tests/fx_preset_test.cpp` | NEW — YAML + .PRO/.MUL FX round-trip |
| `tests/host_state_test.cpp` | binary FX round-trip + v1 back-compat |
| `tests/concurrency_test.cpp` | FX mutations in MT op set |
| `CMakeLists.txt` | `parvati_fx_preset_test` target |
| `CHANGELOG.md` | FX entry |

## New Files
- `Source/dsp/fx/FxProcessor.h`, `Source/dsp/fx/FxProcessors.h/.cpp`, `Source/dsp/fx/FxChain.h/.cpp`
- `Source/ui/FxMatrixView.h/.cpp`, `Source/ui/FxWorkspace.h/.cpp`
- `tests/fx_preset_test.cpp`

## Dependencies
- Phase 0 (DSP) → independent.
- Phase 1 (engine) → needs Phase 0 (`FxChain`).
- Phase 2 (params) → needs Phase 1 (engine setters + `fxState`).
- Phase 3 (serialization) → needs Phase 2 (`isFx` + `fxState`).
- Phase 4 (UI) → needs Phase 2 (fx* param IDs for page generation).
- Phase 5 (tests) → needs Phase 3 + 4. Concurrency test needs Phase 1+2.
- Each phase is a compile+test checkpoint; do not start a dependent phase until the prior one builds green in `build/`.

## Risks
- **Threading correctness (highest).** FX state is MT-write/AT-read. Must follow the exact `fxDirty_` release-store / acq_rel-exchange pattern (Phase 1). The concurrency test under TSAN is the guard — run it 5× (races are timing-dependent, per CONTRIBUTING.md).
- **Generator-page sharing (SYNTH↔FX).** Reparenting one editor-owned page between two workspace hosts must never double-parent. Mitigation: only the visible workspace hosts the active generator; reparent on toggle. Verify with `tools/editor_test.cpp`.
- **Main-bus source swap changes float-summation order** — not bit-identical to the pre-FX mix. Audibly identical; if any test asserts exact sample values on the main bus, it may need a tolerance update (check `realtime_test`/`mod_audio_test`).
- **`sectionForId` prefix collisions**: `fxmod` vs `fx` vs existing `mod`. Add FX rules before the generic `mod` rule and test the routing.
- **Binary v2 forward-compat**: an older Parvati (v1-only) reading a v2 host blob rejects it and falls back to legacy APVTS restore (acceptable, documented). DAW projects saved on v2 won't fully restore on v1 — note in CHANGELOG.
- **JUCE `juce::dsp` prepare/reset semantics**: DelayLine/Chorus/Reverb must be `prepare`d with a `ProcessSpec` and never allocate in `process`. Reserve max-block sizes.
- **`setFxModSlot` torn state**: when one of source/dest/amount changes, re-read the two siblings from fxState/APVTS and write all three atomically (under the same `fxDirty_` publish) to avoid a transient inconsistent matrix slot.

## Explicit Non-Goals (carry over from `FX_DESIGN_DECISIONS.md`)
- Sample-accurate FX modulation (block-rate only).
- Drag-to-reorder slots (up/down buttons via `fx_order` suffice).
- A rich effect library (5 placeholders suffice).
- Post-FX per-part aux buses (aux stays dry).
- Undo/redo beyond APVTS (free for FX params already).
- Pixel-perfect UI design (copy the synth layout; tune later).

---

**Executive summary.** The feature is delivered in 6 compile-gated phases on the `fx` branch, building in `build/` (Debug) after each: (0) standalone FX DSP core — `FxProcessor`/`FxProcessors`/`FxChain` under `Source/dsp/fx/`; (1) engine integration — `PartFxState` + per-part stereo `FxChain`s, `renderPartFx()` inserted post-render in `processBlock`, main-bus sourced from FX-output buffers (aux stays dry), MT→AT via an `fxDirty_` flag; (2) 71 new `isFx` APVTS params wired through `applyFxParameter`/`parameterChanged`/`loadPartIntoApvts`; (3) serialization — `.parvati` YAML round-trips FX for free via the existing per-part `params:` map (plus binary host-state bump to v2 with a length-prefixed FX block, and `|| d.isFx` skips so `.PRO`/`.MUL` stay Ambika-compatible); (4) UI — a `[Synth]/[FX]` header toggle, an `FxWorkspace` clone, an `FxMatrixView` clone (16 slots, separate FX destinations), and the shared synth modulator editor; (5) tests (`parvati_fx_preset_test`, extended `host_state_test` + `concurrency_test` under TSAN) + CHANGELOG. Estimated ~8 new files and ~10 edited files (engine, params, processor, preset, editor, 3 tests, CMake, CHANGELOG).
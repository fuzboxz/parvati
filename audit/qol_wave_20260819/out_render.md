# Parvati — Render-Quality / Offline-Render Audit

## 1. How oversampling + offline detection work today

**Oversampling (filter-only, per-voice).** `uiOversampling_` (1/2/4/8, default 2, `PluginProcessor.h:485`) drives everything. `ParvatiAudioProcessor::setOversamplingFactor` (`PluginProcessor.cpp:1885-1899`) clamps the factor, persists via `setUiOversampling` (1892), rebuilds the latency probe `rebuildOsLatencyProbe()` (1831-1864, message thread, min-phase IIR half-band, max quality, integer latency), then calls `engine_.setOversamplingFactor` (1897) and sets `latencyDirty_` (1898). `SynthEngine::setOversamplingFactor` (`SynthEngine.h:414-421`) loops all voices → `AmbikaVoice::setOversamplingFactor` (`AmbikaVoice.cpp:103-136`): **pre-builds** the replacement `juce::dsp::Oversampling` on the message thread (`buildOversamplingFor`, 147-168, `initProcessing(40)`) and stages it via `pendingOs_`/`osStageState_`. The audio thread installs with **pointer moves only** in `AmbikaVoice::fillInternalBlock` (`AmbikaVoice.cpp:382-411` → `consumeStagedOversampling`, 188-214): park displaced object (freed by the 60 Hz message-thread reaper `reapRetired` / `reapRetiredAudioObjects`, `AmbikaVoice.cpp:233-244`, timer at `PluginProcessor.cpp:218-224`), install staged object, `prepareFilterAtOsRate()` (resets filter state → click-free). Idle voices install on their next note (idle self-gate at `AmbikaVoice.cpp:682-694`).

**Latency.** `computePluginLatency` (`PluginProcessor.cpp:1867-1882`) = Lagrange 2 internal samples + staged OS latency (input samples × hostRate/39216), clamped 0..4096. Reported in `prepareToPlay` (322-331) and re-reported from `processBlock` when `latencyDirty_` (437-444).

**Offline detection.** `setNonRealtime` (`PluginProcessor.cpp:389-400`) stores `nonRealtime_` atomic only; comment explicitly defers max-quality mode. `processBlock` re-polls `isNonRealtime()` each block as fallback (`PluginProcessor.cpp:427-428`, header `PluginProcessor.h:65-71,497-499`). Not implemented beyond detection.

**Tail.** `getTailLengthSeconds()` returns `0.0` (`PluginProcessor.h:54`).

## 2. Auto-max-quality offline render — recommendation

**Trigger:** implement in `setNonRealtime` (message thread — allocation is safe there). **Do not** drive the switch from the per-block poll (427): the audio thread cannot stage.

**Key constraint — don't reuse `setOversamplingFactor` verbatim:** it writes `setUiOversampling(factor)` (1892), which would (a) persist 8x into host state and (b) desync the Settings combo. Add an internal `applyOversamplingFactor(int)` that does steps 1893-1898 only (probe rebuild + engine staging + `latencyDirty_`), and make `rebuildOsLatencyProbe()` take the factor as a parameter instead of reading `getUiOversampling()` (1839).

- **Enter offline:** if `!JUCE_IOS` and `getUiOversampling() != 8`: save `offlineSavedOs_ = getUiOversampling()`, call `applyOversamplingFactor(8)`. Active voices install at their next 40-sample internal block via pointer moves — no AT allocation, click-free (filter state reset is the existing documented behaviour, `AmbikaVoice.cpp:382-394`). Idle voices install on next note.
- **Latency re-report:** `latencyDirty_` → first offline `processBlock` re-reports (437-444); `prepareToPlay` (322-331) covers hosts that re-prepare for the bounce. JUCE `setLatencySamples` during an offline render is honored by most hosts pre-first-block; since `setNonRealtime(true)` precedes render start, this lands in time. The 4096-sample clamp (1882) is ample (8x min-phase IIR latency is a handful of input samples).
- **Exit offline:** `setNonRealtime(false)` → `applyOversamplingFactor(offlineSavedOs_)`. Guard double-entry (if already offline, ignore). Reaper capacity `kRetiredOsCap` = 2 retired objects (`AmbikaVoice.h:361-366`): enter/exit/enter within one 60 Hz reaper interval (16384 taps × 96 voices ≈ MBs) falls back to `delete old` on the message thread at `parkRetiredOversampling` (`AmbikaVoice.cpp:216-231`) — acceptable, but worth noting rapid bounce cycles briefly spike memory.
- **iOS exclusion:** keep the `#if !JUCE_IOS` guard; measured 8x = 2.3-3.7x realtime on A12 (`PluginProcessor.cpp:1708-1723` rationale) — even offline that's a multi-minute bounce; the state-restore clamp at 1708-1723 (4x/8x → 2x) documents the policy.
- **FX with own fixed internal oversampling (unaffected by the voice OS factor):** Fv1LutDistortion/Fv1Overdrive (6x Warps polyphase FIR, `Fv1LutDistortion.h:22-82`), FxWavefolder (6x, `FxProcessors.h:256-279`), FxRingModulator (6x, `FxProcessors.h:331-359`, latency reported via `FxProcessor::latency()` `FxProcessor.h:58`). The uiOversampling_ factor only touches the per-voice analog filter — that's correct scoping for the auto mode.

**Tail length.** Max decays: Plate 0.1..4.0 s (`Fv1PlateReverb.cpp:42-44`, predelay cap 4096 ≈ 100 ms at 32k), Spring 0.2..4 s (`Fv1Spring.h:11`), Room 0.1..3 s (`Fv1Room.h:9`), CVerb time→0.30..0.95 tank feedback + 200 ms predelay (`FxProcessors.cpp:102,131` — at 0.95 the tail exceeds 10 s), Echo 10..470 ms/repeat with fb 0.995 "reads as infinite" (`Fv1Echo.cpp:24-28`), ClockedDelay tempo-synced fb 0.95. **Recommend 4.5 s** (4 s plate/spring + 0.2 s predelay + margin). Trade-offs: a finite value truncates CVerb-max/Echo-max/LoopingDelay-frozen (`FxProcessors.cpp:294-295`) tails no matter what; larger values (8-10 s) bloat every freeze/bounce with silence. Alternative: cache a per-part max-decay atomic updated on FX param change and return its max — JUCE only requires it be fast and stable-ish; start with the constant.

## 3. Other render-path QoL

- **Block-size clamp truncation** (`PluginProcessor.cpp:513-536`): a host offline-rendering blocks larger than the prepared size silently drops the tail of *every* block (audible corruption, not just truncation). Fix: chunk the render — loop `engine_.renderNextBlock`/`renderPartFx` in `preparedMaxBlock_`-sized sub-blocks within one processBlock instead of clamping. Worst buffer is Wavefolder `osL_` at maxBlock*6+8 (`FxProcessors.h:279`).
- **DC blocker** already prepared/reset per `prepareToPlay` (`PluginProcessor.cpp:296-313`) — fine for renders.
- **releaseResources** (`PluginProcessor.cpp:338-364`) kills all voices + MIDI queue but does **not** reset FX tails (FxChain state survives, re-prepared on next `prepareToPlay`) — intentional and fine; no change needed.
- `buffer.clear()` before render (`PluginProcessor.cpp:458`) ensures no garbage in the silent tail of oversized blocks.

## Start Here
`Source/PluginProcessor.cpp:389` (setNonRealtime) + `:1885` (setOversamplingFactor) — the auto-max seam is entirely between these two.

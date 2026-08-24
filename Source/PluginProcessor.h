// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// ParvatiAudioProcessor — the plugin's AudioProcessor. Owns the SynthEngine
// (a kNumVoices = 96 AmbikaVoice pool — 6 parts x 16 max slots) and an
// AudioProcessorValueTreeState (APVTS) that exposes every Ambika patch/part
// parameter. Parameter changes are bridged to the integer engine by writing
// the corresponding Patch/Part struct byte into every active voice (see
// ParameterLayout + SynthEngine::applyPatchByte).

#pragma once

#include <juce_audio_devices/juce_audio_devices.h>   // juce::MidiMessageCollector
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>   // juce::dsp::Oversampling (filter-OS latency probe)

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "MulExport.h"
#include "ParameterLayout.h"
#include "SynthEngine.h"
#include "MidiParameterMap.h"

class ParvatiAudioProcessor : public juce::AudioProcessor,
                              private juce::AudioProcessorValueTreeState::Listener
{
public:
    ParvatiAudioProcessor();
    ~ParvatiAudioProcessor() override;

    //==========================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    // F-ios-lc-2 (bug hunt 2026-08-19): hosts tear down render resources on
    // interruption / route change (AUv3 deallocateRenderResources -> this
    // hook). Clears held notes + queued MIDI so a resume starts from silence
    // — see the .cpp definition for where the split happens.
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    // Bypassed render: a bypassed synth must output silence. Overridden (rather
    // than inheriting juce::AudioProcessor::processBlockBypassed, which
    // jasserts in debug when getLatencySamples() > 0 — ours is nonzero whenever
    // filter oversampling is active) to clear every output bus. No state
    // flushes: the voices/FX keep running internally so an un-bypass resumes
    // where it left off (the wrapper's dry signal passes the host's bypass).
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==========================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    //==========================================================================
    const juce::String getName() const override { return "Parvati"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return engine_.getTailLengthSeconds(); }

    // MPE (MIDI Polyphonic Expression). The engine routes pitch bend / channel
    // pressure / CC74 per-voice by MIDI channel (unified per-channel routing,
    // SynthEngine::handlePitchWheel/handleChannelPressure/handleController), so
    // the plugin is a genuine MPE receiver. Declaring this lets MPE-aware hosts
    // expose per-note expression. Harmless for standard single-channel MIDI:
    // all notes share one channel => the routing is channel-wide (and pitch
    // bend, previously a no-op, now actually bends the oscillator).
    bool supportsMPE() const override { return true; }

    // ---- Offline-render detection (host bounce / freeze). setNonRealtime() is
    // called by the host wrapper on the message thread when it switches to
    // offline rendering; isNonRealtime() is a cheap fallback polled in
    // processBlock. OFFLINE AUTO-MAX QUALITY (desktop only): entering offline
    // bumps the per-voice filter oversampling to 8x (CPU is unconstrained in a
    // bounce); leaving offline restores the user's saved factor. The 8x is
    // applied WITHOUT persisting (applyOversamplingFactor) so host state and
    // the Settings combo keep the user's choice; isOfflineOversamplingActive()
    // exposes the state (tests).
    void setNonRealtime (bool isNonRealtime) noexcept override;
    bool isNonRealtimeRender() const noexcept { return nonRealtime_.load (std::memory_order_relaxed); }
    bool isOfflineOversamplingActive() const noexcept { return offlineSavedOs_ >= 0; }

    //==========================================================================
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    //==========================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Thread-safe MIDI injection surface for UI click-play (Phase 4a). The
    // MidiMessageCollector queues message-thread MIDI and the audio thread
    // merges it into the block buffer right before rendering (additive with
    // host MIDI). addMessageToQueue requires a non-zero timestamp, so the
    // message's timestamp is stamped here.
    void addMidiEvent (const juce::MidiMessage& message)
    {
        auto m = message;
        m.setTimeStamp (juce::Time::getMillisecondCounterHiRes() * 0.001);
        midiCollector_.addMessageToQueue (m);
    }

    // ---- UI settings (persisted in processor state, Phase 4a) ----
    // F-ios-lc-1 (iOS hunt 2026-08-19): these members are read/written from
    // BOTH the message thread (SettingsPanel combos, editor zoom/language) and
    // host threads (getStateInformation/setStateInformation — AUv3 hosts call
    // them on non-message threads for session saves / autosaves). juce::String
    // is refcounted (NOT safe under concurrent read/write — a torn String is a
    // UAF class); the scalars tear silently. Every access below now takes
    // uiPrefsLock_; the state serialize/restore paths snapshot the whole family
    // under ONE lock acquisition. (The engine state is separately atomic/
    // seqlocked — this lock only covers the UI-preference mirror.)
    juce::String getUiTheme() const               { const std::lock_guard<std::mutex> l (uiPrefsLock_); return uiThemeName_; }
    double       getUiZoom() const                { const std::lock_guard<std::mutex> l (uiPrefsLock_); return uiZoom_; }
    bool         getUiTooltips() const            { const std::lock_guard<std::mutex> l (uiPrefsLock_); return uiTooltips_; }
    bool         getUiSmoothing() const           { const std::lock_guard<std::mutex> l (uiPrefsLock_); return uiSmoothing_; }
    int          getUiOversampling() const        { const std::lock_guard<std::mutex> l (uiPrefsLock_); return uiOversampling_; }
    // Live mod-feedback animation cadence (docs/LIVE_MOD_FEEDBACK_DESIGN.md):
    // the rate at which the CentralModBar history strips, the EnvelopeDisplay
    // stage markers and the FilterResponseDisplay live curve re-read the
    // engine telemetry frame. 5..60 Hz, default 30. Lower = fewer repaints /
    // less GPU load on constrained hosts (iPad AUv3 panes); every poll is
    // change-gated, so this only caps the cadence — it never adds idle work.
    int          getUiRefreshHz() const           { const std::lock_guard<std::mutex> l (uiPrefsLock_); return uiRefreshHz_; }
    // Editor chrome language code ("auto" / "en" / "fr"). "auto" defers to the
    // OS locale. Persisted so the chosen language survives host save/restore.
    juce::String getUiLanguage() const             { const std::lock_guard<std::mutex> l (uiPrefsLock_); return uiLanguage_; }
    // Mod-matrix lamp colour policy (both matrices at once). True: the lamp
    // ON colour follows the row's modulator category colour. False: the lamp
    // stays the theme accent. Persisted so the choice survives host
    // save/restore. Default true.
    bool         getUiModLampCategory() const      { const std::lock_guard<std::mutex> l (uiPrefsLock_); return uiModLampCategory_; }
    void setUiTheme (juce::String name)           { const std::lock_guard<std::mutex> l (uiPrefsLock_); uiThemeName_ = std::move (name); }
    void setUiZoom (double z)                     { const std::lock_guard<std::mutex> l (uiPrefsLock_); uiZoom_ = z; }

    // ---- Arpeggiator clock: manual tempo fallback (2026-08-19 AUv3 wave) ----
    // Hosts that supply no musical context to the plugin (the GarageBand-
    // class AUv3 hosts; also the Standalone app, which has no transport)
    // used to run the arpeggiator clock at a hard-coded 120 BPM. processBlock
    // now resolves the clock tempo as: HOST bpm when the playhead carries
    // one, else this MANUAL value (persisted in the plugin state alongside
    // the UI preferences; default 120 = the old hard-coded behaviour). The
    // atomic is written by the message thread (Settings slider) and by host
    // threads during state restore, read by the audio thread every block.
    int  getManualTempoBpm() const                { const std::lock_guard<std::mutex> l (uiPrefsLock_); return manualTempoBpm_.load (std::memory_order_relaxed); }
    void setManualTempoBpm (int bpm);
    // True when the LAST processBlock resolved the clock from a HOST tempo
    // (false = the manual fallback is driving). Optimistic `true` default so
    // nothing fires before the first audio block (see the editor's
    // no-host-tempo transient hint). Relaxed — advisory display only.
    bool   isHostTempoPresent() const noexcept    { return hostTempoPresent_.load (std::memory_order_relaxed); }
    // The bpm value the last block actually fed the arpeggiator clock (host
    // or manual), for the Settings status line + tests. Relaxed advisory.
    double getLastClockBpm() const noexcept        { return static_cast<double> (lastClockBpm_.load (std::memory_order_relaxed)); }
    void setUiTooltips (bool b)                   { const std::lock_guard<std::mutex> l (uiPrefsLock_); uiTooltips_ = b; }
    void setUiSmoothing (bool b)                  { const std::lock_guard<std::mutex> l (uiPrefsLock_); uiSmoothing_ = b; }
    void setUiOversampling (int n)                { const std::lock_guard<std::mutex> l (uiPrefsLock_); uiOversampling_ = n; }
    void setUiRefreshHz (int hz)                  { const std::lock_guard<std::mutex> l (uiPrefsLock_); uiRefreshHz_ = juce::jlimit (5, 60, hz); }   // clamped: an out-of-range restored state never drives an absurd timer
    void setUiLanguage (juce::String code)        { const std::lock_guard<std::mutex> l (uiPrefsLock_); uiLanguage_ = std::move (code); }
    void setUiModLampCategory (bool b)            { const std::lock_guard<std::mutex> l (uiPrefsLock_); uiModLampCategory_ = b; }

    // ---- Thermal-state awareness (F-ios-perf-2, iOS hunt 2026-08-19) ----
    // Sustained 6-part multitimbral play keeps an iPad core near its budget;
    // iOS responds by throttling (intermittent crackle the user cannot
    // attribute). The policy is DELIBERATELY advisory-only — NEVER auto-change
    // sound-affecting state (filter quality) without the user. The pure
    // mapping below is the entire policy; the iOS-only sampler (see
    // DeferredParamTimer::timerCallback) feeds it from NSProcessInfo at ~1 Hz
    // and stores the result in thermalHint_. The editor's 30 Hz timer may
    // surface it (3-line follow-up in the parent lane: read getThermalHint()
    // and drive the transient status label).
    enum class ThermalLevel { Nominal = 0, Fair = 1, Serious = 2, Critical = 3 };
    enum class ThermalAction { None = 0, Hint = 1, StrongHint = 2 };
    static constexpr ThermalAction thermalActionForLevel (ThermalLevel level) noexcept
    {
        // NSProcessInfoThermalState semantics: Nominal/Fair are normal
        // operation (Fair = fans/dissipation, invisible on passively cooled
        // iPads); Serious = sustained thermal pressure (start hinting);
        // Critical = the OS is about to intervene (insist).
        return level == ThermalLevel::Serious   ? ThermalAction::Hint
             : level == ThermalLevel::Critical  ? ThermalAction::StrongHint
                                                : ThermalAction::None;
    }
    // Current thermal hint (ThermalAction as int; None on desktop / before
    // the first sample). Relaxed load — advisory display only.
    int getThermalHint() const noexcept { return thermalHint_.load (std::memory_order_relaxed); }

    // NOTE: the UI font mode selector was removed — the whole UI now uses the
    // system default sans-serif (see ParvatiLookAndFeel::appFont). The field
    // below is kept ONLY so legacy saved states that carry a ui_font_mode
    // property still load (the value is read + re-saved but no longer applied).

    // Enable/disable optional per-sample parameter smoothing (knob / automation
    // zipper-noise reduction). Propagates to all voices + persists the pref.
    void setParameterSmoothing (bool smoothing);

    // Enable optional FILTER oversampling (1 / 2 / 4 / 8). Default 2 (the
    // constructor applies it); 1 = the bit-identical no-OS path. Propagates to
    // every voice + persists the pref + recomputes + reports the plugin latency
    // (the OS filter adds a few samples of group delay on top of the resampler
    // latency).
    void setOversamplingFactor (int factor);

    // Exposed for the (Phase 4) GUI and external control.
    SynthEngine& getEngine() noexcept { return engine_; }
    juce::AudioProcessorValueTreeState& getApvts() noexcept { return apvts; }

    // The APVTS-owned UndoManager. Declared before `apvts` so it is constructed
    // first and can be passed to the APVTS ctor — every parameter write then
    // becomes undoable (knob drags, combo changes, getParameterAsValue writes,
    // and the ParamControl reset/randomize context-menu actions). It is NOT
    // serialized: only the APVTS ValueTree state is (get/setStateInformation),
    // and replaceState() clears the undo history on restore.
    // FIXED-SIZE undo history (public constants; see the undoManager_ member
    // for the full rationale): an explicit unit cap + transaction floor so the
    // stack never grows without bound — JUCE drops the OLDEST transactions
    // once their cumulative getSizeInUnits() accounting passes the cap (a
    // ValueTree property transaction ≈ sizeof(itself) ≈ ~120 units, so 16000
    // units ≈ a ~130-step history; APVTS routes every parameter write through
    // the manager). Chosen over runtime memory-pressure machinery: a bounded
    // per-instance history keeps an AUv3 extension process (several hosted
    // instances, one memory budget) predictable with no dynamic machinery.
    static constexpr int kUndoMaxUnits        = 16000;   // ≈ ~130 undo steps
    static constexpr int kUndoMinTransactions = 16;      // always-undoable floor
    juce::UndoManager& getUndoManager() noexcept { return undoManager_; }

    // The editor's ONLY undo/redo entry points (header buttons + keyboard
    // shortcuts). A part switch invalidates every recorded action's part
    // context (a replay of an old action would write Part A's values into
    // Part B's engine storage via parameterChanged), and JUCE's
    // append-after-listeners ordering plus the 10 Hz APVTS tree-flush timer
    // can leave late entries after onPartSelect's synchronous clear — these
    // sweep once more, then replay. Always call these instead of
    // getUndoManager().undo()/redo() from UI code.
    void undoSafe();
    void redoSafe();

    // ---- Realtime overrun probe (diagnostic) ----
    // Measures each processBlock's wall-clock time against its real-time budget
    // (numSamples / sampleRate). audioLoadCurrent_ is the latest block's ratio
    // (1.0 = fully saturated = an xrun); audioLoadPeak_ is the worst seen since
    // the last reset; audioOverrunCount_ counts blocks that exceeded budget.
    // Read from the GUI timer (message thread); reset via resetAudioLoadProbe().
    // Used to confirm whether audible crackle correlates with audio-thread
    // overruns (e.g. GUI render load starving the RT thread).
    double   getAudioLoadCurrent() const noexcept { return audioLoadCurrent_.load (std::memory_order_relaxed); }
    double   getAudioLoadPeak() const noexcept    { return audioLoadPeak_.load (std::memory_order_relaxed); }
    uint64_t getAudioOverrunCount() const noexcept { return audioOverrunCount_.load (std::memory_order_relaxed); }
    void     resetAudioLoadProbe() noexcept       { audioLoadResetReq_.store (true, std::memory_order_relaxed); }

    // Push the current value of every APVTS parameter into all voices.
    // Public so the GUI / preset loader can deterministically apply a freshly
    // loaded set of parameters. (Live host/GUI edits also apply automatically
    // via the APVTS listener, but that dispatch is synchronous on the CALLING
    // thread -- the message thread for GUI edits, the AUDIO thread for host
    // automation and the CC/NRPN map -- with audio-thread-origin arp/seq/
    // part_select edits deferred to the 60 Hz message-thread drain; an
    // explicit sync is still used after bulk loads and in tests.)
    void syncAllParamsToEngine();

    // ---- Deferred-param diagnostics (tests / debug UI) ----
    // Ring overflow since construction: non-zero means audio-thread-origin
    // arp/seq/part_select events were DROPPED (64-slot latest-wins ring @60 Hz
    // drain => sustained >3840 events/s required; real controllers stay far
    // below). Pending count = entries not yet drained (convergence polling in
    // the concurrency test).
    uint64_t getDroppedDeferredCount() const noexcept
    {
        return deferredParams_.dropped.load (std::memory_order_relaxed);
    }
    uint32_t getPendingDeferredCount() const noexcept
    {
        return deferredParams_.count.load (std::memory_order_relaxed);
    }

    // ---- Ambika .PRO patch support ----
    // Load a parsed Ambika program (raw Patch[112] + Part[84] bytes) into the
    // APVTS (so the GUI reflects it) and the engine. Returns true on success.
    bool loadProgramFromBytes (const uint8_t* patch112, const uint8_t* part84);
    // Load + parse a .PRO file. Returns true on success.
    bool loadProgramFile (const juce::File& file);
    // Gather the CURRENT part's APVTS values as Patch[112] + PartData[84]
    // bytes: every descriptor except isArp / isOption / isFx (they carry no
    // patch or part byte); sequencer params convert directly (the shared
    // helper early-returns 0 for them); part/sequencer bytes route to @p part,
    // the rest to @p patch. The shared bridge of saveProgramFile and
    // saveMultiFile (the current part of the .MUL captures live edits).
    void gatherCurrentPartBytes (std::array<uint8_t, 112>& patch, std::array<uint8_t, 84>& part) const;
    // Save the CURRENT part's Patch[112] + PartData[84] (gathered from the
    // APVTS) as an Ambika .PRO — the exact inverse of loadProgramFile. A saved
    // file re-loads to the same engine state. Returns true on success.
    bool saveProgramFile (const juce::File& file);
    // Where the factory Ambika patches are installed (FACTORY root; banks A/B/F/S
    // are subfolders), where the factory multis live, and the user save area.
    static juce::File getFactoryPatchDir();
    static juce::File getUserPatchDir();

    // ---- Parvati-native preset format (.parvati, human-editable YAML) ----
    // A full-fidelity format that carries Parvati-only settings (vca_curve,
    // filter_card) and arp/seq that the Ambika .PRO/.MUL byte format drops.
    // Coexists with .PRO/.MUL (kept for Ambika interop).
    bool saveParvatiPatchFile (const juce::File& file);   // current part
    bool loadParvatiPatchFile (const juce::File& file);
    bool saveParvatiMultiFile (const juce::File& file);   // all 6 parts
    bool loadParvatiMultiFile (const juce::File& file);

    // Multi-load hygiene: reset every Part's voice slots to the ENGINE INIT
    // allocation (Part 0 = 6 voices, the popcount of the constructor's 0x3f
    // init bitmask; Parts 1..5 disabled) BEFORE a multi file applies its own
    // per-part data. Thus a file that does not carry voice settings for a
    // Part never inherits the PREVIOUS multi's leftover counts (stale-voice
    // bug: a short/legacy .parvati parts list, or any future MultiData-less
    // .MUL acceptance). Public setters only: setPartVoiceSlots for the
    // enabled Part 0 and the legacy setPartVoiceAllocation(part, 0) disable
    // path for the rest. Called by loadMultiFile + loadParvatiMultiFile.
    void resetVoiceSlotsToInit();

    // ---- Ambika .MUL (multi) support ----
    // Load a .MUL: configures all 6 Parts (patches + PartData + MIDI channel +
    // key zone + voice allocation from MultiData.part_mapping_[], and per-part
    // arp/sequencer settings from each PartData). Shows Part 0 in the editor
    // afterwards. Returns true on success.
    bool loadMultiFile (const juce::File& file);
    // Save the whole 6-Part state as an Ambika .MUL — the exact inverse of
    // loadMultiFile. The CURRENT part's Patch/PartData bytes are gathered from
    // the APVTS (live edits); the other 5 parts are read from engine storage.
    // MultiData.part_mapping_ is rebuilt from the engine's per-part channel /
    // keyrange / voice-allocation. A saved file re-loads to the same state.
    // Save the whole 6-Part state as an Ambika .MUL — the exact inverse of
    // loadMultiFile. The CURRENT part's Patch/PartData bytes are gathered from
    // the APVTS (live edits); the other 5 parts are read from engine storage.
    // MultiData.part_mapping_ is rebuilt from the engine's per-part channel /
    // keyrange / voice-allocation. A saved file re-loads to the same state.
    //
    // Export fallback (voice-slot extension): when a Part requests more voices
    // than its voicecards (see mul_export::needsFallback), the chosen strategy
    // maps the requested voices onto the 6 hardware cards (bitmask rewrite +
    // optional polyphony-mode rewrite). ChainSplit writes more sibling
    // "-2.MUL"/"-3.MUL" unit files for physically chained Ambikas. The default
    // (0 = AsIs) is the legacy behaviour: bitmasks unchanged, slots ignored.
    // @p strategyInt is a parvati::mul_export::Strategy value passed as int to
    // keep this header light for the headless tests.
    bool saveMultiFile (const juce::File& file, int strategyInt = 0);

    // The current export Setup (requested voices / cards / poly modes), for
    // the editor's fallback dialog preview. Pure read of engine state.
    parvati::mul_export::Setup getMulExportSetup() const;
    static juce::File getFactoryMultiDir();
    // Stock init templates (Mono / Poly / Unison / Multitimbral) — full-fidelity
    // .parvati multis. <appdata>/Parvati/TEMPLATES/.
    static juce::File getTemplatesDir();
    // The name of the last loaded program (for the GUI title).
    juce::String getLoadedProgramName() const { return loadedProgramName_; }
    // Set the loaded-program name (used by the template generator so each stock
    // template's .parvati `name:` field matches its label).
    void setLoadedProgramName (const juce::String& n) { loadedProgramName_ = n; }

    // Re-read a Part's engine storage into the APVTS (engine→APVTS display
    // refresh). Public so the .parvati/.MUL multi-load epilogue can refresh the
    // editor after making engine storage authoritative.
    void loadPartIntoApvts (int part);

private:
    // ---- APVTS::Listener: a parameter changed (host / GUI / automation). ----
    // DISPATCH REALITY (JUCE 9): listener dispatch is SYNCHRONOUS ON THE
    // CALLING THREAD. Host automation arrives inside the render callback
    // (AUv3 render events / VST3 process()), and MidiParameterMap's CC/NRPN
    // setter (called from processBlock) routes through setValueNotifyingHost
    // -- so this callback can run on the AUDIO thread. Most branches only stage
    // atomics (RT-safe); the unsafe classes are DEFERRED to the message thread
    // via deferredParams_ when the callback is off the message thread (see
    // DeferredParamRing): arp/seq (they write the pendingConfig_ seqlock -- a
    // second writer tears it), part_select (loadPartIntoApvts' ~250
    // ValueTree+UndoManager writes), and the FX params (applyFxParameter builds
    // juce::String/substring/std::string lookup keys = heap traffic on the
    // render thread). The engine stages FX values through fxDirty_ atomics
    // anyway, so a <=16 ms deferral is inaudible.
    void parameterChanged (const juce::String& parameterID, float newValue) override;

    // ---- Deferred audio-thread-origin parameter writes (message-thread drain) ----
    // A fixed-capacity (64) spinlock-protected latest-wins ring. The audio
    // thread pushes (descriptor index, value) for the unsafe parameter classes;
    // a 60 Hz juce::Timer on the message thread drains each entry through the
    // normal apply path. Latest-wins coalescing (a second push of the same
    // parameter overwrites the pending entry) matches how a knob turn sends a
    // burst of values: only the last one matters. Overflow DROPS (bounded
    // memory, RT-safe) and counts into `dropped` -- with 64 slots, one
    // parameter per push and a 60 Hz drain, overflow needs a sustained
    // >3840 param-events/s of arp/seq/part_select writes, which no real
    // controller produces; the counter exists so tests can assert zero drops.
    struct DeferredParamRing
    {
        struct Entry { int index = -1; float value = 0.0f; };
        std::array<Entry, 64> slots;
        std::atomic<uint32_t> count { 0 };            // used entries (diagnostic)
        std::atomic<uint64_t> dropped { 0 };          // overflow counter (diagnostic)
        std::atomic_flag lock = ATOMIC_FLAG_INIT;     // spinlock guarding slots/count

        // RT-safe push (audio thread): acquires the spinlock (bounded hold: a
        // short scan + store), coalesces same-index entries, drops on overflow.
        void push (int index, float value) noexcept;
        // Message-thread drain: copies the ring into a FIXED-CAPACITY local
        // snapshot under the spinlock (a bounded copy of trivially-copyable
        // entries -- NO allocation inside the critical section) and clears it;
        // the returned juce::Array is built AFTER the lock is released, so a
        // heap allocation can never stall the audio thread's push() spin
        // (priority inversion -> block overrun).
        juce::Array<Entry> drain() noexcept;
    };

    // The 60 Hz message-thread drain for deferredParams_. Plain juce::Timer
    // (not HighResolutionTimer): the drain does the exact GUI-path apply
    // work (engine setters that stage atomics / loadPartIntoApvts' ValueTree
    // writes), which is message-thread work; <=16.7 ms added latency for
    // audio-thread-origin arp/seq/part_select edits only (GUI edits stay
    // synchronous), the same order as the frameDirty_ one-block staging.
    struct DeferredParamTimer : juce::Timer
    {
        explicit DeferredParamTimer (ParvatiAudioProcessor& o) : owner (o) {}
        void timerCallback() override;
        ParvatiAudioProcessor& owner;
        // F-ios-perf-2: tick counter so the thermal sampler runs every 60th
        // callback (~1 Hz at the timer's 60 Hz) instead of every tick.
        int tick = 0;
    };
    DeferredParamRing deferredParams_;
    std::unique_ptr<DeferredParamTimer> deferredTimer_;   // created in the ctor (message thread)

    // Apply one parameter's current APVTS value to the engine.
    void applyParameterToEngine (const PatchParamDescriptor& descriptor);

    // Route an arp/seq config parameter to the SynthEngine (the five arp
    // fields + the three seq lengths via kArpSeqParamMap; the seq step
    // params ride their PartData byteOffset). Message-thread only.
    void applyArpSeqParameter (const PatchParamDescriptor& descriptor, float rawValue);

    // (Re)builds the oversampling LATENCY probe for @p osFactor (NOT the
    // persisted uiOversampling_ — the offline auto-max path probes 8x without
    // persisting it). The probe mirrors the per-voice Oversampling config
    // exactly (1 channel, min-phase IIR half-band, max quality, integer latency)
    // so its getLatencyInSamples() matches what every voice adds; it is never
    // fed audio, so rebuilding it on the message thread is race-free and
    // decouples latency reporting from the audio-thread voice rebuild.
    void rebuildOsLatencyProbe (int osFactor);

    // Apply a filter-oversampling factor WITHOUT persisting it: latency-probe
    // rebuild + engine staging + latency re-report. The shared engine half of
    // setOversamplingFactor (which persists first, then calls this). Message
    // thread (it pre-builds per-voice Oversampling objects + the probe).
    void applyOversamplingFactor (int factor);

    // Total reportable plugin latency (host samples) = Lagrange resampler
    // latency + active filter-OS latency (both converted from INPUT/internal
    // samples via hostRate/internalRate).
    int computePluginLatency (double hostSampleRate) const;

    // Route a synth option (e.g. VCA curve) to the engine.
    void applyOptionParameter (const PatchParamDescriptor& descriptor, float rawValue);

    // Route a per-part FX parameter to the engine's current-Part FX setters.
    // Decodes the descriptor paramID by prefix (fx{N}_*, fx_topo, fx_order,
    // fxmod{M}_*) and calls the matching engine_.setFx* setter. For fxmod{M}_*
    // the three sibling APVTS values (source/dest/amount) are re-read and written
    // together via setFxModSlot to avoid a torn matrix slot.
    void applyFxParameter (const PatchParamDescriptor& descriptor, float rawValue);

    // Multitimbral: switch the Part being edited. Loads the new Part's stored
    // patch/part/arp/seq into the APVTS (GUI reflects it) + the engine.
    void onPartSelect (int newPart1Based);

    SynthEngine engine_;
    // The APVTS-owned UndoManager. Declared before `apvts` so it is constructed
    // first and can be passed to the APVTS ctor — every parameter write then
    // becomes undoable (knob drags, combo changes, getParameterAsValue writes,
    // and the ParamControl reset/randomize context-menu actions). NOT
    // serialized: only the APVTS ValueTree state is (get/setStateInformation),
    // and replaceState() clears the undo history on restore. Constructed with
    // the FIXED-SIZE bounds (see kUndoMaxUnits above).
    juce::UndoManager undoManager_ { kUndoMaxUnits, kUndoMinTransactions };   // constructed before apvts (member order)
    bool undoInvalidatedByPartSwitch_ = false;   // set by onPartSelect; swept by undoSafe/redoSafe
    juce::AudioProcessorValueTreeState apvts;
    juce::String loadedProgramName_ { "Init" };
    // NOTE: there is no mirrored current-part member — engine_.getCurrentPart()
    // is the single source of truth. Every write goes through
    // engine_.setCurrentPart (message thread).

    // True while loadPartIntoApvts is pushing engine storage into the APVTS.
    // Suppresses the parameterChanged -> engine re-apply feedback loop: loading
    // is engine->APVTS, so feeding the same values back into the engine is
    // redundant for byte/arp/seq params and HARMFUL for the FX mod matrix.
    // Its applyFxParameter re-reads all three sibling APVTS values (source/
    // dest/amount) and would otherwise read them stale mid-load and clobber
    // the engine fxState it is sourcing from. Message-thread-only (load +
    // listener).
    bool loadingPartIntoApvts_ = false;

    // True while setStateInformation's replaceState rewrites the APVTS from a
    // restored host state. Same feedback-suppression job as
    // loadingPartIntoApvts_: in JUCE 9 replaceState DOES fire parameterChanged
    // per changed parameter (valueTreeRedirected -> setDenormalisedValue ->
    // setValueNotifyingHost), so without this guard each restored parameter is
    // re-applied to the engine MID-RESTORE (stale/partial values racing the
    // authoritative engine-blob restore that follows). Message-thread-only.
    bool restoringState_ = false;

    // Hardware-parity MIDI CC/NRPN -> parameter mapping (spec F.4).
    MidiParameterMap midiParamMap_;

    // Thread-safe queue for UI-injected MIDI (keyboard click-play). The audio
    // thread drains it in processBlock.
    juce::MidiMessageCollector midiCollector_;

    // Persisted UI preferences (theme / zoom / tooltips / language).
    // Guarded by uiPrefsLock_ (see the accessor block above — F-ios-lc-1):
    // message-thread setters vs host-thread state save/restore. Reads/writes
    // ONLY through the accessors or under an explicit uiPrefsLock_ scope
    // (getStateInformation / setStateInformation / rebuildOsLatencyProbe).
    mutable std::mutex uiPrefsLock_;   // getters are const and lock (F-ios-lc-1)
    juce::String uiThemeName_ { "Carbon" };
    double       uiZoom_ { 1.0 };
    bool         uiTooltips_ { true };
    bool         uiModLampCategory_ { true };   // global mod-lamp colour policy
    bool         uiSmoothing_ { false };   // default OFF -> bit-identical audio
    int          uiOversampling_ { 2 };    // 1 / 2 / 4 / 8; default 2x (1 = bit-identical path)
    int          uiRefreshHz_ { 30 };       // live mod-feedback animation rate, 5..60 Hz (see getUiRefreshHz; docs/LIVE_MOD_FEEDBACK_DESIGN.md)
    int          uiFontMode_ { 0 };        // LEGACY: persisted for old states only (font selector removed; UI is sans-serif)
    juce::String uiLanguage_ { "auto" };   // editor chrome language (auto/en/fr)
    // Arp-clock manual tempo (see the public block): the atomic is the
    // audio-thread view; uiPrefsLock_ keeps setter/state-restore vs
    // getStateInformation consistent (serialized in the same snapshot).
    std::atomic<int> manualTempoBpm_ { 120 };
    // Clock-source observables, written by processBlock (audio thread), read
    // by the editor/Settings for display (relaxed — advisory only).
    std::atomic<bool>  hostTempoPresent_ { true };   // optimistic: no hint before the first block
    std::atomic<float> lastClockBpm_ { 120.0f };     // resolved value fed to the clock

    // Offline-render flag (host bounce). Updated by setNonRealtime() and kept
    // warm each block from isNonRealtime() as a host-compat fallback.
    std::atomic<bool> nonRealtime_ { false };

    // Offline auto-max-quality bookkeeping (message thread only — written by
    // setNonRealtime / prepareToPlay, never from processBlock):
    // -1 = offline boost inactive; >= 1 = the user factor to restore on exit.
    // See setNonRealtime for the full policy.
    int offlineSavedOs_ = -1;

    // Chunked-render scratch: when a host block exceeds the prepared size,
    // each >first slice receives its window's MIDI events rebased to [0, n)
    // in this buffer (clear() keeps capacity — no steady-state allocation;
    // only oversized blocks ever touch it). Audio-thread-only.
    juce::MidiBuffer sliceMidiScratch_;

    // Thermal hint (F-ios-perf-2): ThermalAction as int, written ~1 Hz by the
    // iOS-only sampler in DeferredParamTimer::timerCallback, read by the
    // editor (advisory display only). Desktop never writes it (stays None).
    std::atomic<int> thermalHint_ { 0 };

    // Last latency reported to the host, so we only call setLatencySamples when
    // it actually changes (e.g. on a sample-rate switch), not every prepareToPlay.
    int lastReportedLatency_ = -1;

    // Host sample rate cached from prepareToPlay. getSampleRate() can read 0 in
    // standalone / headless contexts (bus not fully enabled), so the audio-thread
    // latency re-report uses this authoritative value instead.
    double hostSampleRate_ = 0.0;

    // Block size cached from prepareToPlay. Every engine-side scratch buffer is
    // sized from it (voicecard buffers, FX-output buffers, each FxChain's dry/
    // wet scratch, and the per-processor oversampled scratch -- worst case the
    // Wavefolder's 6x osL_/osR_ at maxBlock*6+8). processBlock clamps the host
    // block against this so an over-prepared host block (a prepare/process
    // contract violation that does occur in the wild: buffer-size transitions,
    // offline/freeze renders) degrades to a truncated render instead of a heap
    // OOB write (FX audit F3/F6). Atomic + relaxed: hosts that overlap
    // prepareToPlay with a running callback would otherwise risk a torn read.
    // In the worst case the clamp briefly uses the previous block size.
    // 0 = not yet prepared (the clamp is bypassed, preserving the old
    // behaviour).
    std::atomic<int> preparedMaxBlock_ { 0 };

    // Filter-OS latency probe + a dirty flag so a live factor change (UI)
    // re-reports the latency from processBlock (prepareToPlay also reports it).
    std::unique_ptr<juce::dsp::Oversampling<float>> osLatencyProbe_;
    std::atomic<bool> latencyDirty_ { false };
    // Filter-OS latency in INPUT (internal) samples, computed on the message
    // thread by rebuildOsLatencyProbe() (the sole reader of osLatencyProbe_) and
    // read by computePluginLatency() on the audio thread. Staging an int avoids
    // the audio thread dereferencing the probe's unique_ptr while the message
    // thread rebuilds it (a data race).
    std::atomic<int> stagedOsLatencyInputSamples_ { 0 };

    // ---- Realtime overrun probe state (see getAudioLoad* in the public section) ----
    std::atomic<double>   audioLoadCurrent_  { 0.0 };   // latest block's render/budget ratio (1.0 = saturated)
    std::atomic<double>   audioLoadPeak_     { 0.0 };   // worst ratio since last reset
    std::atomic<uint64_t> audioOverrunCount_ { 0 };     // blocks whose render time exceeded the budget
    std::atomic<bool>     audioLoadResetReq_ { false }; // message thread requests a peak/overrun reset

    // Master DC blocker (one IIR high-pass ~15 Hz per main-bus channel). The
    // engine's analog filter + VCA are DC-coupled, so any sub-audio/DC offset
    // would otherwise leak as a low-frequency rumble. Applied to the MAIN bus
    // only, after the voicecard sum; the raw aux voicecard buses are untouched.
    // 15 Hz is well below the audible band, so it never colours the sound.
    juce::dsp::IIR::Filter<float> dcBlocker_[2];

    // parameterID -> index into getPatchParamDescriptors() for O(1) lookups.
    std::unordered_map<std::string, int> paramIndex_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParvatiAudioProcessor)
};

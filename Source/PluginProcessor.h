// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// ParvatiAudioProcessor — the plugin's AudioProcessor. Owns the SynthEngine
// (16 AmbikaVoice instances) and an AudioProcessorValueTreeState (APVTS) that
// exposes every Ambika patch/part parameter. Parameter changes are bridged to
// the integer engine by writing the corresponding Patch/Part struct byte into
// every active voice (see ParameterLayout + SynthEngine::applyPatchByte).

#pragma once

#include <juce_audio_devices/juce_audio_devices.h>   // juce::MidiMessageCollector
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>   // juce::dsp::Oversampling (filter-OS latency probe)

#include <atomic>
#include <memory>
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
    ~ParvatiAudioProcessor() override = default;

    //==========================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==========================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    //==========================================================================
    const juce::String getName() const override { return "Parvati"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

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
    // processBlock. Exposed so a future "max quality" mode (e.g. oversampling)
    // can auto-engage during offline bounce, where CPU is unconstrained.
    void setNonRealtime (bool isNonRealtime) noexcept override;
    bool isNonRealtimeRender() const noexcept { return nonRealtime_.load (std::memory_order_relaxed); }

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
    juce::String getUiTheme() const noexcept    { return uiThemeName_; }
    double       getUiZoom() const noexcept     { return uiZoom_; }
    bool         getUiTooltips() const noexcept { return uiTooltips_; }
    bool         getUiSmoothing() const noexcept { return uiSmoothing_; }
    int          getUiOversampling() const noexcept { return uiOversampling_; }
    // Editor chrome language code ("auto" / "en" / "fr"). "auto" defers to the
    // OS locale. Persisted so the chosen language survives host save/restore.
    juce::String getUiLanguage() const noexcept { return uiLanguage_; }
    void setUiTheme (juce::String name) { uiThemeName_ = std::move (name); }
    void setUiZoom (double z)           { uiZoom_ = z; }
    void setUiTooltips (bool b)         { uiTooltips_ = b; }
    void setUiSmoothing (bool b)        { uiSmoothing_ = b; }
    void setUiOversampling (int n)      { uiOversampling_ = n; }
    void setUiLanguage (juce::String code) { uiLanguage_ = std::move (code); }

    // NOTE: the UI font mode selector was removed — the whole UI now uses the
    // system default sans-serif (see ParvatiLookAndFeel::appFont). The field
    // below is kept ONLY so legacy saved states that carry a ui_font_mode
    // property still load (the value is read + re-saved but no longer applied).

    // Enable/disable optional per-sample parameter smoothing (knob / automation
    // zipper-noise reduction). Propagates to all voices + persists the pref.
    void setParameterSmoothing (bool smoothing);

    // Enable optional FILTER oversampling (1 / 2 / 4). 1 = off (default, the
    // audio path is bit-identical). Propagates to every voice + persists the
    // pref + recomputes + reports the plugin latency (the OS filter adds a few
    // samples of group delay on top of the resampler latency).
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
    juce::UndoManager& getUndoManager() noexcept { return undoManager_; }

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
    // via the APVTS listener, but that is ValueTree/timer-routed, so an
    // explicit sync is used after bulk loads and in tests.)
    void syncAllParamsToEngine();

    // ---- Ambika .PRO patch support ----
    // Load a parsed Ambika program (raw Patch[112] + Part[84] bytes) into the
    // APVTS (so the GUI reflects it) and the engine. Returns true on success.
    bool loadProgramFromBytes (const uint8_t* patch112, const uint8_t* part84);
    // Load + parse a .PRO file. Returns true on success.
    bool loadProgramFile (const juce::File& file);
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
    // optional polyphony-mode rewrite). ChainSplit writes additional sibling
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
    void parameterChanged (const juce::String& parameterID, float newValue) override;

    // Apply one parameter's current APVTS value to the engine.
    void applyParameterToEngine (const PatchParamDescriptor& descriptor);

    // Route an arpeggiator parameter to the SynthEngine's arpeggiator.
    void applyArpParameter (const PatchParamDescriptor& descriptor, float rawValue);

    // (Re)builds the oversampling LATENCY probe for the current uiOversampling_.
    // The probe mirrors the per-voice Oversampling config exactly so its
    // getLatencyInSamples() matches what every voice adds; it is never fed audio,
    // so rebuilding it on the message thread (UI factor change) is race-free and
    // decouples latency reporting from the audio-thread voice rebuild.
    void rebuildOsLatencyProbe();

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

    // Route a step-sequencer parameter to the engine Sequencer.
    void applySequencerParameter (const PatchParamDescriptor& descriptor, float rawValue);

    SynthEngine engine_;
    juce::UndoManager undoManager_;   // constructed before apvts (member order)
    juce::AudioProcessorValueTreeState apvts;
    juce::String loadedProgramName_ { "Init" };
    int currentPart_ = 0;   // 0-based part currently shown in the APVTS/editor

    // True while loadPartIntoApvts is pushing engine storage into the APVTS.
    // Suppresses the parameterChanged -> engine re-apply feedback loop: loading
    // is engine->APVTS, so feeding the same values back into the engine is
    // redundant for byte/arp/seq params and HARMFUL for the FX mod matrix, whose
    // applyFxParameter re-reads all three sibling APVTS values (source/dest/
    // amount) and would otherwise read them stale mid-load and clobber the
    // engine fxState it is sourcing from. Message-thread-only (load + listener).
    bool loadingPartIntoApvts_ = false;

    // Hardware-parity MIDI CC/NRPN -> parameter mapping (spec F.4).
    MidiParameterMap midiParamMap_;

    // Thread-safe queue for UI-injected MIDI (keyboard click-play). The audio
    // thread drains it in processBlock.
    juce::MidiMessageCollector midiCollector_;

    // Persisted UI preferences (theme / zoom / tooltips / language).
    juce::String uiThemeName_ { "Carbon" };
    double       uiZoom_ { 1.0 };
    bool         uiTooltips_ { true };
    bool         uiSmoothing_ { false };   // default OFF -> bit-identical audio
    int          uiOversampling_ { 1 };    // 1 / 2 / 4; default 1 -> bit-identical
    int          uiFontMode_ { 0 };        // LEGACY: persisted for old states only (font selector removed; UI is sans-serif)
    juce::String uiLanguage_ { "auto" };   // editor chrome language (auto/en/fr)

    // Offline-render flag (host bounce). Updated by setNonRealtime() and kept
    // warm each block from isNonRealtime() as a host-compat fallback.
    std::atomic<bool> nonRealtime_ { false };

    // Last latency reported to the host, so we only call setLatencySamples when
    // it actually changes (e.g. on a sample-rate switch), not every prepareToPlay.
    int lastReportedLatency_ = -1;

    // Host sample rate cached from prepareToPlay. getSampleRate() can read 0 in
    // standalone / headless contexts (bus not fully enabled), so the audio-thread
    // latency re-report uses this authoritative value instead.
    double hostSampleRate_ = 0.0;

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

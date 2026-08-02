// Copyright (c) 2024 805LABS / Parvati.
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

#include "ParameterLayout.h"
#include "PatchFile.h"
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
    // Where the factory Ambika patches are installed.
    static juce::File getFactoryPatchDir();

    // ---- Ambika .MUL (multi) support ----
    // Load a .MUL: configures all 6 Parts (patches + PartData + MIDI channel +
    // key zone + voice allocation from MultiData.part_mapping_[], and per-part
    // arp/sequencer settings from each PartData). Shows Part 0 in the editor
    // afterwards. Returns true on success.
    bool loadMultiFile (const juce::File& file);
    static juce::File getFactoryMultiDir();
    // The name of the last loaded program (for the GUI title).
    juce::String getLoadedProgramName() const { return loadedProgramName_; }

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

    // Multitimbral: switch the Part being edited. Loads the new Part's stored
    // patch/part/arp/seq into the APVTS (GUI reflects it) + the engine.
    void onPartSelect (int newPart1Based);
    void loadPartIntoApvts (int part);

    // Route a step-sequencer parameter to the engine Sequencer.
    void applySequencerParameter (const PatchParamDescriptor& descriptor, float rawValue);

    SynthEngine engine_;
    juce::UndoManager undoManager_;   // constructed before apvts (member order)
    juce::AudioProcessorValueTreeState apvts;
    juce::String loadedProgramName_ { "Init" };
    int currentPart_ = 0;   // 0-based part currently shown in the APVTS/editor

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

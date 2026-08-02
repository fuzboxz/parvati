// Copyright (c) 2024 805LABS / Parvati.
//
// AmbikaVoice — bridges ONE Ambika integer voice (ambika::dsp::Voice) into a
// juce::SynthesiserVoice, running the digital engine at the fixed internal
// sample rate and resampling to the host rate.
//
// Signal flow (faithful to the hybrid hardware — see docs/DSP_PORT_SPEC.md §G):
//   Voice::ProcessBlock()  ->  40 x uint8 (centred 128)
//                           ->  (u8 - 128) / 128  (float, internal rate)
//                           ->  AnalogFilter (float, internal rate, CV updated
//                                               once per 40-sample block)
//                           ->  x VCA gain (vca()/255)
//                           ->  staging FIFO
//                           ->  juce::LagrangeInterpolator (39216 -> host rate)
//                           ->  host output buffer (mono, added to both chans)
//
// A new voice plays a usable default tone (osc1 = bandlimited saw) on top of
// the faithful init patch (whose oscillators are WAVEFORM_NONE). Full APVTS /
// patch loading arrives in a later task.

#pragma once

#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>   // juce::dsp::Oversampling (filter-only oversampling)

#include "dsp/analog_filter.h"
#include "dsp/constants.h"
#include "dsp/voice.h"

class AmbikaVoice : public juce::SynthesiserVoice
{
public:
    AmbikaVoice() = default;

    // ---- juce::SynthesiserVoice -------------------------------------------------
    bool canPlaySound (juce::SynthesiserSound* sound) override;
    void startNote (int midiNoteNumber, float velocity, juce::SynthesiserSound*,
                    int currentPitchWheelPosition) override;
    void stopNote (float velocity, bool allowTailOff) override;
    void pitchWheelMoved (int) override    {}
    void controllerMoved (int, int) override {}
    void renderNextBlock (juce::AudioBuffer<float>& outputBuffer, int startSample, int numSamples) override;

    // ---- Message-thread voice-activity snapshot (SF-1) ----
    // Lock-free mirror of the base currentlyPlayingNote / isVoiceActive state,
    // staged on the audio thread so the GUI (voice meter + keyboard latch,
    // polled at ~30 Hz on the message thread) never reads the non-atomic
    // SynthesiserVoice::currentlyPlayingNote. The transitions mirror the base
    // semantics exactly: set to (note, true) on startNote, cleared (-1, false)
    // when the voice is fully freed (hard stopNote / release-tail completion);
    // a release-tail stopNote keeps the voice lit until the tail ends.
    int  getDisplayedNote()  const noexcept { return displayedNote_.load(); }
    bool isDisplayedActive() const noexcept { return displayedActive_.load(); }

    // Called once by the SynthEngine at prepareToPlay with the HOST rate.
    void prepare (double hostSampleRate, int blockSize);

    // ---- APVTS -> engine byte bridge (forwarded to the integer Voice) ----
    void setPatchByte (int offset, uint8_t value)
    {
        voice_.set_patch_data (static_cast<uint8_t> (offset), value);
    }
    void setPartByte (int offset, uint8_t value)
    {
        voice_.set_part_data (static_cast<uint8_t> (offset), value);
        // PartData bytes 1/2 are controller-side octave/tuning (the voicecard
        // dsp::Part struct has no such fields). Apply them here at trigger time
        // (firmware Part::TuneNote) — both applyPartByte and pushPartBytesToVoices
        // route through this, so live edits AND .MUL loads update the offset.
        if (offset == 1)      partOctave_  = static_cast<int8_t> (value);
        else if (offset == 2) partTuning_ = static_cast<int8_t> (value);
    }

    // Push host transport tempo to the inner engine (tempo-synced LFOs).
    void setTempo (double bpm) { voice_.setTempo (bpm); }

    // GLOBAL filter-card topology (one Ambika unit = one filter card). Re-prepares
    // the analog filter at the current (oversampled) rate so the newly-active
    // stage is armed.
    void setFilterTopology (ambika::dsp::FilterTopology t)
    {
        filter_.setTopology (t);
        prepareFilterAtOsRate();
    }

    // OPTIONAL filter oversampling. The digital filter MODEL (not the real
    // analog card) aliases; running it at osFactor_*kInternalSampleRate and
    // up/downsampling its I/O reduces that aliasing for higher fidelity. The
    // oscillators stay FIXED-RATE at kInternalSampleRate (39216) for
    // authenticity — only the filter is oversampled. Default osFactor_==1 keeps
    // fillInternalBlock() bit-identical to the un-oversampled path. Changing the
    // factor is deferred to the audio thread (see osFactorDirty_) so the
    // per-voice Oversampling object is never recreated under a concurrent reader.
    void setOversamplingFactor (int factor);

    // Write/read a modulation-source slot (SEQ_1/2 are injected by the engine-
    // side sequencer; LoadSources does not clobber them).
    void setModulationSource (uint8_t idx, uint8_t value)
    {
        voice_.set_modulation_source (idx, value);
    }
    uint8_t getModulationSource (uint8_t idx) const
    {
        return voice_.modulation_source (idx);
    }

    // VCA response curve: false = linearized (gain=vca/255), true = exponential OTA taper.
    void setVcaExponential (bool e) { vcaExponential_ = e; }

    // Optional parameter smoothing (default OFF). When enabled, cutoff /
    // resonance / VCA gain are ramped per-sample (20 ms linear) instead of
    // applied once per 40-sample control block, reducing zipper noise on knob
    // turns / automation. Default-OFF keeps the audio path bit-identical.
    void setSmoothingEnabled (bool e) { smoothingEnabled_ = e; if (! e) smoothingActive_ = false; }

    // Mono/legato retrigger hint: set by the engine before startVoice() so the
    // next Trigger is a legato (no re-attack) retrigger (firmware passes
    // legato = mono_stack.size() > 1). Consumed (reset) by startNote().
    void setLegatoNext (bool l) { legatoNext_ = l; }

    // One-shot per-voice pitch-drift hint (14-bit units, 1/128 semitone each):
// firmware PartData.spread applied as `tuned_note + drift` at Trigger. Consumed
    // (reset to 0) by startNote so a subsequent default trigger has no drift.
    void setSpreadDrift (int drift14) { spreadDrift14_ = drift14; }

    // ---- MPE / per-voice expression (MIDI Polyphonic Expression) ----
    // The engine routes pitch wheel / channel pressure / CC74 to each ACTIVE
    // voice on the matching MIDI channel (SynthEngine::handlePitchWheel /
    // handleChannelPressure / handleController). Under MPE each note lives on a
    // unique channel => per-note expression; under standard single-channel MIDI
    // all notes share one channel => channel-wide expression (the historical
    // intent). This ALSO fixes the previously no-op pitchWheelMoved: the bend
    // now actually bends the oscillator.
    //
    // Pitch bend is applied DIRECTLY to the oscillator pitch (always audible,
    // independent of the mod matrix) AND fed to the mod matrix
    // (MOD_SRC_PITCH_BEND / AFTERTOUCH / EXPRESSION) so user routings
    // (Pitch Bend -> osc, Aftertouch -> cutoff, ...) work per-voice. CC74
    // (MPE "slide") maps to MOD_SRC_EXPRESSION.
    void setMpeBendRangeSemitones (float semis) { mpeBendRangeSemitones_ = semis; applyMpeToVoice(); }
    void setMpePitchBendSemitones (float semis) { mpePitchBendSemitones_ = semis; applyMpeToVoice(); }
    void setMpePressure           (float p01)   { mpePressure_ = p01;            applyMpeToVoice(); }
    void setMpeSlide              (float s01)   { mpeSlide_ = s01;               applyMpeToVoice(); }

    // ---- multitimbral: each voice is owned by one Part ----
    int  getPartIndex() const { return partIndex_; }
    void setPartIndex (int p) { partIndex_ = p; }

    // ---- multi-output: each voice belongs to a FIXED voicecard (0..5). The
    // Ambika hardware exposes 6 individual voicecard outputs; the engine routes
    // each voice's render to its voicecard buffer, which the processor copies to
    // the matching optional aux bus (and sums into the main stereo mix).
    int  getVoiceCard() const noexcept { return voiceCard_; }
    void setVoiceCard (int vc) noexcept { voiceCard_ = vc; }

    // Engine-only: force the one-time init (loads the init patch) + read its
    // 112 patch bytes (used to seed each Part's default patch storage).
    void forceInit() { ensureInitialized(); }
    void copyPatchBytes (uint8_t* dst) { ensureInitialized(); std::memcpy (dst, voice_.mutable_patch_data(), 112); }

private:
    // One-time engine init (loads the faithful init patch). The audible
    // program is then driven entirely by the APVTS parameter bridge (see
    // PluginProcessor), which writes the patch bytes after Init(). Idempotent.
    void ensureInitialized();

    // Renders one 40-sample engine block -> float -> filter -> VCA -> staging.
    void fillInternalBlock();

    // (Re)prepares the analog filter at the rate implied by osFactor_ (the
    // oversampled rate when OS is on, else kInternalSampleRate). Keeps the
    // current topology; AnalogFilter::prepare resets the filter state so a rate
    // / topology change is click-free.
    void prepareFilterAtOsRate();

    // Push the current MPE expression state (bend / pressure / slide) into the
    // integer Voice: the direct oscillator pitch-bend offset + the mod-matrix
    // sources (PITCH_BEND / AFTERTOUCH / EXPRESSION). Called by the setters on
    // the audio thread (the engine's handle* overrides).
    void applyMpeToVoice();

    // Builds/destroys the per-voice Oversampling object for osFactor_ and then
    // calls prepareFilterAtOsRate() (the filter rate depends on the factor).
    // Called only where it cannot race fillInternalBlock(): prepare() (host
    // setup, non-concurrent with processBlock) and the audio-thread service of
    // osFactorDirty_ at the top of fillInternalBlock().
    void recreateOversampling();

    ambika::dsp::Voice         voice_;
    bool                       vcaExponential_ { false };

    // Optional per-sample parameter smoothing (20 ms linear). Disabled by
    // default; when off, fillInternalBlock() uses the exact once-per-block CV
    // path (bit-identical). `smoothingActive_` is audio-thread state that snaps
    // the smoothers to their current targets on the first block after the flag
    // is turned on, so re-enabling never ramps up from a stale/zero value.
    bool                       smoothingEnabled_ { false };
    bool                       smoothingActive_  { false };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedCutoffHz_;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedResonance_;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedVcaGain_;
    bool                       legatoNext_ { false };
    int                        partIndex_ { 0 };
    int                        voiceCard_ { 0 };   // FIXED voicecard (0..5) — set once from the voice index
    int                        partOctave_ { 0 };   // PartData.octave (applied at trigger, firmware TuneNote)
    int                        partTuning_ { 0 };   // PartData.tuning (1/128-semitone units)
    int                        spreadDrift14_ { 0 }; // PartData.spread per-voice drift (1/128-semitone units)

    // ---- MPE / per-voice expression state (audio-thread) ----
    float                      mpeBendRangeSemitones_ { 2.f };  // per-voice bend range (MPE standard default)
    float                      mpePitchBendSemitones_ { 0.f };  // net semitones applied to the osc pitch
    float                      mpePressure_           { 0.f };  // channel pressure, 0..1
    float                      mpeSlide_              { 0.f };  // CC74 (expression/slide), 0..1
    ambika::dsp::AnalogFilter  filter_;

    // Filter-only oversampling state. osFactor_ is the ACTIVE factor (1/2/4);
    // pendingOsFactor_/osFactorDirty_ stage a message-thread change for the
    // audio thread (which owns the Oversampling object). filterOS_ is null when
    // osFactor_==1 (no oversampling -> bit-identical path).
    int osFactor_ { 1 };
    int pendingOsFactor_ { 1 };
    std::atomic<bool> osFactorDirty_ { false };
    std::unique_ptr<juce::dsp::Oversampling<float>> filterOS_;

    juce::LagrangeInterpolator interp_;

    // Internal-rate (39216 Hz) staging of filtered/VCA'd float samples feeding
    // the resampler. `interp_` keeps its own fractional state; we refill this
    // FIFO from engine blocks and erase exactly the samples it consumed.
    std::vector<float> fifo_;

    double hostRate_   = 0.0;   // host (DAW) sample rate
    double speedRatio_ = 1.0;   // input(internal) samples per output(host) sample

    bool initialized_  = false;
    bool isReleasing_  = false;

    // Atomic staging for the message-thread meter/keyboard poll (SF-1): written
    // only on the audio thread, read from the message thread. Relaxed ordering
    // suffices (a single stale frame is cosmetically harmless).
    std::atomic<int>  displayedNote_   { -1 };
    std::atomic<bool> displayedActive_ { false };

    // Resampler chunking: produce at most this many host samples per process()
    // call, and always keep at least kLookahead internal samples ahead of the
    // read position (Lagrange is a short polynomial filter).
    static constexpr int kMaxChunk  = 256;
    static constexpr int kLookahead = 16;
};

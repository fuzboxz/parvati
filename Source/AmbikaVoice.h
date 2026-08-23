// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
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
    // Deletes anything still parked for retirement (the engine destroys its
    // voices with the audio callback stopped, so a plain exchange is safe).
    ~AmbikaVoice() override;

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
    // semantics exactly: set to (note, true) on startNote. Cleared (-1, false)
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
    // Re-prime the envelope phase increments from the CURRENT patch bytes. The
    // increments default to 0 and are (re)seeded by Voice::Init() and by
    // Envelope::Update() each ProcessBlock for ACTIVE voices. But an IDLE voice
    // is gated out of renderNextBlock, so after a bulk patch-byte push (a
    // voice-mode / allocation rebuild, or a .MUL/.parvati load) its increments
    // are stale. The next Trigger(ATTACK) then reads a 0 attack increment =>
    // the voice renders SILENT (VCA never opens). This was the standalone
    // "goes dead after a voice-mode / template switch" glitch. Call right
    // after a bulk push.
    void reprimeEnvelopes()
    {
        const auto& p = voice_.patch();
        for (uint8_t i = 0; i < ambika::dsp::kNumEnvelopes; ++i)
            voice_.mutable_envelope (i)->Update (p.env_lfo[i].attack,
                                                 p.env_lfo[i].decay,
                                                 p.env_lfo[i].sustain,
                                                 p.env_lfo[i].release);
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

    // Per-part tuning table (firmware raga preset or Parvati custom table):
    // 12 offsets in 1/128-semitone units, indexed by the RAW incoming note's
    // class at trigger (startNote). Staged by the engine's audio-thread tuning
    // service (pushTuningToVoices) — never written from the message thread
    // while the voice renders. Default zeros = 12-EDO (bit-identical to the
    // pre-tuning path: the hook adds tuneOffsets_[note % 12] == 0).
    void setTuningOffsets (const int16_t* offsets)
    {
        if (offsets != nullptr)
            std::memcpy (tuneOffsets_, offsets, sizeof (tuneOffsets_));
    }

    // Push host transport tempo to the inner engine (tempo-synced LFOs).
    void setTempo (double bpm) { voice_.setTempo (bpm); }

    // GLOBAL filter-card topology (one Ambika unit = one filter card). Re-prepares
    // the analog filter at the current (oversampled) rate so the newly-active
    // stage is armed.
    void setFilterTopology (ambika::dsp::FilterTopology t)
    {
        // Stage for the audio thread (same pattern as setOversamplingFactor):
        // filter_.setTopology + prepareFilterAtOsRate mutate float filter state
        // that fillInternalBlock() reads every block. An application on the
        // message thread would race the processSample reader. Serviced at the
        // top of fillInternalBlock().
        pendingTopology_.store (t, std::memory_order_relaxed);
        topologyDirty_.store (true, std::memory_order_release);
    }

    // OPTIONAL filter oversampling. The digital filter MODEL (not the real
    // analog card) aliases. Run it at osFactor_*kInternalSampleRate with
    // up/downsampled I/O to reduce that aliasing for higher fidelity. The
    // oscillators stay FIXED-RATE at kInternalSampleRate (39216) for
    // authenticity — only the filter is oversampled. osFactor_==1 keeps
    // fillInternalBlock() bit-identical to the un-oversampled path. The new
    // Oversampling object is PRE-BUILT here on the calling (message) thread
    // and staged. The audio thread installs it with pointer moves only
    // (audit F3 — the old AT-side rebuild was up to 96 make_unique + frees
    // inside one callback).
    void setOversamplingFactor (int factor);

    // Message-thread reaper: delete the Oversampling objects the audio thread
    // parked when installing a staged swap (audit F3). Called from
    // SynthEngine::reapRetiredAudioObjects() at ~60 Hz.
    void reapRetired() noexcept;

    // TEST-ONLY (tests/os_reaper_test.cpp): how many retired-OS objects this
    // voice currently has parked for the reaper (0..kRetiredOsCap). Advisory
    // count of non-null parking slots; relaxed loads.
    int debugRetiredOsCount() const noexcept
    {
        int n = 0;
        for (const auto& r : retiredOs_)
            if (r.load (std::memory_order_relaxed) != nullptr)
                ++n;
        return n;
    }

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

    // ---- Per-internal-block mod-source capture ring (FX mod matrix @ 980 Hz) ----
    // fillInternalBlock() pushes this voice's MOD_SRC_LAST mod sources into a
    // small ring ONCE per 40-sample internal block (right after
    // Voice::ProcessBlock advances them). SynthEngine::renderPartFx reads the
    // first-active voice's ring at internal-block host-sample boundaries so the
    // FX mod matrix evaluates at the ~980 Hz internal control rate instead of
    // host-block rate. PURE OBSERVATION — does not touch out/filter/VCA, so the
    // synth audio path stays byte-identical.
    int modRingCount() const noexcept { return modRingCount_; }
    const uint8_t* modRingEntry (int i) const noexcept
    {
        return modRing_[(size_t) juce::jlimit (0, kModRingCap - 1, i)].data();
    }
    void clearModRing() noexcept { modRingCount_ = 0; }

    // ---- UI live-modulation telemetry readouts (const, audio-thread reads) ----
    // Thin const wrappers over the integer Voice's envelope + EFFECTIVE filter
    // destinations, sampled ONCE per host block by SynthEngine::renderPartFx for
    // the tracked part's representative voice (docs/LIVE_MOD_FEEDBACK_DESIGN.md).
    // PURE OBSERVATION — no state is touched, so the audio path stays
    // bit-identical. Index clamped like the mod-ring accessor above.
    //   * envelopeStage()          — 0..4 (ATTACK/DECAY/SUSTAIN/RELEASE/DEAD).
    //   * envelopePhase()          — 16-bit position within the stage.
    //   * envelopePhaseIncrement() — the live segment increment (0 in SUSTAIN/
    //                                DEAD; the caller derives progress from
    //                                phase/65536 only when it is non-zero).
    //   * envelopeValueByte()      — current 8-bit output (0..254).
    //   * effectiveCutoff()/effectiveResonance()/filterMode() — the
    //     modulation-applied filter bytes the analog filter consumes this
    //     block (NOT the knob bytes; env-2/lfo-2/matrix offsets included).
    //   * effectiveOscParameter(osc) — the modulation-applied OSC parameter
    //     byte (0..127) the oscillator consumes this block (NOT the knob
    //     byte; env/LFO/matrix offsets included) — drives the OSC waveform
    //     preview's live overlay.
    uint8_t  envelopeStage          (int i) const { return voice_.envelope (envIdx (i)).stage(); }
    uint16_t envelopePhase          (int i) const { return voice_.envelope (envIdx (i)).phase(); }
    uint16_t envelopePhaseIncrement (int i) const { return voice_.envelope (envIdx (i)).phase_increment(); }
    uint8_t  envelopeValueByte      (int i) const { return voice_.envelope (envIdx (i)).value_byte(); }
    uint8_t  effectiveCutoff    () const { return voice_.cutoff(); }
    uint8_t  effectiveResonance () const { return voice_.resonance(); }
    // Effective (modulation-applied) OSC parameter byte (0..127) of
    // oscillator 0/1 — the same modulated byte UpdateDestinations feeds the
    // oscillator's set_parameter() this block (env/LFO/matrix amounts folded
    // into the 14-bit accumulator, top 7 bits taken). Drives the OSC waveform
    // preview's live overlay through the telemetry frame.
    uint8_t  effectiveOscParameter (int oscIdx) const
    {
        return voice_.osc_parameter (static_cast<uint8_t> (juce::jlimit (0, 1, oscIdx)));
    }
    uint8_t  filterMode         () const { return voice_.mode(); }

    // VCA response curve: false = linearized (gain=vca/255), true = exponential OTA taper.
    void setVcaExponential (bool e) { vcaExponential_ = e; }

    // Ladder saturation drive (caches into the AnalogFilter; applied on its next
    // control-rate commit on the audio thread — same benign cross-thread float
    // pattern as setVcaExponential). Ladder card only.
    void setFilterDrive (float d) { filter_.setDrive (d); }

    // Optional parameter smoothing (default OFF). When enabled, cutoff /
    // resonance / VCA gain are ramped per-sample (20 ms linear) instead of
    // applied once per 40-sample control block, reducing zipper noise on knob
    // turns / automation. Default-OFF keeps the audio path bit-identical.
    void setSmoothingEnabled (bool e) { smoothingEnabled_ = e; if (! e) smoothingActive_ = false; }

    // Mono/legato retrigger hint: set by the engine before startVoice() so the
    // next Trigger is a legato (no re-attack) retrigger (firmware passes
    // legato = mono_stack.size() > 1). Consumed (reset) by startNote().
    void setLegatoNext (bool l) { legatoNext_ = l; }

    // No-kill re-trigger of an ALREADY-SOUNDING voice (legato overlap OR a
    // mono retrigger of a release tail), 2026-08-22 redesign:
    // through the engine's startVoice (full truthful JUCE bookkeeping: note,
    // channel, noteOnTime, sound, pedals) with the kill guard neutralized.
    // clearCurrentNote() (bookkeeping-only; the DSP voice / FIFO / gain are
    // NOT touched) drops the currentlyPlayingSound pointer that arms
    // startVoice's pre-emptive stopNote(0,false) -> Kill. continuityNext_
    // then makes startNote CONTINUE the live audio. Replaces the old
    // retriggerNote() shortcut, which left currentlyPlayingNote stale
    // (benign for pure legato; wrong once release-tail retriggers used it).
    void armRetriggerContinuation()
    {
        continuityNext_ = true;
        clearCurrentNote();   // public, bookkeeping-only — disarms startVoice's kill
    }

    // One-shot per-voice pitch-drift hint (14-bit units, 1/128 semitone each):
// firmware PartData.spread applied as `tuned_note + drift` at Trigger. Consumed
    // (reset to 0) by startNote so a later default trigger has no drift.
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
    // Stamp this voice as the most-recently-triggered (engine-side counter).
    void setTriggerSeq (uint64_t s) noexcept { triggerSeq_.store (s, std::memory_order_relaxed); }
    uint64_t triggerSeq() const noexcept { return triggerSeq_.load (std::memory_order_relaxed); }

    void setMpePitchBendSemitones (float semis) { mpePitchBendSemitones_ = semis; applyMpeToVoice(); }
    void setMpePressure           (float p01)   { mpePressure_ = p01;            applyMpeToVoice(); }
    void setMpeSlide              (float s01)   { mpeSlide_ = s01;               applyMpeToVoice(); }

    // Test-only readouts (tuning_test): the 14-bit pitch the last startNote
    // computed (baseNote*128 + tuneOffsets_[note % 12] + partTuning_ + drift,
    // before jlimit — clamped exactly like the live path) and the net bend
    // semitones now applied to the oscillator (standing-bend pickup).
    // Atomic staged in startNote / the MPE setters; relaxed reads are fine for
    // tests (the same SF-1 displayedNote_ pattern).
    int   getLastNote14() const noexcept { return lastNote14_.load (std::memory_order_relaxed); }
    float getMpePitchBendSemitones() const noexcept { return mpePitchBendSemitones_; }

    // ---- multitimbral: each voice is owned by one Part ----
    // Relaxed atomics: written on the audio thread (rebuildVoiceAllocation
    // re-tags voices on every allocation rebuild) and read from the message
    // thread by the editor's part-relative voice meter / status count — the
    // same SF-1 discipline as displayedActive_/displayedNote_.
    int  getPartIndex() const { return partIndex_.load (std::memory_order_relaxed); }
    void setPartIndex (int p) { partIndex_.store (p, std::memory_order_relaxed); }

    // ---- multi-output: each voice belongs to a FIXED voicecard (0..5). The
    // Ambika hardware exposes 6 individual voicecard outputs; the engine routes
    // each voice's render to its voicecard buffer, which the processor copies to
    // the matching optional aux bus (and sums into the main stereo mix).
    // Atomic for the same cross-thread reason as partIndex_ (audio-thread
    // re-tagging vs. message-thread export / meter reads).
    int  getVoiceCard() const noexcept { return voiceCard_.load (std::memory_order_relaxed); }
    void setVoiceCard (int vc) noexcept { voiceCard_.store (vc, std::memory_order_relaxed); }

    // Snap @p factor to the supported oversampling factors (1 / 2 / 4 / 8;
    // anything else rounds to the nearest). Shared by the voice staging and
    // the processor's factor setters so the two paths never disagree.
    static int clampOversamplingFactor (int factor) noexcept
    {
        if (factor == 1 || factor == 2 || factor == 4 || factor == 8)
            return factor;
        return (factor <= 1) ? 1 : (factor <= 2 ? 2 : (factor <= 4 ? 4 : 8));
    }

    // Builds the per-voice Oversampling object for @p factor (exponent
    // mapping, min-phase IIR half-band, integer latency, initProcessing at the
    // constant internal block size). Returns null for factor 1 (the
    // bit-identical no-OS path). Used by BOTH the MT staging (pre-build) and
    // recreateOversampling (the AT fallback), so the two paths stay
    // configuration-identical. The processor's latency probe uses it too, so
    // its getLatencyInSamples() matches every voice exactly.
    static std::unique_ptr<juce::dsp::Oversampling<float>> buildOversamplingFor (int factor);

private:
    // Index clamp for the 3-slot envelope wrappers above (mirrors the
    // mod-ring accessor's juce::jlimit discipline — defensive against a
    // future caller passing an out-of-range slot).
    static uint8_t envIdx (int i) noexcept
    {
        return static_cast<uint8_t> (juce::jlimit (0, ambika::dsp::kNumEnvelopes - 1, i));
    }

    // One-time engine init (loads the faithful init patch). The audible
    // program is then driven entirely by the APVTS parameter bridge (see
    // PluginProcessor), which writes the patch bytes after Init(). Idempotent.
    void ensureInitialized();

    // ---- Staged-OS-swap internals (audit F3; mirrors FxChain's staging) ----
    // MT: acquire the staging slot (spin over the 3-way state; take back an
    // unconsumed Staged entry to coalesce rapid changes). Bounded by an
    // in-flight AT install only (microseconds) -- an audio-stopped engine
    // returns immediately via the take-back.
    bool acquireOsStaging() noexcept;
    // AT (or prepare on the MT with the callback stopped): install a staged
    // object if one is published (CAS Staged->Empty; pointer moves only).
    // Returns true when an install happened.
    bool consumeStagedOversampling() noexcept;
    // Park a displaced Oversampling object for the MT reaper.
    void parkRetiredOversampling (juce::dsp::Oversampling<float>* old) noexcept;

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
    // One-shot continuity hint (2026-08-22 mono-retrigger fix): set by
    // retriggerNote when the voice is ALREADY SOUNDING (legato overlap OR
    // mid-release tail). startNote then CONTINUES the live audio — no
    // resampler FIFO clear (that discards ~0.4-1.3 ms of unconsumed internal
    // samples and cold-restarts the Lagrange interpolator: a time-skip
    // click) and no de-click ramp (that would punch a ~1 ms gain hole in
    // audio that is already running). The DSP retrigger semantics are set
    // INDEPENDENTLY by legatoNext_: a release-tail retrigger still does the
    // full firmware Voice::Trigger(legato=0) — envelope ATTACK from the
    // CURRENT value (Envelope::Trigger seeds a_ from value_), gate, osc2
    // reset, LFO retrigger — exactly what the firmware does when the same
    // voice is retriggered in mono mode. Only the plumbing (kill/fifo/ramp)
    // differs from a fresh voice.
    bool                       continuityNext_ { false };
    // std::atomic (non-copyable) is fine: juce::SynthesiserVoice is held by
    // pointer in the engine's Synthesiser voice list and never copied.
    std::atomic<int>          partIndex_ { 0 };
    std::atomic<int>          voiceCard_ { 0 };   // FIXED voicecard (0..5) — re-tagged by rebuildVoiceAllocation
    int                        partOctave_ { 0 };   // PartData.octave (applied at trigger, firmware TuneNote)
    int                        partTuning_ { 0 };   // PartData.tuning (1/128-semitone units)
    int                        spreadDrift14_ { 0 }; // PartData.spread per-voice drift (1/128-semitone units)
    int16_t                    tuneOffsets_[12] {}; // per-class tuning table (engine staging; see setTuningOffsets)

    // ---- MPE / per-voice expression state (audio-thread) ----
    float                      mpeBendRangeSemitones_ { 2.f };  // per-voice bend range (MPE standard default)
    float                      mpePitchBendSemitones_ { 0.f };  // net semitones applied to the osc pitch
    float                      mpePressure_           { 0.f };  // channel pressure, 0..1
    float                      mpeSlide_              { 0.f };  // CC74 (expression/slide), 0..1
    ambika::dsp::AnalogFilter  filter_;

    // Filter-only oversampling state. osFactor_ is the ACTIVE factor (1/2/4/8);
    // pendingOsFactor_/osFactorDirty_ stage a message-thread change for the
    // audio thread (which owns the Oversampling object). filterOS_ is null when
    // osFactor_==1 (no oversampling -> bit-identical path).
    int osFactor_ { 1 };
    std::atomic<int> pendingOsFactor_ { 1 };
    std::atomic<bool> osFactorDirty_ { false };

    // ---- Staged OS swap (MT builds / AT installs; audit F3) ----
    // Same 3-way handoff as FxChain's type staging: pendingOs_ is owned by
    // whoever holds the stage state (Empty / Filling=MT filling / Staged=
    // published, the AT may install at any moment). The MT takes an unconsumed
    // Staged entry back to coalesce rapid changes; the AT installs via
    // CAS Staged->Empty and then owns pendingOs_ exclusively.
    static constexpr int kOsStageEmpty     = 0;
    static constexpr int kOsStageFilling   = 1;
    static constexpr int kOsStageStaged    = 2;
    // Bug hunt 2026-08-18 (TSan, same class as FxChain F-eng-3): the AT's
    // consumeStagedOversampling used to CAS Staged->EMPTY before moving
    // pendingOs_ out. The MT could then acquire and WRITE pendingOs_ while
    // the AT was still between its CAS and its move (a verified data race /
    // UAF window). Consuming marks the AT's exclusive ownership window; the
    // MT's acquireOsStaging spins (neither of its CASes match Consuming) until
    // the final Empty store below the move.
    static constexpr int kOsStageConsuming = 3;
    std::unique_ptr<juce::dsp::Oversampling<float>> pendingOs_;
    std::atomic<int> osStageState_ { kOsStageEmpty };

    // Retirement parking for displaced Oversampling objects: the AT releases
    // with a compare_exchange, the MT reaper claims with an exchange + delete
    // (atomic slots so a park and a reap racing on one entry resolve pointer
    // ownership to exactly one side).
    static constexpr int kRetiredOsCap = 2;
    std::array<std::atomic<juce::dsp::Oversampling<float>*>, (size_t) kRetiredOsCap> retiredOs_ {};
    std::atomic<bool> retiredOsDirty_ { false };

    // Filter-card topology staging (message-thread -> audio-thread). The active
    // topology is applied in fillInternalBlock() so the filter is never
    // re-prepared under a concurrent processSample. Defaults to the topology
    // prepare() initialises (four-pole ladder).
    std::atomic<ambika::dsp::FilterTopology> pendingTopology_ { ambika::dsp::FilterTopology::FOUR_POLE_LADDER };
    std::atomic<bool> topologyDirty_ { false };
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

    // Test-only staged readout of the last computed 14-bit pitch (see
    // getLastNote14). -1 before the first note.
    std::atomic<int>  lastNote14_      { -1 };

    // Monotonic trigger sequence (stamped by SynthEngine at every note-on via
    // setTriggerSeq). Read by renderPartFx's FX mod-source representative-voice
    // tracker to pick the MOST-RECENTLY-TRIGGERED active voice per part (so
    // per-voice sources like VELOCITY/NOTE/per-note MPE follow the latest note).
    // Relaxed atomic: written at note-on (audio or message thread), read on the
    // audio thread. 0 = never triggered (loses to any real seq in the comparison).
    std::atomic<uint64_t> triggerSeq_ { 0 };

    // Resampler chunking: produce at most this many host samples per process()
    // call, and always keep at least kLookahead internal samples ahead of the
    // read position (Lagrange is a short polynomial filter).
    static constexpr int kMaxChunk  = 256;
    static constexpr int kLookahead = 16;

    // Per-internal-block mod-source capture ring (read by renderPartFx to drive
    // the FX mod matrix at ~980 Hz). kModRingCap covers the worst-case
    // internal-blocks-per-host-block across supported hosts: 1024 @ 44.1k ≈
    // 25.3 → 34 with headroom (2026-08-21: was 12, which DROPPED the trailing
    // sub-chunks of larger host blocks — the FX matrix + UI telemetry re-read
    // a stale entry, adding per-block discontinuities to the recorded ENV/
    // LFO traces). ~1 KB per voice.
    static constexpr int kModRingCap = 34;
    std::array<std::array<uint8_t, ambika::dsp::MOD_SRC_LAST>, kModRingCap> modRing_ {};
    int modRingCount_ = 0;

    // De-click: a one-shot per-voice gain ramp (0 -> 1) applied over the first
    // ~1 ms of a FRESH (non-legato) note start, so the oscillator/envelope
    // restart does not produce a click. Runs only while startupRampRemaining_ >
    // 0 (host-rate samples); sustained + legato notes pass through at gain 1.0.
    static constexpr int   kDeClickRamp = 48;                 // ~1 ms @ 48 kHz
    static constexpr float kDeClickInc  = 1.0f / kDeClickRamp;
    float startupGain_          { 1.0f };
    int   startupRampRemaining_ { 0 };

    // VCA gain glide (2026-08-22 release-noise fix, default path): the
    // firmware VCA is ANALOG post-DSP and smooths the block-rate CV steps; a
    // per-block ZOH gain staircase in the port AM-modulates the signal at
    // ~980 Hz (control rate), which reads as aliasing-like noise while the
    // envelope moves — most audible in the decaying release tail. The last
    // APPLIED gain; each block linearly glides from it to the new target
    // across the 40 internal samples (same sanctioned-divergence class as
    // the dsp::Voice mix-gain glide). A static CV (sustain / parked env)
    // has a zero diff, so static output is bit-identical.
    float vcaGlideGain_         { 0.0f };

    // Crush (sample-and-hold / decimator): mirrors the firmware voicecard DAC
    // which only updates its output every `crush()` internal samples (voice.h).
    // crushHeldSample_ holds the last "updated" 8-bit sample; crushSampleCounter_
    // counts internal samples between updates. Persist across blocks so the hold
    // is continuous. crush()==1 => every sample updates (no effect).
    uint8_t crushHeldSample_     { 128 };
    int     crushSampleCounter_  { 0 };
};

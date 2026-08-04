// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// SynthEngine — a juce::Synthesiser owning 16 AmbikaVoice instances divided
// among up to kNumParts (6) Parts (multitimbral, hardware-accurate). Each Part
// has its own Patch + PartData + Arpeggiator + Sequencer + MIDI channel + key
// zone + a subset of the voices. Only the "current" Part is edited via APVTS
// (matching the hardware: one editor, part-select). Direct MIDI is routed by
// channel+keyzone to the matching Part; arpeggiator/sequencer-generated notes
// trigger a voice WITHIN the generating Part.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "AmbikaSound.h"
#include "AmbikaVoice.h"
#include "Arpeggiator.h"
#include "NoteStack.h"
#include "Sequencer.h"
#include "TransportClock.h"
#include "dsp/patch.h"

// Authentic hardware = 6 voicecards => 6 Parts.
static constexpr int kNumParts  = 6;
static constexpr int kNumVoices = 16;   // plugin exposes 16 for polyphony headroom

// Voice capacity mode. Hardware = each of the 6 voicecards contributes ONE
// voice (faithful to the Ambika: 6-note max polyphony total across Parts).
// Extended = each voicecard contributes its full block of Parvati voice slots
// (vc0={0,1,2}..vc5={14,15} => up to 16 voices) for polyphony headroom.
// Default = Hardware (6); the user opts into Extended (16) explicitly.
enum class VoiceMode { Hardware = 0, Extended = 1 };

// One multitimbral Part. The Arpeggiator/Sesequencer objects ARE the per-part
// storage for those settings (edits route to the current Part's objects).
//
// Polyphony: faithful port of ambika::VoiceAllocator (controller/
// voice_allocator.cc) operating over a Part's voiceIndices. PolyphonyMode
// (firmware part.h:58): MONO=0, POLY=1, UNISON_2X=2, CYCLIC=3, CHAIN=4.
// pool_[i]: 0 = inactive; 0x80|note = active holding note. cyclic_: 0xff = LRU
// mode (POLY/UNISON_2X/CHAIN); else a round-robin counter (CYCLIC).
struct PolyAllocator
{
    static constexpr int kMax = 16;
    uint8_t pool_[kMax] {};
    uint8_t lru_[kMax]   {};
    uint8_t size_   = 0;
    uint8_t cyclic_ = 0xff;

    void init (uint8_t size, bool cyclic)
    {
        size_   = size;
        cyclic_ = cyclic ? 0 : 0xff;
        for (uint8_t i = 0; i < kMax; ++i) pool_[i] = 0;  // NOLINT(modernize-loop-convert): faithful port of ambika::VoiceAllocator (controller/voice_allocator.cc)
        for (uint8_t i = 0; i < kMax; ++i) lru_[i] = (i < size_) ? static_cast<uint8_t> (size_ - 1 - i) : 0;
    }
    uint8_t find (uint8_t note) const
    {
        for (uint8_t i = 0; i < size_; ++i)
            if ((pool_[i] & 0x7f) == note) return i;
        return 0xff;
    }
    void touch (uint8_t voice)
    {
        int8_t s = static_cast<int8_t> (size_) - 1;
        int8_t d = s;
        while (s >= 0) { if (lru_[s] != voice) lru_[d--] = lru_[s]; --s; }
        lru_[0] = voice;
    }
    uint8_t noteOn (uint8_t note)
    {
        if (size_ == 0) return 0xff;
        uint8_t voice = 0xff;
        if (cyclic_ == 0xff)
        {
            voice = find (note);
            if (voice == 0xff) for (uint8_t i = 0; i < size_; ++i) if (lru_[i] < size_ && ! (pool_[lru_[i]] & 0x80)) voice = lru_[i];
            if (voice == 0xff) for (uint8_t i = 0; i < size_; ++i) if (lru_[i] < size_) voice = lru_[i];
        }
        else
        {
            cyclic_ = (static_cast<uint8_t> (cyclic_ + 1) >= size_) ? 0 : static_cast<uint8_t> (cyclic_ + 1);
            voice = cyclic_;
        }
        pool_[voice] = static_cast<uint8_t> (0x80 | note);
        touch (voice);
        return voice;
    }
    uint8_t noteOff (uint8_t note)
    {
        uint8_t voice = find (note);
        if (cyclic_ == 0xff) { if (voice != 0xff) { pool_[voice] &= 0x7f; touch (voice); } }
        else                { if (voice != 0xff) pool_[voice] = 0xff; }
        return voice;
    }
};

struct Part
{
    std::array<uint8_t, 112> patchBytes {};   // sizeof(Patch)
    std::array<uint8_t, 84>  partBytes {};    // sizeof(PartData)
    parvati::Arpeggiator arp;
    parvati::Sequencer   seq;
    // These three are written on the message thread (Multi page / .MUL load) and
    // read on the audio thread (findPartForNote, every note) -> atomic to avoid a
    // data race. (polyphonyMode / voiceAllocation below stay plain: they are
    // published to the audio thread via the allocationDirty_ release/acquire.)
    std::atomic<uint8_t> midiChannel  { 0 };   // 0 = Omni (all channels); else 1..16
    std::atomic<uint8_t> keyrangeLow  { 0 };
    std::atomic<uint8_t> keyrangeHigh { 127 };
    // Flagged by setArpMode (message thread) when the arp/seq is turned OFF;
    // serviced at the top of processTransport (audio thread) to kill this Part's
    // arp/seq-generated voices. stopNote() mutates voice state the audio thread
    // renders, so it must not run on the message thread.
    std::atomic<bool> killGeneratedNotes_ { false };
    // Message thread -> audio thread: a live APVTS edit (applyPatchByte/
    // applyPartByte) wrote this Part's patch/part storage; the audio thread pushes
    // the full frame to this Part's voices (pushPartBytesToVoices) so the
    // voice_.patch_/part_ write never races the renderer. Mirrors the firmware
    // "ship a patch frame to the voicecard". <=1 block latency.
    std::atomic<bool> frameDirty_ { false };

    // Arp/seq config staging (message thread writes, audio thread applies via
    // configDirty_ release/acquire). Preserves the live objects' runtime state
    // (pressedKeys_, step counters) — only the CONFIG is re-applied.
    struct PendingConfig {
        uint8_t arpMode = 0, arpDirection = 0, arpOctave = 1, arpPattern = 0, arpResolution = 0;
        uint8_t seqLength[3] = { 0, 0, 0 };
        uint8_t seqData[64]  = {};
    };
    PendingConfig pendingConfig_;
    std::atomic<bool> configDirty_ { false };

    // AT-written snapshot of voiceIndices.size() (rebuildVoiceAllocation) so the
    // message thread (editor status strip) never reads voiceIndices directly.
    std::atomic<int> voiceCount_ { 0 };

    uint8_t voiceAllocation = 0;   // 6-bitmask over firmware voicecards (vc0..5)
    uint8_t polyphonyMode = 1;     // POLY (firmware default); PartData byte 15
    PolyAllocator          polyAlloc;   // POLY/CYCLIC/UNISON_2X/CHAIN allocator
    parvati::NoteStack<12> monoStack;   // MONO note-priority stack
    std::vector<int> voiceIndices;   // indices into the Synthesiser's voice list
};

class SynthEngine : public juce::Synthesiser
{
public:
    SynthEngine();

    // Called from the AudioProcessor's prepareToPlay with the HOST rate.
    void prepare (double sampleRate, int blockSize);

    // ---- APVTS -> voice patch/part byte bridge (CURRENT part only) ----
    // Writes the byte into the current Part's voices AND stores it in that
    // Part's patch/part storage (so part swaps are consistent).
    void applyPatchByte (int offset, uint8_t value);
    void applyPartByte  (int offset, uint8_t value);

    // Global (all voices): host tempo + VCA curve.
    void applyTempo (double bpm);
    void setVcaExponential (bool exponential);

    // Global (all voices): optional per-sample parameter smoothing (knob /
    // automation zipper-noise reduction). Default OFF keeps the audio path
    // bit-identical.
    void setParameterSmoothing (bool smoothing);

    // ---- MPE / per-voice expression ----
    // Per-voice pitch-bend range in semitones, fixed at 2 = the MPE standard
    // per-note bend range (AmbikaVoice::mpeBendRangeSemitones_ default). Not
    // exposed as a parameter; handlePitchWheel uses it to convert the host wheel
    // to semitones before routing per-voice.

    // Global (all voices): optional FILTER oversampling (1/2/4). Default 1 keeps
    // the audio path bit-identical. Each voice defers the rebuild to its audio
    // thread (see AmbikaVoice::setOversamplingFactor).
    void setOversamplingFactor (int factor)
    {
        for (auto* v : voices)
            if (auto* av = dynamic_cast<AmbikaVoice*> (v))
                av->setOversamplingFactor (factor);
    }

    // GLOBAL filter-card topology (one Ambika unit = one filter card).
    void setFilterTopology (ambika::dsp::FilterTopology topology)
    {
        for (auto* v : voices)
            if (auto* av = dynamic_cast<AmbikaVoice*> (v))
                av->setFilterTopology (topology);
    }

    // GLOBAL Ladder saturation drive (one Ambika unit). Ladder card only; cached
    // in each voice's AnalogFilter and applied on its next control-rate commit.
    // Staged + deferred to the audio thread (optionsDirty_) so the per-voice
    // filter_.drive_ write never races the renderer.
    void setFilterDrive (float drive)
    {
        pendingFilterDrive_.store (drive, std::memory_order_relaxed);
        optionsDirty_.store (true, std::memory_order_release);
    }

    // ---- Arpeggiator / Sequencer config (CURRENT part) ----
    // All staged in pendingConfig_ + configDirty_; applied on the audio thread
    // in processTransport before the clock loop (see servicePendingConfig).
    void setArpMode (uint8_t mode);
    void setArpDirection (uint8_t dir)  { parts_[currentPart_].pendingConfig_.arpDirection = dir; parts_[currentPart_].configDirty_.store (true, std::memory_order_release); }
    void setArpOctave (uint8_t oct)     { parts_[currentPart_].pendingConfig_.arpOctave = oct;    parts_[currentPart_].configDirty_.store (true, std::memory_order_release); }
    void setArpPattern (uint8_t pat)    { parts_[currentPart_].pendingConfig_.arpPattern = pat;   parts_[currentPart_].configDirty_.store (true, std::memory_order_release); }
    void setArpResolution (uint8_t res) { parts_[currentPart_].pendingConfig_.arpResolution = res; parts_[currentPart_].configDirty_.store (true, std::memory_order_release); }
    void setSequenceLength (int i, uint8_t len) { if (i>=0&&i<3) { parts_[currentPart_].pendingConfig_.seqLength[i] = len; parts_[currentPart_].configDirty_.store (true, std::memory_order_release); } }
    void setSequenceDataByte (int offset, uint8_t value) { if (offset>=0&&offset<64) { parts_[currentPart_].pendingConfig_.seqData[offset] = value; parts_[currentPart_].configDirty_.store (true, std::memory_order_release); } }

    // ---- multitimbral Part management ----
    static constexpr int getNumParts() { return kNumParts; }
    void setCurrentPart (int part);
    int  getCurrentPart() const { return currentPart_; }
    Part& getPart (int i) { return parts_[i]; }

    // Push a Part's stored patch/part bytes into ALL of that Part's voices
    // (used when a .MUL loads every Part at once; edits normally go through
    // applyPatchByte for the current Part only).
    void pushPartBytesToVoices (int part);

    // Hard-reset EVERY voice: stopNote(.,false) (Kill + clearCurrentNote) +
    // reprimeEnvelopes, so a patch switch starts from silence with no stuck /
    // orphaned voices carrying stale Part/patch state. Called on the message
    // thread before new patch bytes are pushed (the audio thread services the
    // subsequent rebuild). Mirrors firmware Part::AllSoundOff + re-init.
    void resetAllVoices();

    // Mark that the voice allocation / polyphony / patch data changed on the
    // message thread; the audio thread services it (rebuild + push) at the top
    // of the next processTransport so voiceIndices is never mutated under a
    // concurrent reader. (Message-thread callers must NOT rebuild directly.)
    void markAllocationDirty() { allocationDirty_.store (true, std::memory_order_release); }

    // Part routing (MIDI channel + key zone). channel: 0=Omni, else 1..16.
    void setPartChannel  (int part, uint8_t channel) { if (ok (part)) parts_[part].midiChannel.store (channel); }
    void setPartKeyrange (int part, uint8_t lo, uint8_t hi) { if (ok (part)) { parts_[part].keyrangeLow.store (lo); parts_[part].keyrangeHigh.store (hi); } }
    uint8_t getPartChannel (int part) const { return ok (part) ? parts_[part].midiChannel.load() : 0; }

    // GUI-contract aliases (the multitimbral editor calls these). channel: 0=Omni.
    void setPartMidiChannel (int part, int ch)             { setPartChannel (part, static_cast<uint8_t> (ch)); }
    void setPartKeyZone     (int part, int lo, int hi)     { setPartKeyrange (part, static_cast<uint8_t> (lo), static_cast<uint8_t> (hi)); }
    uint8_t getPartKeyrangeLow  (int part) const { return ok (part) ? parts_[part].keyrangeLow.load()  : 0; }
    uint8_t getPartKeyrangeHigh (int part) const { return ok (part) ? parts_[part].keyrangeHigh.load() : 127; }

    // ---- Voice allocation (firmware 6-voicecard bitmask) ----
    // Each firmware voicecard maps to a fixed block of Parvati voices
    // (vc0={0,1,2} vc1={3,4,5} vc2={6,7,8} vc3={9,10,11} vc4={12,13} vc5={14,15}).
    // A Part owns the union of blocks for the voicecard bits it sets; a voicecard
    // already claimed by an earlier Part is not reassigned (first-wins, like
    // firmware Multi::AssignVoices). Default bitmask = 1<<partIndex.
    void setPartVoiceAllocation (int part, uint8_t bitmask);
    uint8_t getPartVoiceAllocation (int part) const { return ok (part) ? parts_[part].voiceAllocation : 0; }

    // Voice capacity mode (Hardware=6 / Extended=16). Sets the mode + flags a
    // deferred voice-allocation rebuild (same release/acquire path as
    // setPartVoiceAllocation). Plain (non-atomic): published to the audio thread
    // via the allocationDirty_ fence, exactly like polyphonyMode/voiceAllocation.
    void setVoiceMode (VoiceMode m) { voiceMode_ = static_cast<int> (m); markAllocationDirty(); }
    VoiceMode getVoiceMode() const noexcept { return static_cast<VoiceMode> (voiceMode_); }
    int       getVoiceModeInt() const noexcept { return voiceMode_; }

    // Advance the transport + per-part arp/sequencer for one audio block.
    void processTransport (juce::MidiBuffer& midi, int numSamples, double bpm, bool isPlaying);

    // Test/internal access.
    AmbikaVoice* getAmbikaVoice (int i)
    {
        if (i >= 0 && i < (int) voices.size())
            return dynamic_cast<AmbikaVoice*> (voices[(size_t) i]);
        return nullptr;
    }

    // ---- Multi-output (Ambika hardware: 6 individual voicecard outputs) ----
    // Each voice renders into its FIXED voicecard buffer (mono). The processor
    // sums all six into the main stereo bus (audible-identical to the pre-
    // multi-out single-buffer mix) and copies each to its optional aux bus.
    // The buffers are sized in prepare() and cleared/filled per sub-block in the
    // renderVoices override.
    const std::array<juce::AudioBuffer<float>, kNumParts>& getVoiceCardBuffers() const noexcept
    {
        return voiceCardBuffers_;
    }
    // Voicecard (0..5) for a given voice index, via the fixed block mapping
    // (vc0={0,1,2} vc1={3,4,5} vc2={6,7,8} vc3={9,10,11} vc4={12,13} vc5={14,15}).
    static int voiceCardForIndex (int voiceIndex);
    // Back-compat: the current Part's arp/seq.
    parvati::Arpeggiator& getArp()       { return parts_[currentPart_].arp; }
    parvati::Sequencer&   getSequencer() { return parts_[currentPart_].seq; }

private:
    std::array<Part, kNumParts> parts_;

    // One mono buffer per voicecard (6 total). Cleared + filled in renderVoices
    // for each sub-block range; the processor mixes them into the output buses.
    std::array<juce::AudioBuffer<float>, kNumParts> voiceCardBuffers_;
    parvati::TransportClock transport_;
    // Reused per-block scratch MidiBuffer (avoids an audio-thread allocation in
    // processTransport's note-routing pass; clear() each block keeps capacity).
    juce::MidiBuffer processedMidi_;
    int  currentPart_ = 0;
    bool wasPlaying_ = false;
    bool partsSeeded_ = false;   // seed Part storage from the init patch once
    std::atomic<bool> allocationDirty_ { false };   // set by message thread; serviced on the audio thread
    std::atomic<bool> resetAllVoicesPending_ { false };   // message thread -> audio thread: kill every voice on a patch switch

    // Global option staging (message thread writes, audio thread applies via
    // optionsDirty_ release/acquire). Replaces the message-thread voice iteration
    // in setVcaExponential/setParameterSmoothing/setFilterDrive that wrote plain
    // per-voice fields the audio thread reads in fillInternalBlock.
    std::atomic<bool> optionsDirty_ { false };
    std::atomic<bool> pendingVcaExp_ { false };
    std::atomic<bool> pendingSmoothing_ { false };
    std::atomic<float> pendingFilterDrive_ { 1.0f };

    // Voice capacity mode: 0 = Hardware (6 voices, 1 per voicecard),
    // 1 = Extended (16 voices, full block per voicecard). Published to the audio
    // thread through allocationDirty_ (same fence as polyphonyMode/voiceAllocation).
    int voiceMode_ { static_cast<int> (VoiceMode::Hardware) };

    float bendRangeSemitones_ = 2.f;   // per-voice pitch-bend range (MPE default)

    static bool ok (int part) { return part >= 0 && part < kNumParts; }

    // First Part whose channel+keyzone accepts (channel,note); -1 if none.
    int findPartForNote (int channel, int note) const;

    // Recompute every Part's voiceIndices from its voiceAllocation bitmask
    // (first-wins across Parts) and re-tag each voice's partIndex.
    void rebuildVoiceAllocation();

    // (Re)initialise a Part's voice allocator for its current polyphony mode
    // and voice set (firmware Part::InitializeAllocators, part.cc:240).
    void initAllocator (Part& p);

    // Voice selection WITHIN a Part (never touches other Parts' voices).
    // incomingChannel tags the triggered voice with its real MIDI channel so the
    // per-channel expression routing (handlePitchWheel / ChannelPressure /
    // Controller) isolates per-note under MPE (Omni Part) and stays channel-wide
    // under standard single-channel MIDI. For non-Omni Parts incomingChannel ==
    // the Part's channel (findPartForNote matched), so behaviour is unchanged;
    // arp/sequencer-generated notes pass the Part's channel.
    void triggerNoteInPart (int part, int note, float velocity, int incomingChannel);
    void releaseNoteInPart (int part, int note, int incomingChannel);

    // Push the current SEQ_1/2 values into each Part's own voices.
    void injectSequencerModulation();

    // juce::Synthesiser routing hooks (noteOn/noteOff are virtual; handleNoteOn
    // is not, in JUCE 9).
    void noteOn  (int midiChannel, int midiNoteNumber, float velocity) override;
    void noteOff (int midiChannel, int midiNoteNumber, float velocity, bool allowTailOff) override;

    // juce::Synthesiser expression routing (MPE / pitch-bend fix). Unified
    // per-channel routing: each handler targets the ACTIVE voices whose MIDI
    // channel matches (isVoiceActive() && isPlayingChannel()). Under MPE a
    // note's channel is unique => per-note; under standard MIDI all notes share
    // one channel => channel-wide. These override the base, whose per-voice
    // callbacks (pitchWheelMoved / channelPressureChanged / controllerMoved) are
    // no-ops in AmbikaVoice.
    void handlePitchWheel      (int midiChannel, int wheelValue) override;
    void handleChannelPressure (int midiChannel, int channelPressureValue) override;
    void handleController      (int midiChannel, int controllerNumber, int controllerValue) override;

    // GLOBAL continuous-controller (mod wheel CC1 / breath CC2 / foot CC4)
    // mod-matrix write — sets the given mod source on EVERY voice (faithful to
    // firmware Part::WriteToAllVoices over all allocated voicecards). See the
    // .cpp for why this also gives new notes current-wheel pickup for free.
    void applyGlobalModSource (int modSrcEnum, uint8_t value0to254);

    // juce::Synthesiser audio hook: route each voice's mono render into its
    // FIXED voicecard buffer instead of the master buffer. The processor fills
    // the master (main + aux) from these after renderNextBlock returns.
    void renderVoices (juce::AudioBuffer<float>& outputAudio, int startSample, int numSamples) override;
};

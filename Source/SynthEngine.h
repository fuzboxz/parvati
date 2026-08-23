// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// SynthEngine — a juce::Synthesiser that owns a fixed pool of AmbikaVoice
// instances (kNumVoices = kNumParts * kMaxVoicesPerPart). The pool is divided
// among up to kNumParts (6) Parts (multitimbral, hardware-accurate). Each Part
// has its own Patch + PartData + Arpeggiator + Sequencer + MIDI channel + key
// zone + a subset of the voices. Only the "current" Part is edited via APVTS.
// The hardware works the same way: one editor, part-select. Direct MIDI is
// routed by channel+keyzone to the matching Part. Arpeggiator/sequencer notes
// trigger a voice WITHIN the Part that generates them.

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
#include "dsp/fx/FxChain.h"   // per-part FX chains (FxChain, FxType)
#include "dsp/patch.h"

#include "MulExport.h"        // mul_export::deriveMasks (derived voicecard masks)

// ===== Per-part data model shard =====
// PolyAllocator / AtomicByteArray / PartFxState / Part and the voice-pool
// constants (kNumParts / kMaxVoicesPerPart / kNumVoices) live in SynthPart.h
// (moved verbatim 2026-08-23); this include re-exports them to every consumer
// of SynthEngine.h.
#include "SynthPart.h"

// ===== Parvati-exclusive per-part FX (Ambika knows nothing about these) =====
// FxType / FxTopology / FxModDestination / kNumFxSlots / kNumFxMatrixSlots /
// kNumFxSlotParams / fxOrderPermutation. These live in a dependency-free
// shard (dsp/fx/FxTypes.h). The FX DSP core can use them without pulling in
// all of SynthEngine.h. The split avoids a circular include (this file ->
// FxChain.h -> FxProcessor.h).
#include "dsp/fx/FxTypes.h"

// ===== UI live-modulation telemetry (docs/LIVE_MOD_FEEDBACK_DESIGN.md) =====
// The shared snapshot contract is a dependency-light shard under ui/ (only
// <cstddef>/<cstdint>). Thus this engine header and every UI component agree
// on ONE frame layout. No JUCE module header enters the DSP shard.
// The engine is the seqlock WRITER (audio thread, renderPartFx); the editor's
// LiveFeedbackHub is the bounded-retry reader (message thread).
#include "ui/ModTelemetryTypes.h"
static_assert (parvati::ModTelemetrySnapshot::kNumSources >= ambika::dsp::MOD_SRC_LAST,
               "the telemetry frame must hold every MOD_SRC_* source without "
               "misindexing sources[]/history[]");
// NOTE (contract deviation, reported to the parent): the frozen shared header
// pins kNumSources = 32. The enum's MOD_SRC_LAST sentinel is 31 (there are
// 31 named sources), so a == assert can never hold. The >= form above is the
// true invariant: every real source index (0..MOD_SRC_LAST-1) fits. The one
// spare slot (index 31) stays zero. The UI only enumerates the
// ModSourceCatalog's real enum values, so nothing reads it.


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
    // exposed as a parameter. handlePitchWheel uses it to convert the host
    // wheel to semitones before per-voice routing.

    // Global (all voices): optional FILTER oversampling (1/2/4/8). Factor 1
    // keeps the audio path bit-identical. Each voice PRE-BUILDS the
    // replacement Oversampling on this (message) thread and stages it. The
    // audio thread installs it with pointer moves only (audit F3 — see
    // AmbikaVoice::setOversamplingFactor / consumeStagedOversampling).
    void setOversamplingFactor (int factor)
    {
        for (auto* av : voicePool_)
            av->setOversamplingFactor (factor);
    }

    // GLOBAL filter-card topology (one Ambika unit = one filter card).
    void setFilterTopology (ambika::dsp::FilterTopology topology)
    {
        for (auto* av : voicePool_)
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
    void setArpDirection (uint8_t dir)  { parts_[(size_t) currentPart_].writePendingConfig ([dir] (auto& c)  { c.arpDirection = dir;  }); parts_[(size_t) currentPart_].configDirty_.store (true, std::memory_order_release); }
    void setArpOctave (uint8_t oct)     { parts_[(size_t) currentPart_].writePendingConfig ([oct] (auto& c) { c.arpOctave = oct;     }); parts_[(size_t) currentPart_].configDirty_.store (true, std::memory_order_release); }
    void setArpPattern (uint8_t pat)    { parts_[(size_t) currentPart_].writePendingConfig ([pat] (auto& c) { c.arpPattern = pat;    }); parts_[(size_t) currentPart_].configDirty_.store (true, std::memory_order_release); }
    void setArpResolution (uint8_t res) { parts_[(size_t) currentPart_].writePendingConfig ([res] (auto& c) { c.arpResolution = res; }); parts_[(size_t) currentPart_].configDirty_.store (true, std::memory_order_release); }
    void setSequenceLength (int i, uint8_t len) { if (i>=0&&i<3)  { parts_[(size_t) currentPart_].writePendingConfig ([i,len] (auto& c) { c.seqLength[(size_t) i] = len;  }); parts_[(size_t) currentPart_].configDirty_.store (true, std::memory_order_release); } }
    void setSequenceDataByte (int offset, uint8_t value) { if (offset>=0&&offset<64) { parts_[(size_t) currentPart_].writePendingConfig ([offset,value] (auto& c) { c.seqData[(size_t) offset] = value; }); parts_[(size_t) currentPart_].configDirty_.store (true, std::memory_order_release); } }

    // ---- multitimbral Part management ----
    static constexpr int getNumParts() { return kNumParts; }
    void setCurrentPart (int part);
    int  getCurrentPart() const { return currentPart_; }
    Part& getPart (int i) { return parts_[(size_t) i]; }

    // Full 6-Part controller-state capture/restore for host plugin-state
    // persistence (getStateInformation / setStateInformation). Captures every
    // Part's patch/part bytes, arp/seq config (pendingConfig_), MIDI routing,
    // voice allocation, polyphony (via partBytes[15]) and the current part.
    // Thus a DAW project reload preserves the full multitimbral setup, not
    // just the current Part. restoreState returns false for an
    // absent/short/foreign blob. The caller can then fall back to the legacy
    // current-part APVTS restore (backward compatible with pre-persistence
    // states). Byte-oriented payload (endian-independent).
    void captureState (juce::MemoryBlock& dest) const;
    bool restoreState (const void* data, size_t size);

    // Push a Part's stored patch/part bytes into ALL of that Part's voices
    // (used when a .MUL loads every Part at once; edits normally go through
    // applyPatchByte for the current Part only).
    void pushPartBytesToVoices (int part);

    // Stage a Part's arp/sequencer config from its 84-byte PartData (message
    // thread). Routes the arp/seq bytes (PartData 7..14 + 16..79) into
    // pendingConfig_ + flags configDirty_ instead of writing the live
    // Arpeggiator/Sequencer objects directly. The audio thread is the sole
    // writer of those (it services configDirty_ in processTransport). The
    // staging removes the data race between a file load and the audio-thread
    // clock loop. It also keeps pendingConfig_ (the serialize source) in sync
    // with the loaded values. Used by the .MUL and .parvati multi-load paths.
    void stageArpSeqFromPartBytes (int part);   // reads parts_[part].partBytes (atomic)

    // Hard-reset EVERY voice: stopNote(.,false) (Kill + clearCurrentNote) +
    // reprimeEnvelopes. Thus a patch switch starts from silence, with no stuck
    // or orphaned voices that carry stale Part/patch state. Called on the
    // message thread before new patch bytes are pushed (the audio thread
    // services the next rebuild). Mirrors firmware Part::AllSoundOff +
    // re-init.
    void resetAllVoices();

    // Mark that the voice allocation / polyphony / patch data changed on the
    // message thread. The audio thread services it (rebuild + push) at the top
    // of the next processTransport, so voiceIndices is never mutated under a
    // concurrent reader. (Message-thread callers must NOT rebuild directly.)
    void markAllocationDirty() { allocationDirty_.store (true, std::memory_order_release); }

    // ---- UI-mirror invalidation (message-thread mutators) ----
    // Monotonic version bumped by every message-thread mutator of state the
    // Patch page mirrors (per-part polyphony/tuning bytes, voice slots,
    // channel, key zone, names). The editor's poll timer
    // compares it to decide whether a VISIBLE Patch page must re-read the
    // engine. Engine writes that arrive out-of-band (host automation of
    // part_polyphony / part_raga, MIDI NRPN, host undo, state restores) have
    // no editor hook of their own. Without this version the page could keep
    // stale rows on display until the next reveal/load. NOT bumped by
    // audio-thread paths (the AT never mutates those sources). A missed bump
    // would leave a stale row until the next reveal. A spurious bump only
    // costs one cheap idempotent refresh(). Atomic: setStateInformation can
    // arrive off the message thread on some hosts.
    uint32_t getDisplayVersion() const noexcept
    {
        return displayVersion_.load (std::memory_order_relaxed);
    }

    // Part routing (MIDI channel + key zone). channel: 0=Omni, else 1..16.
    void setPartChannel  (int part, uint8_t channel) { if (ok (part)) { parts_[(size_t) part].midiChannel.store (channel); bumpDisplayVersion(); } }
    void setPartKeyrange (int part, uint8_t lo, uint8_t hi) { if (ok (part)) { parts_[(size_t) part].keyrangeLow.store (lo); parts_[(size_t) part].keyrangeHigh.store (hi); bumpDisplayVersion(); } }
    uint8_t getPartChannel (int part) const { return ok (part) ? parts_[(size_t) part].midiChannel.load() : 0; }

    // GUI-contract aliases (the multitimbral editor calls these). channel: 0=Omni.
    void setPartMidiChannel (int part, int ch)             { setPartChannel (part, static_cast<uint8_t> (ch)); }
    void setPartKeyZone     (int part, int lo, int hi)     { setPartKeyrange (part, static_cast<uint8_t> (lo), static_cast<uint8_t> (hi)); }
    uint8_t getPartKeyrangeLow  (int part) const { return ok (part) ? parts_[(size_t) part].keyrangeLow.load()  : 0; }
    uint8_t getPartKeyrangeHigh (int part) const { return ok (part) ? parts_[(size_t) part].keyrangeHigh.load() : 127; }

    // PartData byte 15 = polyphony mode (MONO=0, POLY=1, UNISON_2X=2, CYCLIC=3,
    // CHAIN=4). Additive const read so callers (e.g. arrangement inference) can
    // stay const-correct. Default POLY (1) for an out-of-range part.
    uint8_t getPartPolyphony (int part) const { return ok (part) ? parts_[(size_t) part].partBytes[15] : 1; }

    // PartData byte 3 = per-voice detune spread (firmware PartData.spread,
    // uint8): applied per voice as `voiceIndex * spread` in 1/128-semitone
    // units (SynthEngine.cpp / part.cc). Additive const read so callers (e.g.
    // arrangement inference) can stay const-correct. Default 0 for an
    // out-of-range part.
    uint8_t getPartSpread (int part) const { return ok (part) ? parts_[(size_t) part].partBytes[3] : 0; }

    // ---- Voicecard masks (DERIVED from the voice slots) ----
    // The firmware 6-voicecard bitmask is no longer user state: voiceSlots is
    // the single source of truth. rebuildVoiceAllocation derives each Part's
    // contiguous proportional card share via mul_export::deriveMasks (same
    // shape as the .MUL Proportional strategy) and publishes it into
    // Part::voiceAllocation. setPartVoiceAllocation is kept as the LEGACY
    // LOAD PATH: .MUL files, host-state v1..v5 and older .parvati files carry
    // bitmasks. The setter materializes the equivalent slot count (slots =
    // popcount(mask), 0 -> disabled) and marks the allocation dirty.
    void setPartVoiceAllocation (int part, uint8_t bitmask);
    // The DERIVED mask for @p part, computed FRESH from the slot counts
    // (same pure rule the audio-thread rebuild tags voices with). Thus readers
    // (.MUL export, Patch page) never see a stale-by-one-block value after a
    // slots edit. Part::voiceAllocation keeps the AT-published copy solely as
    // the legacy seed written into host-state blobs.
    uint8_t getPartVoiceAllocation (int part) const
    {
        if (! ok (part))
            return 0;
        std::array<int, kNumParts> want {};
        for (int q = 0; q < kNumParts; ++q)
            want[(size_t) q] = static_cast<int> (parts_[(size_t) q].voiceSlots.load (std::memory_order_relaxed));
        return parvati::mul_export::deriveMasks (want)[(size_t) part];
    }

    // ---- Per-part voice slots (Parvati extension) ----
    // slots: 1..kMaxVoicesPerPart = the Part's voice count drawn from the
    // engine pool (the single source of truth; the card mask is derived from
    // it). The pool (kNumVoices = kNumParts * kMaxVoicesPerPart) always
    // satisfies every Part simultaneously. Changing slots re-partitions the
    // pool on the audio thread (deferred via markAllocationDirty, the same
    // path as the legacy-bitmask/polyphony edits). A Part is enabled iff its
    // slot count >= 1. The PUBLIC setter clamps 0 to 1 (disabling is the
    // legacy loaders' job via setPartVoiceAllocation).
    void setPartVoiceSlots (int part, int slots);
    int  getPartVoiceSlots (int part) const { return ok (part) ? static_cast<int> (parts_[(size_t) part].voiceSlots.load (std::memory_order_relaxed)) : 0; }

    // ---- Per-part microtonal tuning (firmware raga presets) ----
    // Preset selection is NOT a setter here: it is PartData byte 4, edited via
    // applyPartByte / the part_raga APVTS param (rides frameDirty_).
    // Mode encoding: 0 = 12-EDO, 1..32 = raga preset (== byte 4). (The former
    // custom-table mode 33 is gone — custom scales were removed 2026-08-19.)
    // The D4 resolution rule collapses to the byte itself.
    // The resolved table (12 offsets, 1/128-semitone units; 32767 =
    // muted class in raga presets) is resolved into @p out. Mode 0 -> zeros
    // (12-EDO). Reads atomics only; callable from either thread without a
    // dirty flag.
    // Firmware AcceptNote (part.cc:649-660): false when the resolved table
    // mutes the note's class (sentinel). Used to refuse such notes in noteOn /
    // triggerNoteInPart instead of voicing them as garbage pitch.
    int  resolvedTuningMode (int part) const;
    void resolveTuningOffsets (int part, int16_t out[12]) const;
    bool isNoteAcceptedByPartTuning (int part, int rawNote) const;

    // ---- Part names / aliases (Parvati extension; message-thread only) ----
    // 16-char limit keeps the Multi page rows + .parvati lines tidy. Control
    // characters (newlines) are stripped: the .parvati multi format is
    // LINE-based. A newline inside a name would corrupt the document on save
    // (and the hardware name chunk only wants printable text anyway).
    static juce::String sanitizePartName (const juce::String& n)
    {
        juce::String out;
        for (int i = 0; i < n.length() && out.length() < 16; ++i)
        {
            const auto c = n[i];
            if (c >= 0x20)
                out += c;
        }
        return out;
    }
    void setPartName (int part, const juce::String& n) { if (ok (part)) { parts_[(size_t) part].name = sanitizePartName (n); bumpDisplayVersion(); } }
    juce::String getPartName (int part) const { return ok (part) ? parts_[(size_t) part].name : juce::String(); }
    // Display helper: the user name if set, else "Part N".
    juce::String getPartDisplayName (int part) const
    {
        if (! ok (part)) return {};
        const auto& n = parts_[(size_t) part].name;
        return n.isNotEmpty() ? n : "Part " + juce::String (part + 1);
    }

    // Advance the transport + per-part arp/sequencer for one audio block.
    void processTransport (juce::MidiBuffer& midi, int numSamples, double bpm, bool isPlaying);

    // Test/internal access.
    AmbikaVoice* getAmbikaVoice (int i)
    {
        return (i >= 0 && i < kNumVoices) ? voicePool_[(size_t) i] : nullptr;
    }

    // Test-only: the number of times FxChain::process() was called for @p part
    // since the last reset (proves renderPartFx sub-chunks at ~980 Hz).
    // (Always compiled: the instrumentation is runtime-gated by debugEffParamTracking_
    // and the counters are trivial, so there is no release-build overhead. The
    // always-available form lets the FX diagnostic tests build in every config.)
    int debugFxProcessCallCount (int part) const
    {
        return fxChains_[(size_t) part].getProcessCallCountForTest();
    }
    void debugResetFxProcessCallCount (int part)
    {
        fxChains_[(size_t) part].resetProcessCallCountForTest();
    }
    // Test-only: the capture-ring count from the last renderPartFx for @p part.
    int debugLastFxRingCount (int part) const { return debugLastFxRingCount_[(size_t) part]; }

    // Test-only: the live value the chain for @p part consumes for @p slot / @p
    // field, where field 0 = dry/wet and 1..5 = slot param 0..4. The FX param-
    // coverage test drives every FxModDestination through the full engine path
    // and reads this value. The read proves the modulation reached the DSP at
    // full depth (engine -> renderPartFx -> setSlotDryWet/setSlotParam ->
    // params_/dryWet_).
    float debugGetChainValue (int part, int slot, int field) const noexcept
    {
        if (field == 0)
            return fxChains_[(size_t) part].debugGetDryWet (slot);
        return fxChains_[(size_t) part].debugGetParam (slot, field - 1);
    }
    // Test-only: the INSTALLED slot type on @p part's chain (the AT-owned
    // slotType_ cache — a staged swap is reflected only after the audio
    // thread's servicePendingTypeSwaps consumed it, i.e. after a
    // processBlock). Proves a .parvati multi load staged its FX slot TYPES
    // into the DSP chains, not just the fxState atomics. (The atomic-only load
    // left the chains on their previous processors.)
    uint8_t fxChainSlotTypeForTest (int part, int slot) const
    {
        return (part >= 0 && part < kNumParts)
            ? fxChains_[(size_t) part].getInstalledSlotTypeForTest (slot) : 0;
    }
    // Test-only: is @p part's FX-chain SILENCE GATE armed (FxChain.h, the
    // 2026-08-23 idle-CPU gate)? Lets the engine-level silence test observe
    // the gate through the FULL path (processBlock -> renderPartFx ~980 Hz
    // sub-chunks -> chain.process) without exposing the chain object.
    bool fxChainSilenceGateArmedForTest (int part) const
    {
        return (part >= 0 && part < kNumParts)
            && fxChains_[(size_t) part].silenceGateArmedForTest();
    }
    // Test-only: the engine-side state of the global OPTION params staged by
    // setVcaExponential / setFilterDrive (the option atomics the audio thread
    // services; they hold the last-written value). Used by the host-state test
    // to prove a state restore re-APPLIED the options to the engine — the
    // APVTS restore alone left the engine on its defaults while the UI combos
    // showed the saved values.
    bool  vcaExponentialForTest() const noexcept { return pendingVcaExp_.load (std::memory_order_relaxed); }
    float filterDriveForTest() const noexcept { return pendingFilterDrive_.load (std::memory_order_relaxed); }

    // Test-only: begin tracking the effective param (slot 0 param 0) min/max
    // swing during renderPartFx. Call before rendering; read min/max after.
    void debugResetEffParamTracking (int part)
    {
        debugEffParamMin_[(size_t) part] = 1.0f;
        debugEffParamMax_[(size_t) part] = 0.0f;
        debugEffParamTracking_ = true;
    }
    float debugEffParamMin (int part) const { return debugEffParamMin_[(size_t) part]; }
    float debugEffParamMax (int part) const { return debugEffParamMax_[(size_t) part]; }
    void debugStopEffParamTracking() { debugEffParamTracking_ = false; }

    // Test-only: the FX representative-voice tracker state for @p part.
    // trackedVoice = the index of the sticky most-recently-triggered active
    // voice (-1 = none), fadePhase = the per-voice-change source crossfade
    // (0..1; 1 = settled). Lets a test prove the tracker switches to the newest
    // voice and arms the de-click crossfade on a voice change.
    int debugFxTrackedVoice (int part) const { return fxTrackedVoice_[(size_t) part]; }
    float debugFxFadePhase (int part) const { return fxFadePhase_[(size_t) part]; }

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
    // Voicecard (0..5) for a given voice index. Pool voices are pre-tagged
    // round-robin (i % 6) at construction; rebuildVoiceAllocation re-tags every
    // ALLOCATED voice onto its OWN Part's cards (round-robin across the cards
    // the Part's bitmask claims). Thus a Part's audio reaches its individual
    // voicecard outputs no matter how many pool slots it owns. This function
    // is only the pre-rebuild default.
    static int voiceCardForIndex (int voiceIndex);
    // Back-compat: the current Part's arp/seq accessors were removed: they
    // returned references to AUDIO-THREAD-owned objects with zero call sites
    // (verified: Source/ tests/ tools/), so no editor code can call them. The
    // arp/seq state is MT-readable via pendingConfig_
    // (readPendingConfig) instead.

    // ---- Per-part FX (Parvati-exclusive; post-render, host-rate stereo) ----
    // Render every Part's FX chain into its stereo FX-output buffer (called from
    // PluginProcessor::processBlock AFTER renderNextBlock, BEFORE the main-bus
    // sum). For each Part: (1) services fxDirty_ single-threaded (pushes the
    // staged FX params + mod-matrix into the chain); (2) builds a per-part mono
    // sum of its voicecard buffers; (3) samples the first active voice's mod
    // sources; (4) evaluates the 16-slot FX mod matrix at block rate; (5)
    // duplicates mono to L+R; (6) runs the chain. With all fx*_enabled=0 the
    // chain is a dry copy (audibly-identical to the pre-FX path).
    void renderPartFx (int numSamples);
    // The per-part stereo FX-output buffers (2 channels each), sourced by the
    // processor for the main-bus sum. Sized in prepare().
    const std::array<juce::AudioBuffer<float>, kNumParts>& getFxOutputBuffers() const noexcept
    {
        return fxOutputBuffers_;
    }

    // MT setters (message thread; applyFxParameter). Each writes the CURRENT
    // Part's fxState atomics (relaxed: the fxDirty_ release-store publishes the
    // frame) and sets fxDirty_. The audio thread services fxDirty_ in
    // renderPartFx (single-threaded) and pushes the values into fxChains_[p].
    // Mirrors the frameDirty_ / optionsDirty_ staging pattern. slot 0..2, idx 0..3.
    void setFxSlotType    (int slot, uint8_t v);
    void setFxSlotEnabled (int slot, uint8_t v);
    void setFxSlotDryWet  (int slot, uint8_t v);
    void setFxSlotParam   (int slot, int idx, uint8_t v);
    void setFxTopology    (uint8_t v);
    void setFxOrder       (uint8_t v);
    // Master section (v3): global wet/dry + 3-band master EQ.
    void setFxMix         (uint8_t v);
    void setFxEqLow       (uint8_t v);
    void setFxEqMid       (uint8_t v);
    void setFxEqHigh      (uint8_t v);
    // Writes all three mod-matrix fields of one slot atomically (under the same
    // fxDirty_ publish) to avoid a torn matrix slot when only one of the three
    // APVTS params changes. src: MOD_SRC_* index; dest: FxModDestination;
    // amount: -63..+63.
    void setFxModSlot     (int slot, uint8_t src, uint8_t dest, int8_t amount);

    // Reset a Part's entire FX state to the clean defaults (all slots None /
    // bypassed / dry, Series topology, order 0, master mix fully wet, EQ flat,
    // cleared mod matrix). Used when loading a legacy Ambika patch (.PRO/.MUL)
    // that carries no FX information. The FX section is then a clean slate, not
    // the previously-loaded patch's FX. Publishes via fxDirty_.
    void resetPartFx (int part);

    // ---- Tail-length cache (AudioProcessor::getTailLengthSeconds) ----
    // Max tail estimate over every part's ENABLED FX slots (tailSecondsForFx),
    // clamped to [kTailFloorSeconds, kTailCapSeconds]. Maintained by
    // recomputeTailCache() — pure math over the fxState atomics (no audio, no
    // allocation). Called from the audio thread whenever FX state is serviced
    // (fxDirty_) or the transport tempo moves materially (tempo-synced delay
    // tails). Atomic so the host can query it from any thread.
    double getTailLengthSeconds() const noexcept
    {
        return tailSecondsCache_.load (std::memory_order_relaxed);
    }
    // Recompute tailSecondsCache_ from the CURRENT fxState atomics + the cached
    // transport BPM. Safe on the audio thread (relaxed loads + one store).
    void recomputeTailCache() noexcept;
    // Loader-side per-part FX slot-TYPE writer: the same contract as
    // setFxSlotType but for an EXPLICIT part (the .parvati multi loader
    // restores all 6 parts' FX, not just the current one). Stores the fxState
    // atomic AND stages the replacement processor on the message thread
    // (audit F1: the audio thread installs it with pointer moves only) and
    // publishes the frame via fxDirty_. Writing ONLY the atomic (the old
    // loader behaviour) left the chain on its previous processors — the AT's
    // fxDirty_ service deliberately does NOT install slot types — so a loaded
    // multi's FX were silently absent (fresh engine: all-None chains) or kept
    // the PREVIOUS effect active. A None type stages an empty slot (the
    // install clears the processor), same as setSlotType.
    void stagePartFxSlotType (int part, int slot, int type);

    // ---- UI live-modulation telemetry (docs/LIVE_MOD_FEEDBACK_DESIGN.md) ----
    // The engine side of the Pigments-style live feedback system: the audio
    // thread maintains ONE seqlock-guarded frame (this part's mod-source
    // history + envelope/filter observables of its representative voice) and
    // the editor polls it at the user's refresh rate. The frame is PURE
    // OBSERVATION — renderPartFx samples state it already computed for the FX
    // mod matrix, so the audio path stays bit-identical.
    //
    // Reader (message thread — the editor's LiveFeedbackHub): copies out ONE
    // consistent frame, linearizing the internal history ring OLDEST-FIRST.
    // Returns false (and leaves @p out untouched) in these cases: no part is
    // tracked; the retries were exhausted (a torn read: the writer is
    // mid-update — the caller simply polls again); the frame's epoch is stale
    // (a resetUiTelemetry landed and the audio thread has not serviced the
    // clear yet — the UI must hide its live overlays for that window); or the
    // serviced part no longer matches the tracked part (a part switch).
    bool readUiTelemetry (parvati::ModTelemetrySnapshot& out) const;

    // Reset the telemetry (message thread): invalidates the current frame via
    // an epoch bump (visible to the reader IMMEDIATELY, before the audio
    // thread services the clear) and requests the audio thread to wipe the
    // history/sources. Called on patch load / patch switch / init. Thus the pill
    // histories never carry the previous patch's motion into the new patch.
    void resetUiTelemetry();

    // The authoritative validity epoch (starts 0; bumped by every
    // resetUiTelemetry). readUiTelemetry fails whenever the frame carries an
    // older value than this.
    uint32_t uiTelemetryEpoch() const noexcept
    {
        return uiTelemetryEpoch_.load (std::memory_order_relaxed);
    }

    // Select which multitimbral part the telemetry tracks (message thread;
    // clamped to 0..kNumParts-1). The audio thread observes the change at the
    // top of the next renderPartFx and clears the frame for the new part —
    // the reader reports invalid until that service lands. NOTE: the audio
    // thread NEVER dereferences the plain currentPart_ int; this atomic is the
    // only cross-thread part selector.
    void setUiTelemetryPart (int part);

    // Message-thread reaper for the staged audio-object swaps: frees the FX
    // processors displaced by a type change (FxChain retirement parking) and
    // the per-voice Oversampling objects displaced by a filter-oversampling
    // change. Called at ~60 Hz from the processor's DeferredParamTimer so the
    // audio thread's type/OS swaps are pointer moves only (audits F1/F3).
    void reapRetiredAudioObjects();

private:
    std::array<Part, kNumParts> parts_;

    // One mono buffer per voicecard (6 total). Cleared + filled in renderVoices
    // for each sub-block range; the processor mixes them into the output buses.
    std::array<juce::AudioBuffer<float>, kNumParts> voiceCardBuffers_;
    // The voicecard bitmask each Part actually OWNS after exclusive-claim
    // resolution (rebuildVoiceAllocation's per-part `partCards` locals,
    // persisted). renderPartFx sums exactly these card buffers into a Part's
    // FX input. voiceIndices holds POOL indices (0..95, SynthEngine.h:271),
    // which are NOT card indices once slots != card count. Indexing
    // voiceCardBuffers_ by them cross-bleeds other Parts' cards into the FX
    // input (or leaves it silent for pool slices >= 6). AT-only: written in
    // rebuildVoiceAllocation (allocationDirty_ service / ctor) and read in
    // renderPartFx — both on the audio thread, so no atomics are needed.
    uint8_t partCardMask_[kNumParts] {};
    // One stereo (2-channel) FX-output buffer per Part, written by renderPartFx
    // and sourced by the processor for the main-bus sum. Sized in prepare().
    std::array<juce::AudioBuffer<float>, kNumParts> fxOutputBuffers_;
    // One FX chain per Part (host-rate stereo). Serviced single-threaded on the
    // audio thread in renderPartFx when fxDirty_ is set.
    std::array<FxChain, kNumParts> fxChains_;
    // Per-part mod-source snapshot (0..255) used by the FX mod matrix. Updated
    // each block from the Part's first active voice (AmbikaVoice::
    // getModulationSource); reused while no voice is active so tails still
    // modulate. AT-only (written + read in renderPartFx).
    std::array<std::array<uint8_t, ambika::dsp::MOD_SRC_LAST>, kNumParts> lastModSources_ {};

    // ---- FX representative-voice tracker (per part) ----
    // The FX stage is per-part but modulation sources are per-voice. Thus
    // renderPartFx samples ONE voice per part: the MOST-RECENTLY-TRIGGERED
    // active voice (via the per-voice triggerSeq_). Per-voice sources
    // (VELOCITY / NOTE / per-note MPE) follow the latest note. On any voice
    // IDENTITY change a short crossfade bridges the old voice's last effective
    // source values (lastModSources_) to the new voice's live values, so those
    // per-voice sources glide instead of clicking. Global/part-global sources
    // are identical across voices so the crossfade is a no-op there. AT-only.
    std::array<int, kNumParts> fxTrackedVoice_ {};        // sticky tracked voice index (-1 via .fill in ctor)
    std::array<std::array<uint8_t, ambika::dsp::MOD_SRC_LAST>, kNumParts> fxFadeStart_ {};  // crossfade "from" snapshot
    std::array<float, kNumParts> fxFadePhase_ {};         // 0..1 (1 = settled; live values used directly)
    static constexpr double kFxCrossfadeTauSec = 0.005;   // ~5 ms de-click on a voice change

    // Drift-free fractional internal-block boundary position (host-sample
    // units), carried across blocks, for the FX mod-matrix sub-chunking loop in
    // renderPartFx. At host-rate, an internal block (40 @ 39216) spans
    // 40*sr/39216 ≈ 48.96 host samples (non-integer); this phase tracks the
    // fractional boundary so the ~980 Hz cadence is exact over time.
    std::array<double, kNumParts> fxSubPhase_ {};

    // ---- Base-only param de-click (Task: smooth knob/preset jumps, pass LFO
    // modulation RAW). FX param knobs are 7-bit (0..127). The LFO mod source
    // is 8-bit (0..255) and ramps continuously at the 980 Hz cadence, so
    // continuous modulation does NOT need smoothing. A smoother there would
    // only SLEW/band-limit audio-rate modulation. Only abrupt BASE changes
    // (manual knob jumps, preset loads, double-click-to-default) produce
    // discontinuous steps that click.
    // The de-click one-pole is applied to the BASE ONLY; the mod-matrix offset
    // is added RAW. This gives audio-rate modulation parity with the synth voice
    // path (which applies CV raw at 980 Hz) + de-clicked manual jumps.
    // 15 ms (2026-08-21, was 3 ms): a FAST knob drag fires up to ~8 param
    // ticks per audio block; the 3 ms tau tracked that almost instantly and
    // the per-sub-chunk param steps stepped the FX output directly (the
    // wavefolder fold drag measured 0.05-0.10 diff-impulse zipper at 980 Hz
    // sub-chunk cadence). 15 ms is the standard dezipper window: knob drags
    // glide, and preset loads land over one smooth ~30 ms transition. The FX
    // MOD MATRIX still passes through RAW (audio-rate parity preserved).
    static constexpr double kBaseDeClickTauSec = 0.015;
    std::array<std::array<std::array<float, kNumFxSlotParams>, kNumFxSlots>, kNumParts>
        smoothedBase_ {};   // per-part per-slot per-param smoothed base value (AT-only)
    std::array<std::array<uint8_t, kNumFxSlots>, kNumParts> prevSlotType_ {};   // type-change detection

    // Test-only: the capture-ring entry count used by the last renderPartFx for
    // @p part (proves the per-internal-block mod-source capture is populated, so
    // the FX mod matrix runs at ~980 Hz rather than falling back to a single
    // held snapshot at host-block rate).
    std::array<int, kNumParts> debugLastFxRingCount_ {};
    // Test-only: peak-to-peak swing of the EFFECTIVE param (smoothed base + raw
    // mod) for slot 0 param 0 of @p part during the last renderPartFx. Proves the
    // mod reaches the FX at full depth with no slew (a blanket smoother would
    // attenuate high-rate modulation, shrinking this swing).
    std::array<float, kNumParts> debugEffParamMin_ {};
    std::array<float, kNumParts> debugEffParamMax_ {};
    bool debugEffParamTracking_ = false;
    // AT-side cache of each Part's base FX values + mod-matrix config, read from
    // fxState when fxDirty_ is serviced and reused every block. (Mod sources
    // change block-to-block, but the base values + matrix routing are stable
    // between edits.) AT-only. Effective chain values = base + mod-matrix
    // offset.
    struct FxPartCache {
        float   baseDryWet[kNumFxSlots] {};
        float   baseParam [kNumFxSlots][kNumFxSlotParams] {};
        uint8_t modSrc    [kNumFxMatrixSlots] {};
        uint8_t modDst    [kNumFxMatrixSlots] {};
        int8_t  modAmt    [kNumFxMatrixSlots] {};
    };
    std::array<FxPartCache, kNumParts> fxCached_ {};

    // ---- renderPartFx stage helpers (2026-08-23 decomposition) ----
    // Each helper holds one stage of the per-part FX render, moved verbatim
    // from the former ~440-line renderPartFx body. Statement order, loop
    // bounds and memory orders are unchanged; renderPartFx calls them in the
    // original order. Audio thread only.
    bool serviceFxDirtyFrame (int p, Part& part, FxChain& chain, FxPartCache& cache);
    float* sumPartMono (int p, int numSamples);
    AmbikaVoice* pickRepresentativeVoice (int p, int& newestIdx, uint64_t& newestSeq,
                                          int& ringCount);
    void runFxSubChunkLoop (int p, float* mono, int numSamples, AmbikaVoice* repVoice,
                            int newestIdx, uint64_t newestSeq, int ringCount, bool uiTelTrack);
    void buildIdleTelemetryRow (int p, uint8_t* idleRow);
    // ---- Tail-length cache (see getTailLengthSeconds) ----
    // Written by recomputeTailCache() (relaxed; advisory read by the host via
    // the processor's getTailLengthSeconds) on the audio thread. The BPM used
    // by the last recompute: tempo-synced delay tails change with the tempo, so
    // processTransport refreshes the cache when the tempo moves materially.
    std::atomic<float> tailSecondsCache_ { (float) kTailFloorSeconds };
    std::atomic<double> tailBpmCache_ { 120.0 };
    // Mono scratch buffer for the per-part voicecard sum (sized in prepare; AT-only).
    juce::AudioBuffer<float> fxMonoScratch_;
    parvati::TransportClock transport_;
    // Typed view of the juce::Synthesiser voice pool. The ctor adds exactly
    // kNumVoices AmbikaVoice objects and nothing else, so voicePool_[i] aliases
    // voices[i] without a dynamic_cast. The base class keeps ownership.
    std::array<AmbikaVoice*, kNumVoices> voicePool_ {};
    // Reused per-block scratch MidiBuffer (avoids an audio-thread allocation in
    // processTransport's note-routing pass; clear() each block keeps capacity).
    juce::MidiBuffer processedMidi_;
    int  currentPart_ = 0;
    bool wasPlaying_ = false;
    bool partsSeeded_ = false;   // seed Part storage from the init patch once
    std::atomic<bool> allocationDirty_ { false };   // set by message thread; serviced on the audio thread
    std::atomic<bool> resetAllVoicesPending_ { false };   // message thread -> audio thread: kill every voice on a patch switch

    // ---- UI live-modulation telemetry state (docs/LIVE_MOD_FEEDBACK_DESIGN.md) ----
    // Thread roles are FIXED: the AUDIO thread is the sole writer of the plain
    // members below (renderPartFx); the MESSAGE thread is the sole reader
    // (readUiTelemetry via the editor's poll). uiTelSeq_ is the seqlock
    // separating them — the exact Part::readPendingConfig discipline: single
    // writer, single reader, bounded 64-retry reader, odd-begin /
    // release-fence / write / release-fence / even-end writer. The atomics
    // marked MT are written by the message thread and read by the audio thread.
    //
    // Frame layout: uiTel_ carries the CURRENT sources + envelope/filter
    // observables; its history[] is the RING whose next-write position lives
    // IN THE FRAME (uiTel_.historyHead, written under the same seqlock
    // critical sections — the head is metadata the reader needs to linearize,
    // so it is guarded exactly like the samples; a separate plain member was
    // a torn-read window). readUiTelemetry linearizes it OLDEST-FIRST into
    // the caller's frame. Storage is fixed-size (no allocation anywhere on
    // the audio-thread path).
    parvati::ModTelemetrySnapshot uiTel_ {};   // ring storage + observables (AT writes under the seqlock)
    std::atomic<uint32_t> uiTelSeq_ { 0 };       // seqlock: even = stable, odd = writer mid-update
    std::atomic<uint32_t> uiTelemetryEpoch_ { 0 };   // MT-authoritative validity epoch (resetUiTelemetry bumps)
    std::atomic<int>  uiTelPart_ { -1 };         // MT -> AT: the tracked part (-1 = not tracking)
    std::atomic<bool> uiTelResetReq_ { false };  // MT -> AT: wipe request (paired with the epoch bump)
    int  uiTelDecim_ = 0;                        // AT: internal-block decimation counter (history appends)
    int      uiTelVoiceSlot_ = -1;               // AT: STICKY telemetry voice (see renderPartFx): one voice per note
    uint64_t uiTelVoiceSeq_  = 0;                // AT: its triggerSeq (a recycled slot never masquerades as sticky)
    // IDLE DRAG-OUT state (2026-08-22): on release the per-voice rows fall to
    // zero LINEARLY at the strip's scroll pace (1 byte per append = a
    // full-scale fall across one 256-append window) instead of snapping.
    std::array<uint8_t, ambika::dsp::MOD_SRC_LAST> uiTelIdlePrev_ {};
    bool     uiTelIdleSeeded_  = false;
    bool     uiTelLiveSeen_    = false;   // a live append happened since the wipe (gates the seed)
    uint8_t  uiTelNoteSeqLast_ = 0;              // last spare-slot value (melody decay seed)
    int  uiTelWrittenPart_ = -2;                 // AT: the part the frame was last serviced for (-2 => never)
    bool uiTelWasActive_ = false;                // AT: gates the one write on the active->inactive transition
    // History append decimation: the sub-chunk loop ticks at the internal-block
    // cadence (kInternalSampleRate/40 = 980.4 Hz); appending every 12th tick
    // gives 980.4/12 ~= 81.7 appends/s, so kHistoryLen(256) spans ~3.13 s of
    // recent motion — the Pigments-style window the pill sparklines draw.
    static constexpr int kUiTelDecimBlocks = 12;

    // AT helpers (called only from renderPartFx for the tracked part).
    // Service stage: clear the frame when the tracked part changed or a reset
    // was requested (stamps the CURRENT epoch + part so the reader sees the
    // frame as fresh-but-empty until history repopulates).
    void uiTelServiceStage (int p);
    // Decimated history append of this internal block's effective sources
    // (also refreshes uiTel_.sources).
    bool uiTelAppendHistory (const uint8_t* effSrcs, const parvati::Sequencer& noteSeq,
                             int noteSeqOverride = -1);
    // Once-per-block observables refresh: envelope stage/progress/level,
    // effective cutoff/resonance/mode, current sources, voiceActive. @p repVoice
    // may be null — that path fires exactly ONCE on the active->inactive
    // transition (setting voiceActive=false) and never on a steady idle part.
    // @p currentSources = the freshest effective mod-source bytes for the
    // tracked part this block (renderPartFx's lastModSources_[p]).
    void uiTelUpdateObservables (AmbikaVoice* repVoice, const uint8_t* currentSources);

    // UI-mirror invalidation version (see getDisplayVersion). Bumped by the
    // message-thread mutators of Patch-page-mirrored state; relaxed — it is a
    // pure change hint, never a data fence.
    std::atomic<uint32_t> displayVersion_ { 0 };
    void bumpDisplayVersion() noexcept { displayVersion_.fetch_add (1, std::memory_order_relaxed); }

    // Global option staging (message thread writes, audio thread applies via
    // optionsDirty_ release/acquire). Replaces the message-thread voice iteration
    // in setVcaExponential/setParameterSmoothing/setFilterDrive that wrote plain
    // per-voice fields the audio thread reads in fillInternalBlock.
    std::atomic<bool> optionsDirty_ { false };
    std::atomic<bool> pendingVcaExp_ { false };
    std::atomic<bool> pendingSmoothing_ { false };
    std::atomic<float> pendingFilterDrive_ { 1.0f };

    float bendRangeSemitones_ = 2.f;   // per-voice pitch-bend range (MPE default)

    // Standing-bend latch (per MIDI channel; index 0 unused — JUCE channels are
    // 1-based). Written in handlePitchWheel, read by applyStandingBend so a
    // voice triggered while a wheel is off-centre INHERITS the bend (pre-fix a
    // new note started at 0 offset until the next wheel move — audible with
    // MPE / latched wheels). Atomic: wheel events arrive on the audio thread,
    // triggerVoice runs on both.
    std::array<std::atomic<int16_t>, 17> lastWheel_ {{}};

    static bool ok (int part) { return part >= 0 && part < kNumParts; }

    // ---- firmware routing predicates (multi.h PartMapping) ----
    // accept_note (multi.h:47-53): contiguous zone (low<=high) accepts
    // low..high; a WRAP zone (low>high) accepts the complement (note<=high OR
    // note>=low) — the classic hardware wrap split. Static: pure function of
    // the atomic keyrange snapshot.
    static bool partAcceptsNote (const Part& pm, int note);
    // receive_channel (multi.h:41-43): Omni (0) or exact 1-based match.
    static bool partAcceptsChannel (const Part& pm, int channel);
    // accept_channel_note (multi.h:55-57) == receive_channel && accept_note.
    static bool partAcceptsChannelNote (const Part& pm, int channel, int note);

    // MULTICAST note routing (firmware Multi::NoteOn/NoteOff deliver to EVERY
    // accepting part — W8 item 4). Calls @p fn for each accepting part index
    // in part order; returns the count. Template on the functor so there is
    // no std::function / heap indirection on the audio thread. NOTE: unlike
    // the three predicates above this reads parts_, so it is a CONST member
    // (the predicates are static because other translation units reuse them).
    template <typename Fn>
    int forEachAcceptingPart (int channel, int note, Fn&& fn) const
    {
        int n = 0;
        for (int p = 0; p < kNumParts; ++p)
            if (partAcceptsChannelNote (parts_[(size_t) p], channel, note))
            {
                fn (p);
                ++n;
            }
        return n;
    }

    // Channel-only twin of forEachAcceptingPart: call @p fn for every Part
    // whose receive channel matches @p channel (Omni or exact; no keyzone
    // test). Same template discipline (no std::function on the audio thread).
    template <typename Fn>
    int forEachPartOnChannel (int channel, Fn&& fn) const
    {
        int n = 0;
        for (int p = 0; p < kNumParts; ++p)
        {
            const uint8_t ch = parts_[(size_t) p].midiChannel.load (std::memory_order_relaxed);
            if (ch != 0 && ch != channel)
                continue;
            fn (p);
            ++n;
        }
        return n;
    }

    // Call @p fn for every ACTIVE voice playing @p midiChannel (the MPE
    // channel-global routing used by pitch bend / channel pressure / CC74).
    template <typename Fn>
    void forEachActiveVoiceOnChannel (int midiChannel, Fn&& fn)
    {
        for (auto* av : voicePool_)
            if (av->isVoiceActive() && av->isPlayingChannel (midiChannel))
                fn (av);
    }

    // Host wheel 0..16383 (centre 8192) -> semitones via the per-voice bend
    // range. The one conversion shared by handlePitchWheel and the standing-
    // bend latch.
    float wheelToSemitones (int wheel) const noexcept
    {
        return (static_cast<float> (wheel) - 8192.0f) / 8192.0f * bendRangeSemitones_;
    }

    // First Part whose channel+keyzone accepts (channel,note); -1 if none.
    // Retained for callers whose downstream semantics are inherently
    // per-one; under multicast routing this is "first accepting part in part
    // order", not an exclusive route.
    int findPartForNote (int channel, int note) const;

    // Recompute every Part's voiceIndices from its voiceAllocation bitmask
    // (first-wins across Parts) and re-tag each voice's partIndex.
    void rebuildVoiceAllocation();

    // Push the resolved tuning table of @p part into every voice it owns
    // (audio-thread service of frameDirty_ — byte-4 preset
    // edits ride the frame push; the table follows the frame in the same
    // pass so a raga change never needs a separate dirty flag). The per-voice
    // setTuningOffsets writes are idempotent so double application is harmless.
    void pushTuningToVoices (int part);

    // (Re)initialise a Part's voice allocator for its current polyphony mode
    // and voice set (firmware Part::InitializeAllocators, part.cc:240).
    void initAllocator (Part& p);

    // Voice selection WITHIN a Part (never touches other Parts' voices).
    // incomingChannel tags the triggered voice with its real MIDI channel.
    // Thus the per-channel expression routing (handlePitchWheel /
    // ChannelPressure / Controller) isolates per-note under MPE (Omni Part)
    // and stays channel-wide under standard single-channel MIDI. For
    // non-Omni Parts incomingChannel == the Part's channel (findPartForNote
    // matched), so behaviour is unchanged. Arp/sequencer notes pass the
    // Part's channel.
    void triggerNoteInPart (int part, int note, float velocity, int incomingChannel);
    void releaseNoteInPart (int part, int note, int incomingChannel);

    // Push the current SEQ_1/2 values into each Part's own voices.
    void injectSequencerModulation();

    // juce::Synthesiser routing hooks (noteOn/noteOff are virtual; handleNoteOn
    // is not, in JUCE 9).
    void noteOn  (int midiChannel, int midiNoteNumber, float velocity) override;
    void noteOff (int midiChannel, int midiNoteNumber, float velocity, bool allowTailOff) override;

    // ---- All Notes Off (CC123) / All Sound Off (CC120) (W7, firmware midi.h
    // 0x7b/0x78 -> Multi::AllNotesOff -> per-channel Part::AllNotesOff) ----
public:
    // juce::Synthesiser::handleMidiEvent intercepts both messages BEFORE the
    // controller dispatch and calls allNotesOff() — so THIS override is the
    // single service point (the base only stops channel-matched VOICES; the
    // per-part arp held-key stacks, sequencer notes and mono stacks would
    // survive and the arp would keep re-triggering from "held" keys).
    // Firmware treats CC120 identically through the same path;
    // Part::AllNotesOff is a no-op while the sustain pedal holds — mirrored.
    // NOTE: the base ALSO clears its own sustain-pedal-down bookkeeping on
    // this message; ours deliberately does NOT clear the per-part pedal hold
    // (firmware keeps ignore_note_off_messages_ set — the pedal must keep
    // holding back the note-offs).
    void allNotesOff (int midiChannel, bool allowTailOff) override;

private:

    // juce::Synthesiser expression routing (MPE / pitch-bend fix). Unified
    // per-channel routing: each handler targets the ACTIVE voices whose MIDI
    // channel matches (isVoiceActive() && isPlayingChannel()). Under MPE a
    // note's channel is unique => per-note; under standard MIDI all notes share
    // one channel => channel-wide. These override the base, whose per-voice
    // callbacks (pitchWheelMoved / channelPressureChanged / controllerMoved) are
    // no-ops in AmbikaVoice.
    void handlePitchWheel      (int midiChannel, int wheelValue) override;
    void handleChannelPressure (int midiChannel, int channelPressureValue) override;

    // POLYPHONIC AFTERTOUCH (W8 item 3, firmware multi.h:156-162 ->
    // part.cc:485-526): unlike the three per-channel handlers above, the
    // firmware routes poly-AT PER NOTE through the full accept_channel_note
    // predicate (channel AND zone), then — inside the part — per polyphony
    // mode: POLY/CYCLIC/CHAIN write the voice that plays THAT note; UNISON_2X
    // writes the pair; MONO does a channel-wide write instead (all the
    // part's voices, like channel pressure). Implemented in the multicast
    // routing family: every accepting part handles the note.
    void handleAftertouch (int midiChannel, int midiNoteNumber, int aftertouchValue) override;

    void handleController      (int midiChannel, int controllerNumber, int controllerValue) override;

    // Trigger a voice for a note-on and stamp it as the most-recently-triggered
    // (so the FX representative-voice tracker picks it). Wraps juce::Synthesiser
    // ::startVoice / AmbikaVoice::retriggerNote so EVERY trigger site stays in
    // sync with the FX tracker without per-call boilerplate.
    void triggerVoice   (AmbikaVoice* av, juce::SynthesiserSound* sound,
                         int channel, int note, float velocity);
    void retriggerVoice (AmbikaVoice* av, juce::SynthesiserSound* sound,
                         int channel, int note, float velocity);
    uint64_t nextTriggerSeq() noexcept { return triggerSeqCounter_.fetch_add (1, std::memory_order_relaxed) + 1; }

    // GLOBAL continuous-controller (mod wheel CC1 / breath CC2 / foot CC4)
    // mod-matrix write — sets the given mod source on the voices of every Part
    // whose channel matches @p channel (Omni or exact; firmware Part::
    // WriteToAllVoices applies to the channel-routing PART's allocated voicecards
    // — multi.cc ControlChange routes by part first). See the .cpp for why this
    // also gives new notes current-wheel pickup automatically.
    void applyGlobalModSource (int modSrcEnum, uint8_t value0to254, int midiChannel);

    // Sustain pedal (CC64) drain (W7): release every note the pedal swallowed
    // for @p part through its normal release path (arp-held -> arp.noteOff,
    // else releaseNoteInPart). Audio thread only (called from handleController
    // on pedal-up and after CC123 clears the store).
    void drainSustainedNotes (int part);

    // CC123 (All Notes Off) / CC120 (All Sound Off) for @p part — firmware
    // Part::AllNotesOff (part.cc:540): clears the allocators + held keys and
    // releases every allocated voice. NO-OP while the sustain pedal is held
    // (firmware checks ignore_note_off_messages_ first). Audio thread only.
    void partAllNotesOff (int part, bool allowTailOff);

    // Apply the latched standing bend of @p channel to a just-triggered voice
    // (see lastWheel_): same fixed ±2-semitone conversion as handlePitchWheel,
    // routed through the voice's normal setMpePitchBendSemitones path so the
    // oscillator offset AND the mod-matrix source both pick it up.
    void applyStandingBend (AmbikaVoice* av, int channel);

    // Monotonic trigger counter backing nextTriggerSeq() / the per-voice
    // triggerSeq_ stamps (see the FX representative-voice tracker above).
    std::atomic<uint64_t> triggerSeqCounter_ { 0 };

    // juce::Synthesiser audio hook: route each voice's mono render into its
    // FIXED voicecard buffer instead of the master buffer. The processor fills
    // the master (main + aux) from these after renderNextBlock returns.
    void renderVoices (juce::AudioBuffer<float>& outputAudio, int startSample, int numSamples) override;
};

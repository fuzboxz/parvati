// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// SynthEngine — a juce::Synthesiser owning 6 AmbikaVoice instances (one per
// firmware voicecard), divided among up to kNumParts (6) Parts (multitimbral,
// hardware-accurate). Each Part
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
#include "dsp/fx/FxChain.h"   // per-part FX chains (FxChain, FxType)
#include "dsp/patch.h"

// Authentic hardware = 6 voicecards => 6 Parts.
static constexpr int kNumParts  = 6;
// Per-part voice-slot ceiling (Parvati extension). The engine owns a fixed
// pool of kNumParts * kMaxVoicesPerPart voices so EVERY Part can be maxed out
// SIMULTANEOUSLY: the pool always satisfies the sum of all Parts' slot
// settings, so allocation never steals between Parts. A Part's live voice
// count is its per-part `voiceSlots` setting (0 = AUTO: one voice per
// allocated voicecard, faithful 6-voice hardware behaviour; 1..16 = a fixed
// slot count drawn from the pool). Idle pool voices are gated silent and
// skipped by the renderer, so the large pool costs nothing until played;
// worst-case CPU scales with PLAYED voices only.
static constexpr int kMaxVoicesPerPart = 16;
static constexpr int kNumVoices = kNumParts * kMaxVoicesPerPart;   // 96

// ===== Parvati-exclusive per-part FX (Ambika knows nothing about these) =====
// FxType / FxTopology / FxModDestination / kNumFxSlots / kNumFxMatrixSlots /
// kNumFxSlotParams / fxOrderPermutation. Kept in a dependency-free shard
// (dsp/fx/FxTypes.h) so the FX DSP core can use them without pulling in all of
// SynthEngine.h (avoids a circular include: this file -> FxChain.h ->
// FxProcessor.h).
#include "dsp/fx/FxTypes.h"

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

// Fixed-size array of atomically-accessed bytes. The message thread writes
// (apply*/loads/seed) and the audio thread reads (pushPartBytesToVoices, spread,
// polyphony) the same patch/part byte storage; per-byte atomic access removes
// the data race a plain std::array would have under concurrent re-dirtying (the
// per-Part frameDirty_ release/acquire still orders a whole frame's publish, but
// the individual byte reads/writes must themselves be atomic to satisfy the C++
// memory model / TSAN). Element proxies keep the existing `arr[i] = v` and
// `uint8_t x = arr[i]` call sites working unchanged; whole-array ops use the
// loadFrom / fill / assignFrom / copyTo helpers.
template <size_t N>
struct AtomicByteArray
{
    AtomicByteArray() { for (auto& x : a) x.store (0, std::memory_order_relaxed); }

    struct Ref {            // proxy for `arr[i] = v` AND read-through on a non-const Part
        std::atomic<uint8_t>& r;
        uint8_t operator= (uint8_t v) const { r.store (v, std::memory_order_relaxed); return v; }
        operator uint8_t() const { return r.load (std::memory_order_relaxed); }
    };
    struct ConstRef {       // proxy for `uint8_t x = arr[i]` on a const Part
        const std::atomic<uint8_t>& r;
        operator uint8_t() const { return r.load (std::memory_order_relaxed); }
    };
    Ref      operator[] (size_t i)       { return { a[i] }; }
    ConstRef operator[] (size_t i) const { return { a[i] }; }

    void loadFrom (const uint8_t* src)                 { for (size_t i = 0; i < N; ++i) a[i].store (src[i], std::memory_order_relaxed); }
    void fill (uint8_t v)                              { for (auto& x : a) x.store (v, std::memory_order_relaxed); }
    AtomicByteArray& operator= (const std::array<uint8_t, N>& src) { for (size_t i = 0; i < N; ++i) a[i].store (src[i], std::memory_order_relaxed); return *this; }
    void copyTo (std::array<uint8_t, N>& dst) const    { for (size_t i = 0; i < N; ++i) dst[i] = a[i].load (std::memory_order_relaxed); }

    std::array<std::atomic<uint8_t>, N> a;
};

// Per-part FX storage. MT writes (engine setters called by applyFxParameter),
// AT reads (renderPartFx). Each field is atomic; fxDirty_ (release-store by MT,
// acq_rel-exchange by AT) publishes a frame of writes. EXACTLY the frameDirty_ /
// optionsDirty_ pattern (processTransport's dirty-flag service loop). Values are
// stored as raw 0..127 / -63..63 controller-style bytes; the AT normalizes them
// to 0..1 floats when servicing the chain (FxChain::setSlotDryWet/Param).
struct PartFxState
{
    std::atomic<uint8_t> slotType   [kNumFxSlots] {};               // FxType
    std::atomic<uint8_t> slotEnabled[kNumFxSlots] {};               // 0/1
    std::atomic<uint8_t> slotDryWet [kNumFxSlots] {};               // 0..127
    std::atomic<uint8_t> slotParam  [kNumFxSlots][kNumFxSlotParams] {}; // 0..127
    std::atomic<uint8_t> topology { 0 };                            // FxTopology
    std::atomic<uint8_t> orderIdx { 0 };                            // 0..5 (perm of {0,1,2})
    // Master section (engine-state v3). Defaults preserve prior audio:
    // mix=127 (fully wet), EQ at unity/no-cut.
    std::atomic<uint8_t> mix { 127 };                              // global chain wet/dry 0..127
    std::atomic<uint8_t> eqLow { 0 };                              // low-cut (high-pass) 0..127
    std::atomic<uint8_t> eqMid { 64 };                             // mid peaking gain 0..127 (64 = 0 dB)
    std::atomic<uint8_t> eqHigh { 64 };                            // high-shelf gain 0..127 (64 = 0 dB)
    std::atomic<uint8_t> modSource[kNumFxMatrixSlots] {};          // MOD_SRC_* index
    std::atomic<uint8_t> modDest  [kNumFxMatrixSlots] {};          // FxModDestination
    std::atomic<int8_t>  modAmount[kNumFxMatrixSlots] {};          // -63..+63
    std::atomic<bool>    fxDirty_ { false };
};

struct Part
{
    AtomicByteArray<112> patchBytes {};   // sizeof(Patch) — MT writes, AT reads
    AtomicByteArray<84>  partBytes  {};   // sizeof(PartData) — MT writes, AT reads
    parvati::Arpeggiator arp;
    parvati::Sequencer   seq;
    // These three are written on the message thread (Multi page / .MUL load) and
    // read on the audio thread (findPartForNote, every note) and (like the
    // routing fields) voiceAllocation is written on the message thread and read
    // on the audio thread (rebuildVoiceAllocation) -> atomic to avoid a data
    // race. (polyphonyMode below stays plain: it is published to the audio
    // thread via the allocationDirty_ release/acquire.)
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
    //
    // pendingConfig_ is a plain struct accessed by TWO threads: the message
    // thread (setters / stageArpSeqFromPartBytes / file loads) WRITES it, and the
    // audio thread (servicePendingConfig) READS it. configDirty_ release/acquire
    // orders a single edit, but a second MT write landing while the AT reads it
    // is a data race (UB; TSAN-flagged; manifested as a host crash on the
    // note-sequencer path). Guarded by a seqlock (pendingSeq_): the MT is the
    // sole writer, the AT the sole reader — the textbook SPSC case.
    struct PendingConfig {
        uint8_t arpMode = 0, arpDirection = 0, arpOctave = 1, arpPattern = 0, arpResolution = 0;
        uint8_t seqLength[3] = { 0, 0, 0 };
        uint8_t seqData[64]  = {};
    };
    PendingConfig pendingConfig_;
    std::atomic<uint32_t> pendingSeq_ { 0 };   // seqlock: even = stable, odd = writer mid-update

    // Message-thread writer: wrap a field mutation so the audio-thread reader
    // never sees a torn pendingConfig_. (Single message thread => sole writer.)
    template <typename Fn>
    void writePendingConfig (Fn&& fn)
    {
        pendingSeq_.fetch_add (1, std::memory_order_relaxed);          // begin (odd)
        std::atomic_thread_fence (std::memory_order_release);
        fn (pendingConfig_);
        std::atomic_thread_fence (std::memory_order_release);
        pendingSeq_.fetch_add (1, std::memory_order_release);          // end (even, publishes data)
    }
    // Audio-thread reader: copy out a consistent snapshot (retry on a concurrent
    // write). Bounded retries: the writer is non-realtime and quick.
    PendingConfig readPendingConfig() const
    {
        for (;;)
        {
            const uint32_t s1 = pendingSeq_.load (std::memory_order_acquire);
            if (s1 & 1u) continue;                              // writer in progress
            PendingConfig copy = pendingConfig_;
            std::atomic_thread_fence (std::memory_order_acquire);
            if (pendingSeq_.load (std::memory_order_acquire) == s1)
                return copy;
        }
    }

    std::atomic<bool> configDirty_ { false };

    // AT-written snapshot of voiceIndices.size() (rebuildVoiceAllocation) so the
    // message thread (editor status strip) never reads voiceIndices directly.
    std::atomic<int> voiceCount_ { 0 };

    std::atomic<uint8_t> voiceAllocation { 0 };   // 6-bitmask over firmware voicecards (vc0..5)
    // Parvati extension: per-part voice-slot count drawn from the engine pool.
    // 0 = AUTO (one voice per allocated voicecard — faithful hardware);
    // 1..kMaxVoicesPerPart = fixed count. Written on the message thread
    // (setPartVoiceSlots), read on the audio thread in
    // rebuildVoiceAllocation — atomic like voiceAllocation, published through
    // the allocationDirty_ release/acquire.
    std::atomic<uint8_t> voiceSlots { 0 };
    // Parvati extension: user-facing part name/alias ("Kick", "Snare", "Lead").
    // Message-thread-only (MIDI routing and the audio thread never read it),
    // so a plain String is safe. Carried by the .parvati multi format and the
    // host engine-state blob (v2); the Ambika .MUL/.PRO formats have no name
    // bytes, so hardware export falls back to "Part N".
    juce::String name;
    uint8_t polyphonyMode = 1;     // POLY (firmware default); PartData byte 15
    PolyAllocator          polyAlloc;   // POLY/CYCLIC/UNISON_2X/CHAIN allocator
    parvati::NoteStack<12> monoStack;   // MONO note-priority stack
    std::vector<int> voiceIndices;   // indices into the Synthesiser's voice list

    // Per-part FX state (MT writes via the engine setters, AT reads in
    // renderPartFx; published by fxDirty_). See PartFxState above.
    PartFxState fxState;
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
    // voice allocation, polyphony (via partBytes[15]) and the current part — so
    // a DAW project reload preserves the full
    // multitimbral setup, not just the current Part. restoreState returns false
    // for an absent/short/foreign blob so the caller can fall back to the legacy
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
    // Arpeggiator/Sequencer objects directly -- the audio thread is the sole
    // writer of those (it services configDirty_ in processTransport), which
    // removes the data race between a file load and the audio-thread clock
    // loop, and keeps pendingConfig_ (the serialize source) in sync with the
    // loaded values. Used by the .MUL and .parvati multi-load paths.
    void stageArpSeqFromPartBytes (int part);   // reads parts_[part].partBytes (atomic)

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
    void setPartChannel  (int part, uint8_t channel) { if (ok (part)) parts_[(size_t) part].midiChannel.store (channel); }
    void setPartKeyrange (int part, uint8_t lo, uint8_t hi) { if (ok (part)) { parts_[(size_t) part].keyrangeLow.store (lo); parts_[(size_t) part].keyrangeHigh.store (hi); } }
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

    // ---- Voice allocation (firmware 6-voicecard bitmask) ----
    // Each firmware voicecard maps to exactly one Parvati voice (voice i ==
    // voicecard i). A Part owns the voicecards whose bits it sets; a voicecard
    // already claimed by an earlier Part is not reassigned (first-wins, like
    // firmware Multi::AssignVoices), and setPartVoiceAllocation additionally
    // enforces EXCLUSIVE ownership (a card newly claimed by a Part is removed
    // from every other Part). Default bitmask = 1<<partIndex.
    void setPartVoiceAllocation (int part, uint8_t bitmask);
    uint8_t getPartVoiceAllocation (int part) const { return ok (part) ? parts_[(size_t) part].voiceAllocation.load (std::memory_order_relaxed) : 0; }

    // ---- Per-part voice slots (Parvati extension) ----
    // slots: 0 = AUTO (follow the voicecard bitmask: one voice per allocated
    // card, faithful hardware), 1..kMaxVoicesPerPart = fixed count drawn from
    // the engine pool. The pool (kNumVoices = kNumParts * kMaxVoicesPerPart)
    // always satisfies every Part simultaneously. Changing slots re-partitions
    // the pool on the audio thread (deferred via markAllocationDirty, the same
    // path as bitmask/polyphony edits). NOTE: a Part with NO allocated cards is
    // disabled regardless of its slot count (the bitmask keeps its ownership /
    // aux-out / hardware-export jobs); slots only add polyphony beyond the
    // card count.
    void setPartVoiceSlots (int part, int slots);
    int  getPartVoiceSlots (int part) const { return ok (part) ? static_cast<int> (parts_[(size_t) part].voiceSlots.load (std::memory_order_relaxed)) : 0; }

    // ---- Part names / aliases (Parvati extension; message-thread only) ----
    // 16-char limit keeps the Multi page rows + .parvati lines tidy. Control
    // characters (newlines) are stripped: the .parvati multi format is
    // LINE-based, so a newline inside a name would corrupt the document on
    // save (and the hardware name chunk only wants printable text anyway).
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
    void setPartName (int part, const juce::String& n) { if (ok (part)) parts_[(size_t) part].name = sanitizePartName (n); }
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
        if (i >= 0 && i < voices.size())
            return dynamic_cast<AmbikaVoice*> (voices[i]);
        return nullptr;
    }

    // Test-only: the number of times FxChain::process() was called for @p part
    // since the last reset (proves renderPartFx sub-chunks at ~980 Hz).
    // (Always compiled: the instrumentation is runtime-gated by debugEffParamTracking_
    // and the counters are trivial, so there is no release-build overhead. Keeping
    // it always-available lets the FX diagnostic tests build in every config.)
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
    // and reads this to prove the modulation reached the DSP at full depth
    // (engine -> renderPartFx -> setSlotDryWet/setSlotParam -> params_/dryWet_).
    float debugGetChainValue (int part, int slot, int field) const noexcept
    {
        if (field == 0)
            return fxChains_[(size_t) part].debugGetDryWet (slot);
        return fxChains_[(size_t) part].debugGetParam (slot, field - 1);
    }

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
    // the Part's bitmask claims), so a Part's audio reaches its individual
    // voicecard outputs no matter how many pool slots it owns. This function is
    // only the pre-rebuild default.
    static int voiceCardForIndex (int voiceIndex);
    // Back-compat: the current Part's arp/seq.
    parvati::Arpeggiator& getArp()       { return parts_[(size_t) currentPart_].arp; }
    parvati::Sequencer&   getSequencer() { return parts_[(size_t) currentPart_].seq; }

    // ---- Per-part FX (Parvati-exclusive; post-render, host-rate stereo) ----
    // Render every Part's FX chain into its stereo FX-output buffer (called from
    // PluginProcessor::processBlock AFTER renderNextBlock, BEFORE the main-bus
    // sum). For each Part: services fxDirty_ single-threaded (pushes the staged
    // FX params + mod-matrix into the chain), builds a per-part mono sum of its
    // voicecard buffers, samples the first active voice's mod sources, evaluates
    // the 16-slot FX mod matrix at block rate, duplicates mono to L+R, and runs
    // the chain. With all fx*_enabled=0 the chain is a dry copy (audibly-
    // identical to the pre-FX path).
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
    // that carries no FX information, so the FX section is a clean slate instead
    // of retaining the previously-loaded patch's FX. Publishes via fxDirty_.
    void resetPartFx (int part);

private:
    std::array<Part, kNumParts> parts_;

    // One mono buffer per voicecard (6 total). Cleared + filled in renderVoices
    // for each sub-block range; the processor mixes them into the output buses.
    std::array<juce::AudioBuffer<float>, kNumParts> voiceCardBuffers_;
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
    // The FX stage is per-part but modulation sources are per-voice, so
    // renderPartFx samples ONE voice per part: the MOST-RECENTLY-TRIGGERED
    // active voice (via the per-voice triggerSeq_), so per-voice sources
    // (VELOCITY / NOTE / per-note MPE) follow the latest note. On any voice
    // IDENTITY change a short crossfade bridges the old voice's last effective
    // source values (lastModSources_) to the new voice's live values, so those
    // per-voice sources glide instead of clicking. Global/part-global sources
    // are identical across voices so the crossfade is a no-op there. AT-only.
    std::array<int, kNumParts> fxTrackedVoice_ {};        // sticky tracked voice index (-1 via .fill in ctor)
    std::array<std::array<uint8_t, ambika::dsp::MOD_SRC_LAST>, kNumParts> fxFadeStart_ {};  // crossfade "from" snapshot
    std::array<float, kNumParts> fxFadePhase_ {};         // 0..1 (1 = settled; live values used directly)
    static constexpr double kFxCrossfadeTauSec = 0.005;   // ~5 ms de-click on a voice change

    // Monotonic trigger counter for the per-voice triggerSeq_ stamps. Bumped at
    // every note-on (audio thread via render, or message thread via a direct
    // noteOn); renderPartFx reads it on the audio thread. Relaxed ordering
    // suffices (only the relative recency across voices matters).

    // Drift-free fractional internal-block boundary position (host-sample
    // units), carried across blocks, for the FX mod-matrix sub-chunking loop in
    // renderPartFx. At host-rate, an internal block (40 @ 39216) spans
    // 40*sr/39216 ≈ 48.96 host samples (non-integer); this phase tracks the
    // fractional boundary so the ~980 Hz cadence is exact over time.
    std::array<double, kNumParts> fxSubPhase_ {};

    // ---- Base-only param de-click (Task: smooth knob/preset jumps, pass LFO
    // modulation RAW). FX param knobs are 7-bit (0..127); the LFO mod source is
    // 8-bit (0..255) and ramps continuously at the 980 Hz cadence, so continuous
    // modulation does NOT need smoothing (it would only SLEW/band-limit audio-
    // rate modulation). Only abrupt BASE changes (manual knob jumps, preset
    // loads, double-click-to-default) produce discontinuous steps that click.
    // The de-click one-pole is applied to the BASE ONLY; the mod-matrix offset
    // is added RAW. This gives audio-rate modulation parity with the synth voice
    // path (which applies CV raw at 980 Hz) + de-clicked manual jumps.
    static constexpr double kBaseDeClickTauSec = 0.003;   // 3 ms: de-clicks a jump over ~5 ms
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
    // fxState when fxDirty_ is serviced and reused every block (mod sources
    // change block-to-block, but the base values + matrix routing are stable
    // between edits). AT-only. Effective chain values = base + mod-matrix offset.
    struct FxPartCache {
        float   baseDryWet[kNumFxSlots] {};
        float   baseParam [kNumFxSlots][kNumFxSlotParams] {};
        uint8_t modSrc    [kNumFxMatrixSlots] {};
        uint8_t modDst    [kNumFxMatrixSlots] {};
        int8_t  modAmt    [kNumFxMatrixSlots] {};
    };
    std::array<FxPartCache, kNumParts> fxCached_ {};
    // Mono scratch buffer for the per-part voicecard sum (sized in prepare; AT-only).
    juce::AudioBuffer<float> fxMonoScratch_;
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

    // Trigger a voice for a note-on, stamping it as the most-recently-triggered
    // (so the FX representative-voice tracker picks it). Wraps juce::Synthesiser
    // ::startVoice / AmbikaVoice::retriggerNote so EVERY trigger site stays in
    // sync with the FX tracker without per-call boilerplate.
    void triggerVoice   (AmbikaVoice* av, juce::SynthesiserSound* sound,
                         int channel, int note, float velocity);
    void retriggerVoice (AmbikaVoice* av, juce::SynthesiserSound* sound,
                         int note, float velocity);
    uint64_t nextTriggerSeq() noexcept { return triggerSeqCounter_.fetch_add (1, std::memory_order_relaxed) + 1; }

    // GLOBAL continuous-controller (mod wheel CC1 / breath CC2 / foot CC4)
    // mod-matrix write — sets the given mod source on EVERY voice (faithful to
    // firmware Part::WriteToAllVoices over all allocated voicecards). See the
    // .cpp for why this also gives new notes current-wheel pickup for free.
    void applyGlobalModSource (int modSrcEnum, uint8_t value0to254);

    // Monotonic trigger counter backing nextTriggerSeq() / the per-voice
    // triggerSeq_ stamps (see the FX representative-voice tracker above).
    std::atomic<uint64_t> triggerSeqCounter_ { 0 };

    // juce::Synthesiser audio hook: route each voice's mono render into its
    // FIXED voicecard buffer instead of the master buffer. The processor fills
    // the master (main + aux) from these after renderNextBlock returns.
    void renderVoices (juce::AudioBuffer<float>& outputAudio, int startSample, int numSamples) override;
};

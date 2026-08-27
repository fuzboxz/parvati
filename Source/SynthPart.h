// Copyright (c) 2026 805Labs Kft. / Hellcat.
//
// SynthPart — the per-part data model of SynthEngine, in a dependency-light
// shard: PolyAllocator, AtomicByteArray, PartFxState and Part, plus the
// kNumParts / kMaxVoicesPerPart / kNumVoices voice-pool constants. SynthEngine
// includes this header; nothing here needs the Synthesiser itself. Every
// definition moved here verbatim from SynthEngine.h (2026-08-23 extraction).

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "Arpeggiator.h"
#include "NoteStack.h"
#include "Sequencer.h"

// FxTypes.h is the dependency-free FX shard: it carries the slot counts
// (kNumFxSlots / kNumFxSlotParams / kNumFxMatrixSlots) PartFxState stores.
#include "dsp/fx/FxTypes.h"
// patch_sanitizer.h carries the arp/seq field enum (ArpSeqField) that names
// the pendingConfig_ members below. Dependency-light: no JUCE module enters.
#include "dsp/patch_sanitizer.h"

namespace hellcat
{
// ---- seqlock primitives (SPSC: one writer thread, one reader thread) ----
// The protocol behind every guarded frame in this header and the engine
// telemetry. Writer: begin (odd) + release fence, body, release fence + end
// (even). Reader: bounded 64-attempt acquire copy with a sequence re-check.
// Dependency-free: <atomic> only.

// Writer begin: turn the sequence odd. The release fence keeps the body's
// stores below it.
inline void seqlockBegin (std::atomic<uint32_t>& seq)
{
    seq.fetch_add (1, std::memory_order_relaxed);   // begin (odd)
    std::atomic_thread_fence (std::memory_order_release);
}

// Writer end: publish the body's stores, then turn the sequence even.
inline void seqlockEnd (std::atomic<uint32_t>& seq)
{
    std::atomic_thread_fence (std::memory_order_release);
    seq.fetch_add (1, std::memory_order_release);   // end (even, publishes data)
}

// Reader: copy the guarded frame, bounded 64 attempts. Returns true when the
// copy is consistent; @p out then holds a stable snapshot. On exhaustion
// returns false and leaves @p out untouched, so the caller keeps its
// fallback logic at the call site.
template <typename T>
bool seqlockTryRead (const std::atomic<uint32_t>& seq, const T& src, T& out)
{
    for (int attempt = 0; attempt < 64; ++attempt)
    {
        const uint32_t s1 = seq.load (std::memory_order_acquire);
        if (s1 & 1u)
            continue;                                // writer mid-update
        T copy = src;
        std::atomic_thread_fence (std::memory_order_acquire);
        if (seq.load (std::memory_order_acquire) == s1)
        {
            out = copy;
            return true;
        }
    }
    return false;
}
} // namespace hellcat

// Authentic hardware = 6 voicecards => 6 Parts.
static constexpr int kNumParts  = 6;
// Per-part voice-slot ceiling (Hellcat extension). The engine owns a fixed
// pool of kNumParts * kMaxVoicesPerPart voices. Thus EVERY Part can be maxed
// out SIMULTANEOUSLY. The pool always satisfies the sum of all Parts' slot
// settings, so allocation never steals between Parts. voiceSlots is the
// SINGLE SOURCE OF TRUTH for a Part's polyphony (1 voice = digital voice
// section + voicecard). It holds 1..16 voices from the pool. 0 = disabled.
// Only the ctor default and legacy loaders ever store 0. The firmware
// 6-voicecard bitmask is DERIVED from the slot counts (contiguous
// proportional share, mul_export::deriveMasks). The bitmask keeps its
// aux-out routing + .MUL export jobs. Idle pool voices are gated silent and
// skipped by the renderer, so the large pool costs nothing until played.
// Worst-case CPU scales with PLAYED voices only.
static constexpr int kMaxVoicesPerPart = 16;
static constexpr int kNumVoices = kNumParts * kMaxVoicesPerPart;   // 96

// One multitimbral Part. The Arpeggiator/Sequencer objects ARE the per-part
// storage for those settings (edits route to the current Part's objects).
//
// Polyphony: faithful port of ambika::VoiceAllocator (controller/
// voice_allocator.cc) over a Part's voiceIndices. PolyphonyMode
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
    // Forget every note->voice mapping WITHOUT a change to the allocator
    // size/mode (firmware VoiceAllocator::ClearNotes, called by
    // Part::AllNotesOff). The voices themselves are stopped separately. A
    // stale mapping would misroute a later note-off onto a re-stolen slot.
    // The misroute is harmless via releaseNoteInPart's defensive scan, but
    // unclean — W7.
    void clearNotes()
    {
        for (uint8_t i = 0; i < kMax; ++i) pool_[i] = 0;
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
// the same patch/part byte storage (apply*/loads/seed). The audio thread
// reads it (pushPartBytesToVoices, spread, polyphony). Per-byte atomic access
// removes the data race a plain std::array would have under concurrent
// re-dirtying. The per-Part frameDirty_ release/acquire still orders a whole
// frame's publish. But the individual byte reads/writes must themselves be
// atomic to satisfy the C++ memory model / TSAN. Element proxies keep the
// existing `arr[i] = v` and `uint8_t x = arr[i]` call sites unchanged.
// Whole-array ops use the loadFrom / fill / assignFrom / copyTo helpers.
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
// AT reads (renderPartFx). Each field is atomic. fxDirty_ (release-store by
// MT, acq_rel-exchange by AT) publishes a frame of writes. EXACTLY the
// frameDirty_ / optionsDirty_ pattern (processTransport's dirty-flag service
// loop). Values are stored as raw 0..127 / -63..63 controller-style bytes.
// The AT normalizes them to 0..1 floats when it services the chain
// (FxChain::setSlotDryWet/Param).
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

    // Write the clean defaults of every field (the same values a default-
    // constructed PartFxState holds: slots None / bypassed / dry, Series
    // topology, order 0, master mix fully wet, EQ flat, cleared mod matrix).
    // resetPartFx and a state restore use this as the shared clean slate; a
    // blob overwrites the fields it carries afterwards. PartFxState holds
    // atomics (non-copyable), so the writes are field-by-field.
    void storeCleanDefaults() noexcept
    {
        for (int s = 0; s < kNumFxSlots; ++s)
        {
            slotType   [(size_t) s].store (0, std::memory_order_relaxed);   // FxType::None
            slotEnabled[(size_t) s].store (0, std::memory_order_relaxed);
            slotDryWet [(size_t) s].store (0, std::memory_order_relaxed);   // fully dry
            for (int k = 0; k < kNumFxSlotParams; ++k)
                slotParam[(size_t) s][(size_t) k].store (0, std::memory_order_relaxed);
        }
        topology.store (0,   std::memory_order_relaxed);   // Series
        orderIdx.store (0,   std::memory_order_relaxed);
        mix.store      (127, std::memory_order_relaxed);    // fully wet (no-op)
        eqLow.store    (0,   std::memory_order_relaxed);    // flat
        eqMid.store    (64,  std::memory_order_relaxed);    // 0 dB
        eqHigh.store   (64,  std::memory_order_relaxed);    // 0 dB
        for (int m = 0; m < kNumFxMatrixSlots; ++m)
        {
            modSource[(size_t) m].store (0, std::memory_order_relaxed);
            modDest  [(size_t) m].store (0, std::memory_order_relaxed);
            modAmount[(size_t) m].store (0, std::memory_order_relaxed);
        }
    }
};

struct Part
{
    AtomicByteArray<112> patchBytes {};   // sizeof(Patch) — MT writes, AT reads
    AtomicByteArray<84>  partBytes  {};   // sizeof(PartData) — MT writes, AT reads
    // Per-part microtonal tuning: the firmware raga preset (PartData.raga,
    // byte 4). 0 = 12-EDO; 1..32 = firmware raga preset == partBytes[4]. The
    // Hellcat custom-table extension (Scala import / TuningEditor) was REMOVED
    // 2026-08-19 — factory raga presets only. Raga byte edits ride the
    // frameDirty_ publish (the patchBytes pattern); the AT services it in
    // processTransport and pushes the resolved table to the Part's voices
    // (pushTuningToVoices).
    hellcat::Arpeggiator arp;
    hellcat::Sequencer   seq;
    // These three are written on the message thread (Multi page / .MUL load) and
    // read on the audio thread (findPartForNote, every note) and (like the
    // routing fields) voiceAllocation is written on the message thread and read
    // on the audio thread (rebuildVoiceAllocation) -> atomic to avoid a data
    // race. (polyphonyMode below stays plain: it is published to the audio
    // thread via the allocationDirty_ release/acquire.)
    std::atomic<uint8_t> midiChannel  { 0 };   // 0 = Omni (all channels); else 1..16
    std::atomic<uint8_t> keyrangeLow  { 0 };
    std::atomic<uint8_t> keyrangeHigh { 127 };
    // Flagged by the AUDIO-THREAD config service (servicePendingConfig, when
    // an applied arp mode transition turns the arp/seq OFF) — NOT by the
    // message-thread setter; serviced at the top of processTransport (audio
    // thread) to kill this Part's arp/seq-generated voices. stopNote() mutates
    // voice state the audio thread renders, so it must not run on the message
    // thread.
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
    // never sees a torn pendingConfig_. The message thread is the SOLE writer
    // (audio-thread-origin arp/seq/part_select edits are funneled back to the
    // message thread by HellcatAudioProcessor's deferred-parameter drain before
    // they can reach these setters — see PluginProcessor.h / DeferredParamRing;
    // without that deferral this seqlock would have two writers and tear).
    template <typename Fn>
    void writePendingConfig (Fn&& fn)
    {
        hellcat::seqlockBegin (pendingSeq_);
        fn (pendingConfig_);
        hellcat::seqlockEnd (pendingSeq_);
    }
    // Audio-thread reader: copy out a consistent snapshot (retry on a concurrent
    // write). Bounded retries (64): an unbounded spin on the audio thread could
    // burn a whole real-time block under a pathological write storm, which is
    // worse than applying a stale-but-consistent config for one block. On
    // exhaustion the caller learns via @p exhausted (the audio-thread service
    // re-marks configDirty_ so the next block retries) and the AT's
    // lastGoodConfig_ snapshot (or a default-constructed one before the first
    // successful apply) is returned. NOTE: only the AUDIO thread can exhaust:
    // the message-thread callers (captureState / preset save / loadPartIntoApvts)
    // run on the SAME thread as the sole seqlock writer, so they always see a
    // stable even sequence — lastGoodConfig_ is therefore written and read
    // single-threaded in the only exhaustion path that exists.
    PendingConfig readPendingConfig (bool* exhausted = nullptr) const
    {
        PendingConfig copy;
        const bool ok = hellcat::seqlockTryRead (pendingSeq_, pendingConfig_, copy);
        if (exhausted != nullptr)
            *exhausted = ! ok;
        return ok ? copy : (hasLastGoodConfig_ ? lastGoodConfig_ : PendingConfig{});
    }

    // ---- Arp/seq config field access over a pendingConfig_ snapshot ----
    // Single source for the paramID chains that used to hand-code this
    // mapping in five places (see kArpSeqParamMap in ParameterLayout.h). The
    // field enum and its byte domains live in patch_sanitizer.h.
    static uint8_t arpSeqPendingValue (const PendingConfig& pc, ambika::dsp::ArpSeqField f)
    {
        switch (f)
        {
            case ambika::dsp::ArpSeqField::ArpMode:       return pc.arpMode;
            case ambika::dsp::ArpSeqField::ArpDirection:  return pc.arpDirection;
            case ambika::dsp::ArpSeqField::ArpOctave:     return pc.arpOctave;
            case ambika::dsp::ArpSeqField::ArpPattern:    return pc.arpPattern;
            case ambika::dsp::ArpSeqField::ArpResolution: return pc.arpResolution;
            case ambika::dsp::ArpSeqField::SeqLength1:    return pc.seqLength[0];
            case ambika::dsp::ArpSeqField::SeqLength2:    return pc.seqLength[1];
            case ambika::dsp::ArpSeqField::SeqLength3:    return pc.seqLength[2];
        }
        return 0;   // unreachable for the enum's real values
    }

    // Write ONE config field under the pendingConfig_ seqlock (message
    // thread). The caller sets configDirty_ after its whole frame is staged.
    void stageArpSeqConfig (ambika::dsp::ArpSeqField f, uint8_t value)
    {
        writePendingConfig ([f, value] (PendingConfig& pc) {
            switch (f)
            {
                case ambika::dsp::ArpSeqField::ArpMode:       pc.arpMode       = value; break;
                case ambika::dsp::ArpSeqField::ArpDirection:  pc.arpDirection  = value; break;
                case ambika::dsp::ArpSeqField::ArpOctave:     pc.arpOctave     = value; break;
                case ambika::dsp::ArpSeqField::ArpPattern:    pc.arpPattern    = value; break;
                case ambika::dsp::ArpSeqField::ArpResolution: pc.arpResolution = value; break;
                case ambika::dsp::ArpSeqField::SeqLength1:    pc.seqLength[0]  = value; break;
                case ambika::dsp::ArpSeqField::SeqLength2:    pc.seqLength[1]  = value; break;
                case ambika::dsp::ArpSeqField::SeqLength3:    pc.seqLength[2]  = value; break;
            }
        });
    }

    // The audio thread's last SUCCESSFULLY-applied config snapshot (updated at
    // the end of servicePendingConfig). AT-only: the fallback readPendingConfig
    // returns on retry exhaustion. Defaults apply before the first service.
    PendingConfig lastGoodConfig_;
    bool hasLastGoodConfig_ = false;

    std::atomic<bool> configDirty_ { false };

    // AT-written snapshot of voiceIndices.size() (rebuildVoiceAllocation) so the
    // message thread (editor status strip) never reads voiceIndices directly.
    std::atomic<int> voiceCount_ { 0 };

    // DERIVED 6-bitmask over firmware voicecards (vc0..5): written by the
    // audio thread in rebuildVoiceAllocation (mul_export::deriveMasks over
    // the slot counts) and read on the message thread (.MUL export + UI).
    // NOT user state — see voiceSlots.
    std::atomic<uint8_t> voiceAllocation { 0 };
    // Hellcat extension: the Part's VOICE COUNT from the engine pool — the
    // single source of truth for polyphony. 1..kMaxVoicesPerPart = voices;
    // 0 = disabled (only the ctor default / legacy loaders store 0 —
    // setPartVoiceSlots clamps 0 to 1). Written on the message thread
    // (setPartVoiceSlots / legacy loads), read on the audio thread in
    // rebuildVoiceAllocation — atomic like voiceAllocation, published through
    // the allocationDirty_ release/acquire.
    std::atomic<uint8_t> voiceSlots { 0 };
    // Hellcat extension: user-facing part name/alias ("Kick", "Snare", "Lead").
    // Message-thread-only (MIDI routing and the audio thread never read it),
    // so a plain String is safe. Carried by the .yml multi format and the
    // host engine-state blob (v2); the Ambika .MUL/.PRO formats have no name
    // bytes, so hardware export falls back to "Part N".
    juce::String name;
    uint8_t polyphonyMode = 1;     // POLY (firmware default); PartData byte 15
    PolyAllocator          polyAlloc;   // POLY/CYCLIC/UNISON_2X/CHAIN allocator
    hellcat::NoteStack<12> monoStack;   // MONO note-priority stack
    std::vector<int> voiceIndices;   // indices into the Synthesiser's voice list

    // ---- Sustain pedal (CC64): firmware part.cc:335-390 semantics (W7) ----
    // AUDIO-THREAD-ONLY state (like polyAlloc/monoStack): the pedal CC, every
    // note-off routing decision and the pedal-up drain all run inside
    // renderNextBlock/processTransport on the audio thread. Plain fields
    // follow the established discipline (no atomics/locks needed; the message
    // thread never touches these). sustainHold == firmware
    // ignore_note_off_messages_: while set, note-offs for this Part are
    // held back (the notes keep sounding) and remembered in sustainedNotes_.
    // Pedal-up replays them through the normal release path. Capacity 16 ==
    // kMaxVoicesPerPart (a part cannot sound more distinct notes than voices).
    // Overflow (impossible today: >16 held keys with the pedal down) releases
    // the OLDEST entry immediately so nothing strands.
    bool sustainHold_ = false;
    struct SustainedNote { uint8_t note; uint8_t channel; };   // channel 1..16
    SustainedNote sustainedNotes_[kMaxVoicesPerPart] {};
    int numSustainedNotes_ = 0;

    void addSustainedNote (uint8_t note, uint8_t channel)
    {
        // Dedupe (a repeated note-off for an already-sustained note is a no-op).
        for (int i = 0; i < numSustainedNotes_; ++i)
            if (sustainedNotes_[i].note == note) return;
        // Overflow (>16 distinct keys released under the pedal — impossible
        // for a 16-voice part to have 17 SIMULTANEOUS sounding notes; a 17th
        // release can only exist for a voice already stolen by a newer note,
        // whose OWN release is stored separately): drop the oldest entry.
        if (numSustainedNotes_ == kMaxVoicesPerPart)
        {
            for (int i = 1; i < numSustainedNotes_; ++i) sustainedNotes_[i - 1] = sustainedNotes_[i];
            --numSustainedNotes_;
        }
        sustainedNotes_[numSustainedNotes_++] = { note, channel };
    }

    // Per-part FX state (MT writes via the engine setters, AT reads in
    // renderPartFx; published by fxDirty_). See PartFxState above.
    PartFxState fxState;
};

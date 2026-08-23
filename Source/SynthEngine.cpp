// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See SynthEngine.h.

#include <array>
#include <cstring>   // memcpy (blob -> Patch view for sanitization)
#include "SynthEngine.h"

#include "dsp/patch_sanitizer.h"   // sanitizePatch/sanitizePartData (blob ingestion)

#include "stmlib/dsp/dsp.h"    // stmlib::SoftLimit (FX chain-input safety knee)
#include "MulExport.h"        // mul_export::deriveMasks (derived voicecard masks)
#include "VoiceMasks.h"       // parvati::popcount8 (bitmask slot counting)
#include "ParameterLayout.h"   // getControllerInitPatchBytes (audible init patch)
#include "TuningTables.h"     // raga preset tables (resolvedTuningOffsets)

SynthEngine::SynthEngine()
{
    for (int i = 0; i < kNumVoices; ++i)
    {
        auto* av = new AmbikaVoice();
        addVoice (av);
        voicePool_[(size_t) i] = av;   // typed alias of voices[i] (see header)
    }

    // Pre-tag each pool voice round-robin over the 6 firmware voicecards
    // (Ambika hardware: 6 voicecards, each with individual outputs).
    // rebuildVoiceAllocation() (called at the end of the ctor) re-tags every
    // ALLOCATED voice onto its own Part's cards, so this is only the default
    // for unallocated pool voices.
    for (int i = 0; i < kNumVoices; ++i)
        if (auto* av = getAmbikaVoice (i))
            av->setVoiceCard (voiceCardForIndex (i));

    // Standing-bend latch defaults to the wheel CENTRE (8192 = no bend). A
    // zero value would mean "full negative deflection" and bend every
    // first-note-until-a-wheel-event by -2 semitones (see applyStandingBend).
    for (auto& w : lastWheel_)
        w.store (8192, std::memory_order_relaxed);

    addSound (new AmbikaSound());
    // NOTE: juce::Synthesiser's note-stealing path is NOT used — noteOn/noteOff
    // are overridden below and stealing is handled per-part inside the
    // PolyAllocator (SynthEngine.h), so setNoteStealingEnabled would be dead
    // (and its JUCE steal path unreachable).

    // Default voice allocation: SINGLE-PART — all 6 voicecards on Part 0, so a
    // player on MIDI channel 1 gets the full hardware polyphony (6 voices)
    // without setup. This differs from the firmware factory multi
    // (controller/multi.cc: Part0=0x15, Part1=0x2a, a 3+3 multitimbral split).
    // That split remains available: load a factory .MUL or set voice counts
    // per Part on the Patch page. The slots model makes voiceSlots the
    // single source of truth (1 voice = digital voice + voicecard). The
    // default materializes 6 voices on Part 0 (the faithful 6-voice Ambika)
    // and 0 (disabled) elsewhere. rebuildVoiceAllocation() derives the
    // voicecard masks from those counts and partitions the pool.
    constexpr uint8_t kInitVoiceAllocation[kNumParts] = { 0x3f, 0, 0, 0, 0, 0 };
    for (int p = 0; p < kNumParts; ++p)
    {
        auto& part = parts_[(size_t) p];
        // Materialize the Part's real slot count from the legacy init bitmask
        // (popcount; 0 -> disabled), exactly like a legacy-blob/.MUL load —
        // the bitmask itself is only the seed, rebuildVoiceAllocation derives
        // the live masks from the slots.
        part.voiceSlots.store (
            static_cast<uint8_t> (parvati::popcount8 (kInitVoiceAllocation[p])),
            std::memory_order_relaxed);
        part.voiceAllocation.store (kInitVoiceAllocation[p]);
        // Default: part i -> MIDI channel i+1 (so channel 1 -> Part 0).
        part.midiChannel.store  (static_cast<uint8_t> (p + 1));
        part.keyrangeLow.store  (0);
        part.keyrangeHigh.store (127);

        // Wire this Part's arp/seq generated notes to trigger a voice WITHIN
        // this Part (bypassing channel routing — generated notes always belong
        // to the generating Part).
        const int partIdx = p;
        parts_[(size_t) p].arp.setNoteOnCallback ([this, partIdx] (int, int note, uint8_t velocity)
        {
            const int ch = juce::jmax (1, static_cast<int> (parts_[(size_t) partIdx].midiChannel.load()));
            triggerNoteInPart (partIdx, note, velocity / 127.0f, ch);
        });
        parts_[(size_t) p].arp.setNoteOffCallback ([this, partIdx] (int, int note)
        {
            const int ch = juce::jmax (1, static_cast<int> (parts_[(size_t) partIdx].midiChannel.load()));
            releaseNoteInPart (partIdx, note, ch);
        });
        parts_[(size_t) p].seq.setNoteOnCallback ([this, partIdx] (int, int note, uint8_t velocity)
        {
            const int ch = juce::jmax (1, static_cast<int> (parts_[(size_t) partIdx].midiChannel.load()));
            triggerNoteInPart (partIdx, note, velocity / 127.0f, ch);
        });
        parts_[(size_t) p].seq.setNoteOffCallback ([this, partIdx] (int, int note)
        {
            const int ch = juce::jmax (1, static_cast<int> (parts_[(size_t) partIdx].midiChannel.load()));
            releaseNoteInPart (partIdx, note, ch);
        });
    }

    rebuildVoiceAllocation();

    // FX representative-voice tracker defaults: no tracked voice, crossfades
    // settled (so the very first tracked voice is used live, no fade from zero).
    fxTrackedVoice_.fill (-1);
    fxFadePhase_.fill (1.0f);
}

void SynthEngine::prepare (double sampleRate, int blockSize)
{
    setCurrentPlaybackSampleRate (sampleRate);

    // One mono buffer per voicecard, sized to the host block. renderVoices is
    // called with sub-blocks that tile [0, blockSize) without overlap, so each
    // buffer only needs `blockSize` samples of capacity.
    for (auto& b : voiceCardBuffers_)
        b.setSize (1, juce::jmax (1, blockSize), false, true, true);

    // Per-part FX: one stereo FX-output buffer + one FX chain per Part, plus a
    // mono scratch for the per-part voicecard sum. All sized here (never on the
    // audio thread). The chains allocate their internal DSP state in prepare().
    const int fxBlock = juce::jmax (1, blockSize);
    for (int p = 0; p < kNumParts; ++p)
    {
        fxOutputBuffers_[(size_t) p].setSize (2, fxBlock, false, true, true);
        fxChains_[(size_t) p].prepare (sampleRate, fxBlock);
    }
    fxMonoScratch_.setSize (1, fxBlock, false, true, true);
    for (auto& srcs : lastModSources_)
        srcs.fill (0);
    fxSubPhase_.fill (0.0);   // reset the FX mod-matrix sub-chunk phase

    // Reset the base-only de-click state: zero the smoothed bases + type-change
    // tracking so the first fxDirty_ service snaps (no ramp-from-stale).
    for (auto& part : smoothedBase_)
        for (auto& slot : part)
            slot.fill (0.0f);
    for (auto& part : prevSlotType_)
        part.fill (0);

    // Tail cache: pick up any FX state staged BEFORE this prepare (e.g. a host
    // state restore that ran before the first block) so getTailLengthSeconds is
    // correct even if audio never runs. Pure math + atomics: thread-safe here.
    recomputeTailCache();

    for (auto* av : voicePool_)
        av->prepare (sampleRate, blockSize);

    transport_.prepare (sampleRate);

    // Tag each voice with its Part, and seed each Part's patch/part storage
    // from the faithful init patch (once).
    for (int p = 0; p < kNumParts; ++p)
        for (int vi : parts_[(size_t) p].voiceIndices)
            if (auto* av = getAmbikaVoice (vi))
                av->setPartIndex (p);

    if (! partsSeeded_)
    {
        // Seed EVERY Part with the CONTROLLER init patch (Part::InitPatch(DEFAULT):
        // osc1=Saw, audible) + the firmware init_part PartData (volume 120; arp
        // octave 1 / resolution 10; seq length 16; POLY). Also mirror those
        // arp/seq values into the live per-part Arpeggiator/Sequencer OBJECTS,
        // so loadPartIntoApvts (which reads arp/seq from the objects) sees
        // consistent state. Previously this seeded the VOICECARD silence
        // fallback (osc1=None, inaudible) for Parts 1..5 and only set Part 0
        // via the APVTS sync. A freshly-loaded plugin then left Parts 1..5
        // silent until visited -- incorrect vs the firmware (init_part,
        // part.cc:83-100).
        const uint8_t* const initPatch = getControllerInitPatchBytes();
        for (int p = 0; p < kNumParts; ++p)
        {
            parts_[(size_t) p].patchBytes.loadFrom (initPatch);
            parts_[(size_t) p].partBytes.fill (0);
            parts_[(size_t) p].partBytes[0]  = 120;   // volume (init_part)
            parts_[(size_t) p].partBytes[7]  = 0;     // arp / sequencer mode
            parts_[(size_t) p].partBytes[8]  = 0;     // arp direction
            parts_[(size_t) p].partBytes[9]  = 1;     // arp octave range (init_part)
            parts_[(size_t) p].partBytes[10] = 0;     // arp pattern
            parts_[(size_t) p].partBytes[11] = 10;    // arp resolution / divider (init_part => 1/16)
            parts_[(size_t) p].partBytes[12] = 16;    // sequence length 1
            parts_[(size_t) p].partBytes[13] = 16;    // sequence length 2
            parts_[(size_t) p].partBytes[14] = 16;    // sequence length 3
            parts_[(size_t) p].partBytes[15] = 1;     // polyphony_mode = POLY
            // Mirror the init arp/seq config into BOTH the live objects AND
            // pendingConfig_ (the message-thread-authoritative config the audio
            // thread applies via servicePendingConfig, and that serialize /
            // loadPartIntoApvts read). Seeding pendingConfig_ here keeps it in
            // sync with the live objects. Thus a later live edit (which stages
            // only its own field into pendingConfig_ + flags configDirty_)
            // does not re-apply stale defaults and clobber these init values.
            parts_[(size_t) p].arp.setMode (0);              parts_[(size_t) p].seq.setMode (0);
            parts_[(size_t) p].arp.setDirection (0);
            parts_[(size_t) p].arp.setOctave (1);
            parts_[(size_t) p].arp.setPattern (0);
            parts_[(size_t) p].arp.setResolution (10);
            parts_[(size_t) p].seq.setSequenceLength (0, 16);
            parts_[(size_t) p].seq.setSequenceLength (1, 16);
            parts_[(size_t) p].seq.setSequenceLength (2, 16);
            auto& pc = parts_[(size_t) p].pendingConfig_;
            pc.arpMode = 0;  pc.arpDirection = 0;  pc.arpOctave = 1;
            pc.arpPattern = 0;  pc.arpResolution = 10;
            pc.seqLength[0] = 16;  pc.seqLength[1] = 16;  pc.seqLength[2] = 16;
            // seqData stays 0 (matches the init zero-fill); configDirty_ stays
            // false: pendingConfig_ == live objects, nothing for the AT to apply.

            // Pre-size the per-Part voice-index vector to the voice count so a
            // rebuild (rebuildVoiceAllocation, audio thread) never triggers a
            // heap reallocation on the audio thread.
            parts_[(size_t) p].voiceIndices.reserve (kNumVoices);
        }
        partsSeeded_ = true;
    }

    // Arm every Part's allocator for its (default POLY) mode + voice set.
    for (int p = 0; p < kNumParts; ++p)
        initAllocator (parts_[(size_t) p]);
}

//==========================================================================
// APVTS byte bridge — CURRENT part only (also mirrors into Part storage).
void SynthEngine::applyPatchByte (int offset, uint8_t value)
{
    auto& part = parts_[(size_t) currentPart_];
    if (offset >= 0 && offset < 112) part.patchBytes[(size_t) offset] = value;
    // DEFER the voice write to the audio thread: setPatchByte mutates voice_.patch_
    // which the renderer reads every block, so a write here (message thread)
    // was a torn read. Stage it in Part storage (above) + flag frameDirty_.
    // The audio thread pushes the full frame (pushPartBytesToVoices) at the
    // block top.
    // (release publishes the byte write to the audio-thread acquire.)
    part.frameDirty_.store (true, std::memory_order_release);
}

void SynthEngine::applyPartByte (int offset, uint8_t value)
{
    auto& part = parts_[(size_t) currentPart_];
    // Capture the previous polyphony mode before the generic write so we only
    // defer work on an ACTUAL change (syncAllParamsToEngine re-applies every
    // param, including part_polyphony, with its current value — a re-service
    // would rebuild+push every block for no reason and perturb render state).
    const uint8_t prevMode = (offset == 15) ? part.partBytes[15] : 0;
    // Capture the previous values of the Patch-page-mirrored bytes BEFORE the
    // generic write below, so the display-version bump stays change-only.
    // (applyPartByte fires for EVERY part-param automation write; a bump only
    // on a real change of a MIRRORED byte keeps the 30 Hz poll check O(1) and
    // quiet.) Mirrored offsets: 15 (polyphony), 4 (raga/Tune), the three
    // part-character columns — 1 (octave), 5 (legato), 6 (portamento) — and
    // (the completing absorption, 2026-08-20) the output columns 0 (Vol),
    // 2 (Fine tuning), 3 (Spread).
    const auto isMirrorOffset = [] (int o)
    { return o == 15 || o == 4 || o == 1 || o == 5 || o == 6 || o == 0 || o == 2 || o == 3; };
    const uint8_t prevMirrored = isMirrorOffset (offset) ? part.partBytes[(size_t) offset] : 0;
    if (offset >= 0 && offset < 84) part.partBytes[(size_t) offset] = value;
    // PartData byte 15 = polyphony_mode. Defer the mode engage (and, for CHAIN,
    // the voice-set rebuild) to the audio thread via markAllocationDirty() so
    // voiceIndices is never mutated under a concurrent audio-thread reader. The
    // audio-thread service syncs polyphonyMode from partBytes[15] and rebuilds.
    if (offset == 15)
    {
        if (value != prevMode)   // only on a real polyphony-mode change
        {
            markAllocationDirty();
            // The Patch page mirrors byte 15 in its Poly combo — invalidate
            // the visible-page mirror (host automation / NRPN arrive here
            // with no editor hook; see getDisplayVersion).
            bumpDisplayVersion();
        }
        return;
    }
    // The other Patch-page mirrors (byte 4 = raga/Tune combo; bytes 1/5/6 =
    // the absorbed Oct / Lgo / Porta columns) — same visible-mirror
    // invalidation as byte 15 above. Bytes NOT mirrored by the Patch page do
    // not bump the version (keeps the poll check change-only).
    if (isMirrorOffset (offset) && value != prevMirrored)
        bumpDisplayVersion();
    // DEFER the voice write to the audio thread (same fence as applyPatchByte):
    // setPartByte mutates voice_.part_ which the renderer reads; writing it on the
    // message thread was a torn read. Stage in Part storage + frameDirty_.
    part.frameDirty_.store (true, std::memory_order_release);
}

void SynthEngine::applyTempo (double bpm)
{
    for (auto* av : voicePool_)
        av->setTempo (bpm);
}

void SynthEngine::setVcaExponential (bool exponential)
{
    pendingVcaExp_.store (exponential, std::memory_order_relaxed);
    optionsDirty_.store (true, std::memory_order_release);
}

void SynthEngine::setParameterSmoothing (bool smoothing)
{
    pendingSmoothing_.store (smoothing, std::memory_order_relaxed);
    optionsDirty_.store (true, std::memory_order_release);
}

void SynthEngine::setArpMode (uint8_t mode)
{
    // Stage only; the audio thread applies (servicePendingConfig in
    // processTransport) so the live arp/seq objects + the active->inactive
    // transition (arp.stop() + killGeneratedNotes_) never race the clock loop.
    parts_[(size_t) currentPart_].writePendingConfig ([mode] (auto& c) { c.arpMode = mode; });
    parts_[(size_t) currentPart_].configDirty_.store (true, std::memory_order_release);
}

void SynthEngine::stageArpSeqFromPartBytes (int part)
{
    // Message-thread entry point for FILE LOADS (.MUL / .parvati multi): stage
    // the full arp/seq config from a Part's PartData into pendingConfig_ + flag
    // configDirty_, exactly like the live setters do. The audio thread is the
    // sole writer of the live Arpeggiator/Sequencer objects (it services
    // configDirty_), so a write here would race the clock loop. Staging also
    // keeps pendingConfig_ (the serialize / loadPartIntoApvts source) in sync
    // with the loaded values. Reads parts_[(size_t) part].partBytes (atomic).
    if (! ok (part))
        return;
    auto& p  = parts_[(size_t) part];
    // Stage under the pendingConfig_ seqlock so the audio-thread reader
    // (servicePendingConfig) never sees a torn snapshot.
    p.writePendingConfig ([&] (Part::PendingConfig& pc) {
        // CLAMP to the firmware parameter ranges (ParameterLayout): these
        // bytes arrive RAW from .MUL loads / host-state blobs (never through
        // the APVTS, which enforces its own ranges). An out-of-range mode
        // byte makes isActive() true while isEnabled() stays false — the
        // part absorbs every note into the held-key stack and produces NO
        // sound. An arpOctave of 0 with direction Random never ends the
        // Random branch's octave wrap loop (Arpeggiator.cpp) — ON THE
        // AUDIO THREAD (host hang). The firmware's own bytes are always
        // legal (its UI/parameter layer clamps); raw-file loaders must do
        // the same here.
        pc.arpMode       = static_cast<uint8_t> (juce::jlimit (0, 2,  (int) p.partBytes[7]));   // ArpMode: Off/Arp/Sequencer
        pc.arpDirection  = static_cast<uint8_t> (juce::jlimit (0, 5,  (int) p.partBytes[8]));   // ArpDirection: Up..Chord
        pc.arpOctave     = static_cast<uint8_t> (juce::jlimit (1, 4,  (int) p.partBytes[9]));   // >= 1 (0 hung the Random wrap loop)
        pc.arpPattern    = static_cast<uint8_t> (juce::jlimit (0, 21, (int) p.partBytes[10]));  // kArpPatterns: 22 entries
        pc.arpResolution = static_cast<uint8_t> (juce::jlimit (0, 14, (int) p.partBytes[11]));  // kMidiClockTickPerStep: 15 entries
        pc.seqLength[0]  = static_cast<uint8_t> (juce::jlimit (1, 16, (int) p.partBytes[12]));
        pc.seqLength[1]  = static_cast<uint8_t> (juce::jlimit (1, 16, (int) p.partBytes[13]));
        pc.seqLength[2]  = static_cast<uint8_t> (juce::jlimit (1, 16, (int) p.partBytes[14]));
        for (size_t i = 0; i < 64; ++i)
            pc.seqData[i] = p.partBytes[16 + i];
    });
    p.configDirty_.store (true, std::memory_order_release);
}

//==========================================================================
// Per-part FX MT setters (message thread; called by applyFxParameter). Each
// writes the CURRENT Part's fxState atomics + sets fxDirty_. The relaxed stores
// are published as a frame by the fxDirty_ release-store; the audio thread
// services fxDirty_ in renderPartFx (acq_rel exchange) and pushes the values
// into fxChains_[p] single-threaded. EXACTLY the frameDirty_ / optionsDirty_
// staging pattern. slot/idx are range-clamped; values stored verbatim (the AT
// normalizes 0..127 / -63..63 to 0..1 floats when servicing the chain).
#define PARVATI_FX_CURRENT_PART() parts_[(size_t) currentPart_]
void SynthEngine::setFxSlotType    (int slot, uint8_t v)
{
    if (slot >= 0 && slot < kNumFxSlots)
    {
        PARVATI_FX_CURRENT_PART().fxState.slotType[(size_t) slot].store (v, std::memory_order_relaxed);
        // Audit F1: pre-build + stage the replacement processor HERE (message
        // thread) so the audio thread installs it with pointer moves only
        // (FxChain::servicePendingTypeSwaps in renderPartFx). The factory /
        // prepare allocations used to run inside processBlock.
        fxChains_[(size_t) currentPart_].setSlotType (slot, static_cast<FxType> (v));
        PARVATI_FX_CURRENT_PART().fxState.fxDirty_.store (true, std::memory_order_release);
    }
}
void SynthEngine::setFxSlotEnabled (int slot, uint8_t v)
{
    if (slot >= 0 && slot < kNumFxSlots) { PARVATI_FX_CURRENT_PART().fxState.slotEnabled[(size_t) slot].store (v != 0 ? 1 : 0, std::memory_order_relaxed); PARVATI_FX_CURRENT_PART().fxState.fxDirty_.store (true, std::memory_order_release); }
}
void SynthEngine::setFxSlotDryWet  (int slot, uint8_t v)
{
    if (slot >= 0 && slot < kNumFxSlots) { PARVATI_FX_CURRENT_PART().fxState.slotDryWet[(size_t) slot].store (v, std::memory_order_relaxed); PARVATI_FX_CURRENT_PART().fxState.fxDirty_.store (true, std::memory_order_release); }
}
void SynthEngine::setFxSlotParam   (int slot, int idx, uint8_t v)
{
    if (slot >= 0 && slot < kNumFxSlots && idx >= 0 && idx < kNumFxSlotParams)
    { PARVATI_FX_CURRENT_PART().fxState.slotParam[(size_t) slot][(size_t) idx].store (v, std::memory_order_relaxed); PARVATI_FX_CURRENT_PART().fxState.fxDirty_.store (true, std::memory_order_release); }
}
void SynthEngine::setFxTopology    (uint8_t v)
{
    PARVATI_FX_CURRENT_PART().fxState.topology.store (v, std::memory_order_relaxed);
    PARVATI_FX_CURRENT_PART().fxState.fxDirty_.store (true, std::memory_order_release);
}
void SynthEngine::setFxOrder       (uint8_t v)
{
    PARVATI_FX_CURRENT_PART().fxState.orderIdx.store (v, std::memory_order_relaxed);
    PARVATI_FX_CURRENT_PART().fxState.fxDirty_.store (true, std::memory_order_release);
}
// Master-section setters (engine-state v3). Mirror the topology/order pattern:
// relaxed store into the CURRENT Part's fxState + a release-store on fxDirty_ to
// publish the frame to the audio thread. Consumed in renderPartFx's fxDirty_
// service (chain.setMasterMix / setMasterEqLow / Mid / High).
void SynthEngine::setFxMix       (uint8_t v) { PARVATI_FX_CURRENT_PART().fxState.mix.store (v, std::memory_order_relaxed); PARVATI_FX_CURRENT_PART().fxState.fxDirty_.store (true, std::memory_order_release); }
void SynthEngine::setFxEqLow     (uint8_t v) { PARVATI_FX_CURRENT_PART().fxState.eqLow.store (v, std::memory_order_relaxed); PARVATI_FX_CURRENT_PART().fxState.fxDirty_.store (true, std::memory_order_release); }
void SynthEngine::setFxEqMid     (uint8_t v) { PARVATI_FX_CURRENT_PART().fxState.eqMid.store (v, std::memory_order_relaxed); PARVATI_FX_CURRENT_PART().fxState.fxDirty_.store (true, std::memory_order_release); }
void SynthEngine::setFxEqHigh    (uint8_t v) { PARVATI_FX_CURRENT_PART().fxState.eqHigh.store (v, std::memory_order_relaxed); PARVATI_FX_CURRENT_PART().fxState.fxDirty_.store (true, std::memory_order_release); }
void SynthEngine::setFxModSlot     (int slot, uint8_t src, uint8_t dest, int8_t amount)
{
    if (slot < 0 || slot >= kNumFxMatrixSlots) return;
    auto& st = PARVATI_FX_CURRENT_PART().fxState;
    st.modSource[(size_t) slot].store (src,    std::memory_order_relaxed);
    st.modDest  [(size_t) slot].store (dest,   std::memory_order_relaxed);
    st.modAmount[(size_t) slot].store (amount, std::memory_order_relaxed);
    st.fxDirty_.store (true, std::memory_order_release);
}
#undef PARVATI_FX_CURRENT_PART

void SynthEngine::resetPartFx (int part)
{
    // Reset every field of this Part's fxState to the clean defaults (see
    // PartFxState::storeCleanDefaults). Relaxed stores published as one frame
    // by the fxDirty_ release-store.
    if (part < 0 || part >= kNumParts)
        return;
    auto& fx = parts_[(size_t) part].fxState;
    fx.storeCleanDefaults();
    // Audit F1: stage the slot-type resets on the message thread too (the AT
    // installs them with pointer moves only) -- the chain, not just the atomic
    // state, must return to all-None.
    for (int s = 0; s < kNumFxSlots; ++s)
        fxChains_[(size_t) part].setSlotType (s, FxType::None);
    fx.fxDirty_.store (true, std::memory_order_release);
}

void SynthEngine::stagePartFxSlotType (int part, int slot, int type)
{
    // Loader twin of setFxSlotType for an explicit part (see header). The
    // message-thread chain staging below is the essential half: the AT's
    // fxDirty_ service pushes enabled/drywet/params/topology but deliberately
    // does NOT install slot types (they need a pre-built processor; audit F1),
    // so the fxState atomic alone never changed the sound.
    if (part < 0 || part >= kNumParts || slot < 0 || slot >= kNumFxSlots)
        return;
    auto& fx = parts_[(size_t) part].fxState;
    fx.slotType[(size_t) slot].store ((uint8_t) type, std::memory_order_relaxed);
    fxChains_[(size_t) part].setSlotType (slot, static_cast<FxType> (type));
    fx.fxDirty_.store (true, std::memory_order_release);
}

void SynthEngine::reapRetiredAudioObjects()
{
    // Message thread (the processor's 60 Hz DeferredParamTimer): free every
    // object the audio thread parked for retirement -- the FX processors
    // displaced by a staged type swap (audit F1) and the per-voice Oversampling
    // objects displaced by a staged filter-oversampling change (audit F3).
    // With operator delete kept here, the audio thread's steady state and
    // change paths are pointer moves only.
    for (int p = 0; p < kNumParts; ++p)
        fxChains_[(size_t) p].reapRetired();
    for (auto* av : voicePool_)
        av->reapRetired();
}

//==========================================================================
// Host plugin-state capture/restore (full 6-Part multitimbral persistence).
static const char kEngineStateMagic[4] = { 'P','V','S','T' };

void SynthEngine::captureState (juce::MemoryBlock& dest) const
{
    // Byte-oriented payload (endian-independent): magic + version + current
    // part, then per Part: patch[112], part[84] (with the arp/seq region overlaid
    // from the authoritative pendingConfig_), midi channel / keyzone / voice
    // allocation, then a length-prefixed FX block (Parvati-exclusive; version 2),
    // voice slots + name (version 6). polyphony rides in
    // partBytes[15]. arp/seq lives in pendingConfig_ (overlaid here) and is
    // re-staged on restore. The length prefixes are for forward-safety (a
    // future version may grow the blocks without re-versioning).
    //
    // Version history: v8 REMOVED the per-part tuning block (the custom-table
    // extension was removed 2026-08-19 — the raga preset rides partBytes[4],
    // which the core payload already carries, so no tuning block is needed).
    // v7 carried a length-prefixed {u8 resolvedMode; i16 offsets[12]} block.
    // restoreState still ACCEPTS v7 blobs (parses + ignores the tuning block —
    // a v7 custom mode 33 loads as 12-EDO, its raga byte was kept 0 by the
    // custom-active invariant). The version bump means legacy (pre-2026-08-19)
    // Parvati builds reject the v8 blob and fall back to legacy APVTS restore
    // — the same accepted tradeoff as v5->v6 (documented in CHANGELOG).
    //
    // FX block layout (fixed, 78 bytes): slotType[3], slotEnabled[3],
    // slotDryWet[3], slotParam[3][5], topology, orderIdx, modSource[16],
    // modDest[16], modAmount[16]. Length-prefixed (4 bytes LE) for forward-safety
    // (a future version may grow the block without re-versioning). v5 grew the
    // per-slot param count 4->5 (78 bytes, was 75 in v4).
    constexpr uint32_t kFxBlobLen = (uint32_t) (kNumFxSlots * 3            // type+enabled+drywet
                                              + kNumFxSlots * kNumFxSlotParams   // slot params
                                              + 2                              // topology + orderIdx
                                              + kNumFxMatrixSlots * 3          // src+dst+amount
                                              + 4);                            // master section (v3): mix + eqLow/mid/high
    juce::MemoryOutputStream out (dest, false);
    out.write (kEngineStateMagic, 4);
    out.writeByte (8);                                                       // version (8 = tuning block removed; v7 = per-part tuning block; v6 = voiceSlots + name; v5 = per-slot 5th param; v4 = per-part FX + master section)
    out.writeByte ((char) currentPart_);
    for (int p = 0; p < kNumParts; ++p)
    {
        const auto& part = parts_[(size_t) p];
        std::array<uint8_t, 112> patch {};   part.patchBytes.copyTo (patch);  out.write (patch.data(), 112);
        std::array<uint8_t, 84>  pb   {};   part.partBytes.copyTo (pb);
        const auto pc = part.readPendingConfig();   // overlay authoritative arp/seq (seqlock-protected read)
        pb[7]  = pc.arpMode;  pb[8]  = pc.arpDirection;  pb[9]  = pc.arpOctave;
        pb[10] = pc.arpPattern; pb[11] = pc.arpResolution;
        pb[12] = pc.seqLength[0]; pb[13] = pc.seqLength[1]; pb[14] = pc.seqLength[2];
        for (size_t i = 0; i < 64; ++i) pb[16 + i] = pc.seqData[i];
        out.write (pb.data(), 84);
        out.writeByte ((char) part.midiChannel.load (std::memory_order_relaxed));
        out.writeByte ((char) part.keyrangeLow.load (std::memory_order_relaxed));
        out.writeByte ((char) part.keyrangeHigh.load (std::memory_order_relaxed));
        out.writeByte ((char) part.voiceAllocation.load (std::memory_order_relaxed));

        // Parvati-exclusive per-part FX state (version 2). Length prefix first
        // (4 bytes, little-endian), then the fixed-layout FX bytes above.
        const auto& fx = part.fxState;
        out.writeByte ((char) (kFxBlobLen        & 0xFF));
        out.writeByte ((char) ((kFxBlobLen >> 8)  & 0xFF));
        out.writeByte ((char) ((kFxBlobLen >> 16) & 0xFF));
        out.writeByte ((char) ((kFxBlobLen >> 24) & 0xFF));
        for (int s = 0; s < kNumFxSlots; ++s) out.writeByte ((char) fx.slotType   [(size_t) s].load (std::memory_order_relaxed));
        for (int s = 0; s < kNumFxSlots; ++s) out.writeByte ((char) fx.slotEnabled[(size_t) s].load (std::memory_order_relaxed));
        for (int s = 0; s < kNumFxSlots; ++s) out.writeByte ((char) fx.slotDryWet [(size_t) s].load (std::memory_order_relaxed));
        for (int s = 0; s < kNumFxSlots; ++s)
            for (int k = 0; k < kNumFxSlotParams; ++k)
                out.writeByte ((char) fx.slotParam[(size_t) s][(size_t) k].load (std::memory_order_relaxed));
        out.writeByte ((char) fx.topology.load (std::memory_order_relaxed));
        out.writeByte ((char) fx.orderIdx.load  (std::memory_order_relaxed));
        for (int m = 0; m < kNumFxMatrixSlots; ++m) out.writeByte ((char) fx.modSource[(size_t) m].load (std::memory_order_relaxed));
        for (int m = 0; m < kNumFxMatrixSlots; ++m) out.writeByte ((char) fx.modDest  [(size_t) m].load (std::memory_order_relaxed));
        for (int m = 0; m < kNumFxMatrixSlots; ++m) out.writeByte ((char) fx.modAmount[(size_t) m].load (std::memory_order_relaxed));
        // Master section (v3): global wet/dry + 3-band master EQ.
        out.writeByte ((char) fx.mix.load (std::memory_order_relaxed));
        out.writeByte ((char) fx.eqLow.load (std::memory_order_relaxed));
        out.writeByte ((char) fx.eqMid.load (std::memory_order_relaxed));
        out.writeByte ((char) fx.eqHigh.load (std::memory_order_relaxed));

        // Per-part voice slots + name (version 6, Parvati extension).
        // slots: the Part's voice count, 1..kMaxVoicesPerPart (0 = disabled
        // Part; on restore a 0 falls back to the bitmask popcount so
        // pre-conversion AUTO sessions restore their faithful card counts).
        // name: length-prefixed UTF-8 bytes (max 16 chars).
        out.writeByte ((char) part.voiceSlots.load (std::memory_order_relaxed));
        const juce::String pn = part.name;
        const size_t pnLen = pn.getNumBytesAsUTF8();
        out.writeByte ((char) pnLen);
        if (pnLen > 0)
            out.write (pn.toRawUTF8(), pnLen);
    }
    out.flush ();
}

bool SynthEngine::restoreState (const void* data, size_t size)
{
    if (data == nullptr || size < 6)   // magic(4)+version(1)+currentpart(1)
        return false;
    juce::MemoryInputStream in (data, size, false);
    char magic[4];
    if (in.read (magic, 4) != 4 || std::memcmp (magic, kEngineStateMagic, 4) != 0)
        return false;
    const int version = in.readByte();
    if (version < 1 || version > 8)   // strict-reject unknown versions (caller falls back to legacy APVTS restore)
        return false;
    const int savedCurrent = in.readByte();

    // ---- PHASE 1: parse the whole blob into local snapshots (NO mutation) ----
    // A truncated/corrupt blob previously mutated parts_[p] as it parsed and
    // returned false MID-WAY, leaving a half-restored engine (some Parts from
    // the blob, the rest the previous session). The caller's legacy fallback
    // then layered the APVTS on top of that. Every failure return below
    // happens BEFORE any engine state is touched, so a rejected blob leaves
    // the engine exactly as it was.
    struct RestoredPart
    {
        std::array<uint8_t, 112> patch {};
        std::array<uint8_t, 84>  pb    {};
        uint8_t channel = 0, krLo = 0, krHi = 0, mask = 0;
        bool hasFx = false;                       // version >= 2 FX block present
        uint32_t fxLen = 0;
        juce::HeapBlock<uint8_t> fxBlob;          // RAW fx bytes; decoded in phase 2
        bool hasSlotsName = false;                // version >= 6 slots+name tail present
        uint8_t slots = 0;
        juce::String name;
    };
    std::array<RestoredPart, kNumParts> snap;

    for (int p = 0; p < kNumParts; ++p)
    {
        auto& s = snap[(size_t) p];
        if (in.read (s.patch.data(), 112) != 112) return false;
        if (in.read (s.pb.data(), 84) != 84) return false;
        s.channel = (uint8_t) in.readByte();
        s.krLo    = (uint8_t) in.readByte();
        s.krHi    = (uint8_t) in.readByte();
        // The blob's bitmask is only a LEGACY seed under the slots model (the
        // live mask is re-derived from the slots on the next rebuild); it is
        // kept here (a) to consume the byte and (b) to materialize real slot
        // counts for pre-v6 blobs / v6 AUTO saves in phase 2.
        s.mask = (uint8_t) in.readByte();

        if (version >= 2)
        {
            // Parvati-exclusive per-part FX state: length-prefixed (4 bytes LE)
            // then the fixed-layout FX bytes. A larger length is forward-compat
            // (trailing bytes skipped); a short/truncated read is rejected like
            // the core payload above. Absent in v1 -> hasFx stays false.
            uint8_t lenBytes[4];
            if (in.read (lenBytes, 4) != 4) return false;
            s.fxLen = (uint32_t) lenBytes[0]
                    | ((uint32_t) lenBytes[1] << 8)
                    | ((uint32_t) lenBytes[2] << 16)
                    | ((uint32_t) lenBytes[3] << 24);
            if (in.getNumBytesRemaining() < (juce::int64) s.fxLen) return false;   // truncated
            s.fxBlob.calloc (s.fxLen > 0 ? s.fxLen : 1);
            if (in.read (s.fxBlob, (int) s.fxLen) != (int) s.fxLen) return false;
            s.hasFx = true;
        }

        if (version >= 6)
        {
            s.slots = (uint8_t) in.readByte();
            const int nameLen = (uint8_t) in.readByte();
            if (nameLen > 0)
            {
                juce::HeapBlock<char> nb (nameLen + 1, true);
                if (in.read (nb, nameLen) != nameLen) return false;
                // Same sanitize as setPartName (16-char cap + control-char
                // strip): a corrupt or hand-edited state blob could otherwise
                // carry a newline into a name that a later .parvati save would
                // corrupt. Done here (phase 1) so the commit stays a pure copy.
                s.name = sanitizePartName (juce::String::fromUTF8 (nb, nameLen));
            }
            s.hasSlotsName = true;
        }

        if (version == 7)
        {
            // v7 tuning block (removed in v8): length-prefixed
            // {u8 resolvedMode; i16 LE offsets[12]}. PARSED AND IGNORED — the
            // raga preset rides partBytes[4] (already restored above), and the
            // former custom mode (33) is dropped: it loaded a table this
            // version has no storage for, and its raga byte was 0 by the
            // custom-active invariant, so those parts restore as 12-EDO.
            // Still size-checked so a truncated/foreign blob is REJECTED like
            // every other block, never mis-parsed.
            uint8_t tuneLenBytes[4];
            if (in.read (tuneLenBytes, 4) != 4) return false;
            const uint32_t tuneLen = (uint32_t) tuneLenBytes[0]
                                   | ((uint32_t) tuneLenBytes[1] << 8)
                                   | ((uint32_t) tuneLenBytes[2] << 16)
                                   | ((uint32_t) tuneLenBytes[3] << 24);
            if (tuneLen < 25 || in.getNumBytesRemaining() < (juce::int64) tuneLen)
                return false;   // truncated / foreign layout
            in.skipNextBytes ((juce::int64) tuneLen);
        }
    }

    // ---- PHASE 2: commit — the blob parsed completely, apply the snapshots ----
    // (The decode rules below are byte-for-byte the previous single-pass
    // body, reading from the snapshot instead of the stream.)
    for (int p = 0; p < kNumParts; ++p)
    {
        auto& part = parts_[(size_t) p];
        const auto& s = snap[(size_t) p];
        // SANITIZE AT INGESTION (memory-safety wave 2026-08-22): the host blob
        // is untrusted — normalize every patch/part byte to its firmware domain
        // BEFORE it lands in the engine arrays, so all sinks (known AND
        // undiscovered) receive in-domain values. Identity for legit states
        // (our own saves are in-domain by construction); the DSP-side sink
        // clamps stay as defense in depth.
        {
            ambika::dsp::Patch patchView;
            std::memcpy (&patchView, s.patch.data(), sizeof (ambika::dsp::Patch));
            ambika::dsp::sanitizePatch (patchView);
            part.patchBytes.loadFrom (reinterpret_cast<const uint8_t*> (&patchView));
        }
        {
            std::array<uint8_t, 84> pb = s.pb;
            ambika::dsp::sanitizePartData (pb);
            part.partBytes.loadFrom (pb.data());
        }
        stageArpSeqFromPartBytes (p);   // re-stage arp/seq from the restored PartData
        part.midiChannel.store (s.channel);
        part.keyrangeLow.store  (s.krLo);
        part.keyrangeHigh.store (s.krHi);
        part.voiceAllocation.store (s.mask);

        if (s.hasFx)
        {
            auto& fx = part.fxState;
            // Clean slate first: a blob overwrites the fields it carries; a
            // legacy blob that lacks a field (param5 before v5, the master
            // section before v3) loads that field at its clean default, not a
            // stale value from the pre-restore state.
            fx.storeCleanDefaults();
            const uint8_t* fxBlob = s.fxBlob.get();
            const uint32_t fxLen = s.fxLen;
            size_t o = 0;
            const auto take = [&] () -> uint8_t { return (o < fxLen) ? fxBlob[o++] : 0; };
            for (int sl = 0; sl < kNumFxSlots; ++sl) fx.slotType   [(size_t) sl].store (take(), std::memory_order_relaxed);
            for (int sl = 0; sl < kNumFxSlots; ++sl) fx.slotEnabled[(size_t) sl].store (take(), std::memory_order_relaxed);
            for (int sl = 0; sl < kNumFxSlots; ++sl) fx.slotDryWet [(size_t) sl].store (take(), std::memory_order_relaxed);
            // Per-slot param count is version-dependent: v5+ carries 5 params
            // per slot (param1..5); v1..v4 carry only 4 (param1..4). Reading the
            // wrong count would shift every subsequent byte (topology/order/mods),
            // so gate on the version. param5 stays at its clean default for a
            // legacy blob (storeCleanDefaults above zeroed it).
            const int nParams = (version >= 5) ? kNumFxSlotParams : 4;
            for (int sl = 0; sl < kNumFxSlots; ++sl)
                for (int k = 0; k < nParams; ++k)
                    fx.slotParam[(size_t) sl][(size_t) k].store (take(), std::memory_order_relaxed);
            fx.topology.store (take(), std::memory_order_relaxed);
            fx.orderIdx.store  (take(), std::memory_order_relaxed);
            for (int m = 0; m < kNumFxMatrixSlots; ++m) fx.modSource[(size_t) m].store (take(), std::memory_order_relaxed);
            for (int m = 0; m < kNumFxMatrixSlots; ++m) fx.modDest  [(size_t) m].store (take(), std::memory_order_relaxed);
            for (int m = 0; m < kNumFxMatrixSlots; ++m) fx.modAmount[(size_t) m].store ((int8_t) take(), std::memory_order_relaxed);
            // Master section (v3): the clean slate above left the defaults in
            // place; a v3+ save overwrites them next.
            if (version >= 3)
            {
                fx.mix.store (take(), std::memory_order_relaxed);
                // A v3 blob carries a legacy keepTails byte here (now always-on);
                // discard it so the remaining eqLow/mid/high read correctly.
                // A v4 blob has NO keepTails byte (it was removed), so do not
                // consume one in that case.
                if (version == 3) (void) take();
                fx.eqLow.store (take(), std::memory_order_relaxed);
                fx.eqMid.store (take(), std::memory_order_relaxed);
                fx.eqHigh.store (take(), std::memory_order_relaxed);
            }
            fx.fxDirty_.store (true, std::memory_order_release);
            // Audit F1: stage the restored slot types on the message thread
            // (pre-build + prepare here; the AT installs with pointer moves
            // only). Staged BEFORE the next prepare/render, so a restore that
            // lands before prepareToPlay is re-prepared at the real block size
            // by FxChain::prepare.
            for (int sl = 0; sl < kNumFxSlots; ++sl)
                fxChains_[(size_t) p].setSlotType (
                    sl, static_cast<FxType> (fx.slotType[(size_t) sl].load (std::memory_order_relaxed)));
        }

        // Per-part voice slots + name. Slots are the single source of truth
        // under the slots model: a v1..v5 blob (or a v6 save with a 0 = legacy
        // AUTO byte) materializes its real count from the blob bitmask —
        // popcount(mask), 0 -> disabled — so legacy sessions restore with
        // their faithful card counts. Absent in v1..v5 -> empty name ("Part N").
        if (s.hasSlotsName)
        {
            const int restoredSlots = parvati::popcount8 (s.mask);
            // Clamp like the public setPartVoiceSlots (bug hunt 2026-08-18,
            // F-state-5): s.slots is a raw blob byte (no upstream validation;
            // restoredSlots' popcount is inherently <= 8 but the explicit
            // file byte is not), and every nonzero voiceSlots consumer assumes
            // 1..kMaxVoicesPerPart. A hostile/corrupt blob (e.g. 200) must
            // clamp to 16. A 0 (the legacy AUTO byte / a mask-empty disabled
            // part) must stay 0: forcing it to 1 would steal a card and shift
            // every later part's contiguous share (host_state_test [1] pins
            // exactly that).
            const int rawSlots = s.slots != 0 ? (int) s.slots : restoredSlots;
            part.voiceSlots.store (
                static_cast<uint8_t> (rawSlots == 0 ? 0 : juce::jlimit (1, kMaxVoicesPerPart, rawSlots)),
                std::memory_order_relaxed);
            part.name = s.name;
        }
        else
        {
            part.voiceSlots.store (
                static_cast<uint8_t> (parvati::popcount8 (s.mask)),
                std::memory_order_relaxed);
            part.name = juce::String();
        }
    }
    setCurrentPart (juce::jlimit (0, kNumParts - 1, savedCurrent));
    resetAllVoices();        // clean slate for the restored config (deferred to AT)
    markAllocationDirty();   // AT rebuilds voiceIndices + pushes every Part's frame
    // The restore rewrote Patch-page-mirrored state directly (not through the
    // public mutators), so a VISIBLE Patch page must re-read on the next poll
    // (see getDisplayVersion). setStateInformation may arrive off the message
    // thread on some hosts; the version store is atomic either way.
    bumpDisplayVersion();
    return true;
}

void SynthEngine::setCurrentPart (int part)
{
    if (ok (part))
        currentPart_ = part;
}

//==========================================================================
//==========================================================================
// Voice allocation: the firmware 6-voicecard bitmask + the Parvati voice-slot
// extension.
namespace
{
    // Index of the n-th set bit of @p mask (n 0-based). Mask must be non-zero.
    int nthSetBit (uint8_t mask, int n)
    {
        for (int vc = 0, seen = 0; vc < 8; ++vc)
            if (mask & (1u << vc))
            {
                if (seen == n) return vc;
                ++seen;
            }
        return 0;   // defensive (n >= popcount never occurs at the call sites)
    }
}

void SynthEngine::rebuildVoiceAllocation()
{
    // The SLOTS MODEL: each Part's voiceSlots setting (1..kMaxVoicesPerPart;
    // 0 = disabled, only ever set by the ctor default / legacy loaders) is the
    // SINGLE SOURCE OF TRUTH for polyphony — 1 voice = digital voice section
    // + voicecard. The engine partitions its fixed pool of kNumVoices (96)
    // voices in Part order from those counts. Because the pool =
    // kNumParts * kMaxVoicesPerPart, every Part can sit at its maximum
    // simultaneously — the partition never runs short (the jmin clamp below is
    // pure defense).
    //
    // The firmware 6-voicecard bitmask is DERIVED (no longer user state):
    // mul_export::deriveMasks gives each ACTIVE Part a contiguous
    // proportional (largest-remainder) share of the 6 cards, minimum one card
    // — the exact allocation shape the tested .MUL solver produces, so the
    // engine and the export strategies cannot drift. The derived masks keep
    // their two live jobs: aux-out routing (a Part's pool voices are tagged
    // ROUND-ROBIN across its cards, so its audio still appears on the
    // individual voicecard outputs) and the .MUL "AsIs" export baseline
    // (published into Part::voiceAllocation below, where getPartVoiceAllocation
    // / the FX-input partCardMask_ read it).
    std::array<int, kNumParts> want {};
    for (int p = 0; p < kNumParts; ++p)
        want[(size_t) p] = static_cast<int> (parts_[(size_t) p].voiceSlots.load (std::memory_order_relaxed));
    const std::array<uint8_t, kNumParts> partCards = parvati::mul_export::deriveMasks (want);

    int nextVoice = 0;                    // next free pool index

    // Voices owned by ANY Part before this rebuild. A voice that DROPS OUT of
    // its Part's set (fewer slots) never receives a noteOff from that Part
    // again — release it with a graceful tail-off below so it rings out and
    // frees itself instead of sustaining forever. (A hard kill is wrong here:
    // Kill + idle would leave the voice silent on its next trigger — the
    // standalone dead-voice glitch.)
    bool wasOwned[kNumVoices] = {};
    for (int p = 0; p < kNumParts; ++p)
        for (int vi : parts_[(size_t) p].voiceIndices)
            if (vi >= 0 && vi < kNumVoices)
                wasOwned[vi] = true;

    for (int p = 0; p < kNumParts; ++p)
    {
        auto& part = parts_[(size_t) p];
        part.voiceIndices.clear();

        if (want[(size_t) p] <= 0 || partCards[(size_t) p] == 0)
            continue;   // disabled Part (0 slots): no voices

        const int cards = parvati::popcount8 (partCards[(size_t) p]);
        const int n = juce::jmin (want[(size_t) p], kNumVoices - nextVoice);
        for (int k = 0; k < n; ++k)
        {
            const int vi = nextVoice++;
            part.voiceIndices.push_back (vi);
            if (auto* av = getAmbikaVoice (vi))
                av->setVoiceCard (nthSetBit (partCards[(size_t) p], k % cards));
        }
    }
    // CHAIN (Option A): a CHAIN Part doubles its voice set with partner
    // slots from the REMAINING pool (the plugin equivalent of the firmware's
    // 2-unit chain, 2N voices via midi_dispatcher.ForwardNote,
    // midi_dispatcher.h:228, but fully internal — no MIDI output). Base claims
    // (above) are honoured first; if the pool runs low the Part gets a partial
    // chain (graceful). Partner voices are tagged onto the Part's own cards.
    for (int p = 0; p < kNumParts; ++p)
    {
        if (parts_[(size_t) p].polyphonyMode != 4 /*CHAIN*/) continue;
        const int baseN = static_cast<int> (parts_[(size_t) p].voiceIndices.size());
        if (baseN == 0 || partCards[(size_t) p] == 0) continue;
        const int cards = parvati::popcount8 (partCards[(size_t) p]);
        const int n = juce::jmin (baseN, kNumVoices - nextVoice);
        for (int k = 0; k < n; ++k)
        {
            const int vi = nextVoice++;
            parts_[(size_t) p].voiceIndices.push_back (vi);
            if (auto* av = getAmbikaVoice (vi))
                av->setVoiceCard (nthSetBit (partCards[(size_t) p], k % cards));
        }
    }
    // Release voices that lost their slot in this rebuild (see wasOwned above).
    {
        bool isOwned[kNumVoices] = {};
        for (int p = 0; p < kNumParts; ++p)
            for (int vi : parts_[(size_t) p].voiceIndices)
                if (vi >= 0 && vi < kNumVoices)
                    isOwned[vi] = true;
        for (int vi = 0; vi < kNumVoices; ++vi)
            if (wasOwned[vi] && ! isOwned[vi])
                if (auto* av = getAmbikaVoice (vi))
                    stopVoice (av, 1.0f, true);   // tail-off (never Kill)
    }

    // Re-tag each voice with its (possibly changed) Part.
    for (int p = 0; p < kNumParts; ++p)
        for (int vi : parts_[(size_t) p].voiceIndices)
            if (auto* av = getAmbikaVoice (vi))
                av->setPartIndex (p);

    // Voice sets changed -> re-arm every Part's allocator for its mode.
    for (int p = 0; p < kNumParts; ++p)
    {
        initAllocator (parts_[(size_t) p]);
        parts_[(size_t) p].voiceCount_.store (static_cast<int> (parts_[(size_t) p].voiceIndices.size()), std::memory_order_relaxed);
    }

    // Persist each Part's resolved card bitmask for renderPartFx's per-part FX
    // input sum (see partCardMask_): sum the OWNED card buffers, not the
    // buffers indexed by pool voice indices (which only coincide in the default
    // single-part layout). The DERIVED mask is also published into
    // Part::voiceAllocation so message-thread readers (getPartVoiceAllocation:
    // .MUL export + the Patch page) see the allocation the audio thread
    // actually tagged with — the same published-on-rebuild freshness as
    // voiceCount_ (a card/slot edit settles on the next process block).
    for (int p = 0; p < kNumParts; ++p)
    {
        partCardMask_[(size_t) p] = partCards[(size_t) p];
        parts_[(size_t) p].voiceAllocation.store (partCards[(size_t) p], std::memory_order_relaxed);
    }
}

void SynthEngine::setPartVoiceAllocation (int part, uint8_t bitmask)
{
    // LEGACY LOAD PATH (bitmask era). The mask is no longer user state —
    // voiceSlots owns polyphony and the mask is DERIVED from it by
    // rebuildVoiceAllocation. Loaders that still carry bitmasks (.MUL files,
    // host-state v1..v5, PatchArrangement presets, older .parvati files)
    // materialize the equivalent slot count here: slots = popcount(mask)
    // (0 -> disabled). Exclusivity is inherent — derived masks are contiguous
    // and disjoint by construction — so the old card-stealing logic is gone.
    if (! ok (part))
        return;

    int slots = parvati::popcount8 (bitmask);
    const uint8_t v = static_cast<uint8_t> (slots);

    if (parts_[(size_t) part].voiceSlots.load (std::memory_order_relaxed) == v)
        return;   // no change -> nothing to defer

    parts_[(size_t) part].voiceSlots.store (v, std::memory_order_relaxed);
    markAllocationDirty();   // defer the rebuild to the audio thread (next block)
    bumpDisplayVersion();    // the Patch page mirrors the slot count (Voices combo)
}

void SynthEngine::setPartVoiceSlots (int part, int slots)
{
    if (! ok (part))
        return;
    // 1..kMaxVoicesPerPart is the user range; 0 (a legacy AUTO value) clamps
    // to 1 so the PUBLIC setter can never disable a Part — disabling is the
    // loaders' job (setPartVoiceAllocation with a zero mask materializes 0).
    const uint8_t v = static_cast<uint8_t> (juce::jlimit (1, kMaxVoicesPerPart, slots));
    if (parts_[(size_t) part].voiceSlots.load (std::memory_order_relaxed) != v)   // only defer on a real change
    {
        parts_[(size_t) part].voiceSlots.store (v, std::memory_order_relaxed);
        markAllocationDirty();   // re-partition the pool on the audio thread
        bumpDisplayVersion();    // the Patch page mirrors the slot count (Voices combo)
    }
}

//==========================================================================
// Per-part microtonal tuning (PartData.raga presets).
int SynthEngine::resolvedTuningMode (int part) const
{
    if (! ok (part))
        return 0;
    // The raga byte IS the resolved mode: 0 = 12-EDO, 1..32 = firmware
    // raga preset (file-faithful). The former custom-table mode 33 was
    // removed with the custom-tuning subsystem (2026-08-19).
    return static_cast<int> (parts_[(size_t) part].partBytes[4]);
}

void SynthEngine::resolveTuningOffsets (int part, int16_t out[12]) const
{
    if (out == nullptr)
        return;
    if (! ok (part))
    {
        for (int c = 0; c < 12; ++c) out[c] = 0;
        return;
    }
    const int mode = resolvedTuningMode (part);
    if (mode >= 1 && mode <= parvati::kNumTuningPresets)
    {
        const int16_t* t = parvati::tuningPresetTable (mode);
        for (int c = 0; c < 12; ++c)
            out[c] = t != nullptr ? t[c] : 0;
        return;
    }
    for (int c = 0; c < 12; ++c) out[c] = 0;   // 12-EDO (mode 0 / unknown)
}

bool SynthEngine::isNoteAcceptedByPartTuning (int part, int rawNote) const
{
    if (! ok (part))
        return true;
    int16_t t[12];
    resolveTuningOffsets (part, t);
    // Firmware Part::AcceptNote (part.cc:649-660): a muted note CLASS is
    // refused outright. Voiced as a refusal (not as firmware TuneNote's
    // 32767-clamped garbage pitch) — deliberate, documented deviation.
    return t[rawNote % 12] != parvati::kTuningSilence;
}

void SynthEngine::pushTuningToVoices (int part)
{
    if (! ok (part))
        return;
    // Audio-thread path: resolve the table once (atomics only) and hand every
    // voice owned by this Part its own copy (AmbikaVoice::tuneOffsets_ is read
    // at the next startNote — sounding voices keep their triggered pitch, the
    // same change-on-new-notes semantics as partOctave_/partTuning_).
    int16_t t[12];
    resolveTuningOffsets (part, t);
    for (auto* av : voicePool_)
    {
        if (av->getPartIndex() != part)
            continue;
        av->setTuningOffsets (t);
    }
}

void SynthEngine::pushPartBytesToVoices (int part)
{
    if (! ok (part))
        return;
    // Audio-thread path (called only from the allocationDirty service): push this
    // Part's stored patch/part bytes into every voice owned by it. Iterate the
    // stable voice pool filtered by partIndex (NOT voiceIndices) so this is
    // robust even if called before a rebuild has settled voiceIndices.
    // (polyphonyMode / voiceAllocation are synced + rebuilt by the service before
    // this is called, so voice ownership is final here.)
    const auto& p = parts_[(size_t) part];
    for (auto* av : voicePool_)
    {
        if (av->getPartIndex() != part)
            continue;
        for (int o = 0; o < 112; ++o)
            av->setPatchByte (o, p.patchBytes[(size_t) o]);
        // Only the 7-byte dsp::Part is voicecard-relevant (volume/octave/tuning/
        // spread/_/legato/portamento at offsets 0..6). The rest of PartData is
        // controller-side (arp/seq/polyphony) and MUST NOT be pushed -- a push
        // of offsets 7..83 was an out-of-bounds write that clobbered the Voice's
        // envelopes/LFOs/oscillators on every patch/mode switch.
        for (int o = 0; o < static_cast<int>(sizeof (ambika::dsp::Part)); ++o)
            av->setPartByte (o, p.partBytes[(size_t) o]);
        // Re-prime the envelope increments from the just-pushed patch: an idle
        // voice is gated out of renderNextBlock. Without this the pushed A/D/S/R
        // bytes leave its attack increment stale/0 and the next note is SILENT
        // (the standalone "dead after a voice-mode / template switch" glitch).
        av->reprimeEnvelopes();
    }
    // A frame push may carry a PartData byte-4 (raga preset) change: resolve
    // and hand the Part's voices the new tuning table in the same pass.
    // Idempotent (per-voice setTuningOffsets is a plain copy).
    pushTuningToVoices (part);
}

void SynthEngine::resetAllVoices()
{
    // DEFER the kill to the audio thread. stopNote(.,false) runs Voice::Kill +
    // clearCurrentNote + FIFO clear on a voice the audio thread may be
    // mid-rendering -- a call here (message thread) raced the audio thread
    // and crashed in hosts (e.g. Ableton). The kill is serviced at the top of
    // the next processTransport() block. The caller has already pushed the new
    // patch bytes + flagged allocationDirty_, whose service rebuilds and
    // re-primes (pushPartBytesToVoices) every voice.
    resetAllVoicesPending_.store (true, std::memory_order_release);
}

//==========================================================================
// MIDI routing (channel + keyzone -> Part), faithful to multi.h PartMapping.
// The predicates (partAcceptsNote / partAcceptsChannel /
// partAcceptsChannelNote) are declared in the header; forEachAcceptingPart is
// a header-inline template over parts_.
bool SynthEngine::partAcceptsNote (const Part& pm, int note)
{
    const int lo = pm.keyrangeLow.load();
    const int hi = pm.keyrangeHigh.load();
    if (lo <= hi)
        return note >= lo && note <= hi;
    return note <= hi || note >= lo;   // wrap-around zone (firmware parity, W8)
}

bool SynthEngine::partAcceptsChannel (const Part& pm, int channel)
{
    const uint8_t ch = pm.midiChannel.load();
    return ch == 0 || channel == ch;
}

bool SynthEngine::partAcceptsChannelNote (const Part& pm, int channel, int note)
{
    return partAcceptsChannel (pm, channel) && partAcceptsNote (pm, note);
}

// First accepting part, or -1. Kept for the (single-part) callers whose
// downstream semantics are inherently per-one (it is now "first match in
// multicast order", not an exclusive route).
int SynthEngine::findPartForNote (int channel, int note) const
{
    for (int p = 0; p < kNumParts; ++p)
        if (partAcceptsChannelNote (parts_[(size_t) p], channel, note))
            return p;
    return -1;
}

void SynthEngine::initAllocator (Part& p)
{
    const int n = static_cast<int> (p.voiceIndices.size());
    if (p.polyphonyMode == 0 /*MONO*/)
    {
        p.monoStack.clear();
    }
    else
    {
        // Firmware Part::InitializeAllocators (part.cc:240): UNISON_2X uses half
        // the voices per note. CHAIN used to double the ALLOCATOR size (with the
        // upper half forwarded out via MIDI); under Option A rebuildVoiceAllocation
        // instead doubles voiceIndices itself, so the allocator spans the full
        // (already-doubled) set exactly like POLY.
        int size = n;
        if (p.polyphonyMode == 2 /*UNISON_2X*/) size = (n + 1) >> 1;
        p.polyAlloc.init (static_cast<uint8_t> (size), p.polyphonyMode == 3 /*CYCLIC*/);
    }
}

// triggerVoice / retriggerVoice — wrap juce::Synthesiser::startVoice and
// AmbikaVoice::retriggerNote to stamp each triggered voice as the most-recently-
// triggered (a monotonic seq), so the FX representative-voice tracker in
// renderPartFx can pick the newest active voice per part. The central stamp
// keeps every trigger site in sync with the tracker without per-call
// boilerplate (and future note-on paths stay correct automatically).
void SynthEngine::triggerVoice (AmbikaVoice* av, juce::SynthesiserSound* sound,
                                int channel, int note, float velocity)
{
    if (av == nullptr) return;
    startVoice (av, sound, channel, note, velocity);
    // Pick up the standing bend of the note's channel (a voice triggered while
    // a wheel is off-centre previously started un-bent until the next wheel
    // event; under MPE / latched wheels that was audible).
    applyStandingBend (av, channel);
    av->setTriggerSeq (nextTriggerSeq());
}

void SynthEngine::retriggerVoice (AmbikaVoice* av, juce::SynthesiserSound* sound,
                                  int channel, int note, float velocity)
{
    if (av == nullptr) return;
    // 2026-08-22: route through startVoice itself (the engine IS the
    // Synthesiser; startVoice is protected-to-us) for FULL truthful JUCE
    // bookkeeping — note, channel, noteOnTime, sound, pedal states — after
    // armRetriggerContinuation() disarms its only harmful step: the
    // pre-emptive stopNote(0,false) -> Voice::Kill that fires when
    // currentlyPlayingSound != nullptr (it zeroes the envelope: the "fast
    // mono note changes cut out" bug). The voice's audio state (dsp voice,
    // resampler FIFO, gains) is untouched by the disarm. startNote then
    // continues the live audio via continuityNext_ and runs the
    // firmware-faithful Trigger (envelope ATTACK from the CURRENT value).
    av->armRetriggerContinuation();
    startVoice (av, sound, channel, note, velocity);
    applyStandingBend (av, channel);   // same pickup as triggerVoice (legato path)
    av->setTriggerSeq (nextTriggerSeq());
}

void SynthEngine::triggerNoteInPart (int part, int note, float velocity, int incomingChannel)
{
    if (! ok (part)) return;
    auto& p = parts_[(size_t) part];
    // Firmware Part::AcceptNote (part.cc:649-660): a tuning table that mutes
    // the note's class (32767 sentinel) refuses the note outright. This head
    // gate covers EVERY trigger source — direct MIDI (via noteOn), arp and
    // sequencer generated notes (their callbacks land here), MONO/POLY paths
    // — and octave-shifted arp notes stay in-class (note % 12 is octave-
    // invariant), so one check suffices.
    if (! isNoteAcceptedByPartTuning (part, note)) return;
    // Tag the voice with its REAL incoming MIDI channel (clamped 1..16) so the
    // per-channel expression routing isolates per-note under MPE. For a non-Omni
    // Part this equals the Part's own channel (findPartForNote matched); for an
    // Omni Part it is the actual note channel; arp/seq notes pass the Part's
    // channel. Standard single-channel MIDI (all on ch1) is unchanged.
    const int channel = juce::jlimit (1, 16, incomingChannel);
    auto* sound = getNumSounds() > 0 ? getSound (0).get() : nullptr;
    const uint8_t n8 = static_cast<uint8_t> (note);

    if (p.polyphonyMode == 0 /*MONO*/)
    {
        // Firmware InternalNoteOn MONO (part.cc:670): trigger ALL allocated
        // voices to the (tuned) note; legato when a key is already held.
        // Per-voice spread detune (part.cc:675): running pitch_drift += spread.
        // Octave/tuning/spread are all applied per-voice in AmbikaVoice::startNote.
        const uint8_t spread = p.partBytes[3];
        p.monoStack.noteOn (n8, static_cast<uint8_t> (juce::jlimit (0, 127, static_cast<int> (velocity * 127.0f))));
        const bool legato = p.monoStack.size() > 1;
        // MONO fires EVERY allocated VOICE (slots model: 1 voice = digital
        // voice + voicecard — the firmware triggers every allocated voicecard;
        // Parvati's unison size is the Part's voice count). MONO + 1 voice is
        // true single-voice mono, MONO + 16 is a 16-voice unison.
        uint8_t drift = 0;   // 14-bit units (1/128 semitone); uint8 wrap is faithful
        for (int vi : p.voiceIndices)
            if (auto* av = getAmbikaVoice (vi))
            {
                av->setLegatoNext (legato);
                av->setSpreadDrift (drift);
                // Overlap on a SOUNDING voice — legato slide OR a mono
                // retrigger of the previous note's RELEASE TAIL (monoStack
                // back to size 1 while the tail still sounds) — re-triggers
                // WITHOUT the kill that startVoice does. juce startVoice's
                // stopNote(0,false) -> Voice::Kill ZEROES the envelope, so
                // the new attack would start from silence (with a 495 ms
                // attack, every fast note change audibly cut out — the
                // 2026-08-22 mono fix). The firmware simply re-Triggers the
                // same voicecard: Envelope::Trigger(ATTACK) seeds its start
                // from the CURRENT value, so the attack rises from the
                // release level, continuously. retriggerNote's
                // continuityNext_ also keeps the resampler FIFO and the
                // output gain flowing (no time-skip click, no de-click
                // hole). legatoNext_ (false in the tail case) selects the
                // full non-legato DSP retrigger — firmware semantics.
                // The first note (voice idle) still uses startVoice for a
                // fresh attack.
                if (av->isVoiceActive())
                    retriggerVoice (av, sound, channel, note, velocity);
                else
                    triggerVoice (av, sound, channel, note, velocity);
                drift += spread;
            }
        return;
    }

    const int idx = p.polyAlloc.noteOn (n8);
    const int n   = static_cast<int> (p.voiceIndices.size());
    if (idx == 0xff) return;   // allocator full

    if (p.polyphonyMode == 2 /*UNISON_2X*/)
    {
        const int v0 = idx * 2;                          // always < n
        const int v1 = (v0 + 1 < n) ? v0 + 1 : 0;        // firmware GetNextVoice wrap
        // Firmware part.cc:703: the pair is detuned by data_.spread (2nd voice +spread).
        const uint8_t spread = p.partBytes[3];
        if (auto* av = getAmbikaVoice (p.voiceIndices[(size_t) v0])) { av->setSpreadDrift (0);      triggerVoice (av, sound, channel, note, velocity); }
        if (auto* av = getAmbikaVoice (p.voiceIndices[(size_t) v1])) { av->setSpreadDrift (spread); triggerVoice (av, sound, channel, note, velocity); }
    }
    else if (p.polyphonyMode == 4 /*CHAIN*/)  // internal 2x doubling (Option A)
    {
        // The firmware forwards idx>=n to a 2nd unit over MIDI (ForwardNote,
        // midi_dispatcher.h:228). Parvati instead gives the Part 2x its voices
        // internally (rebuildVoiceAllocation partner claim), so every allocator
        // index maps to a real same-patch voice. Drift = idx*spread (part.cc:711).
        if (auto* av = getAmbikaVoice (p.voiceIndices[(size_t) idx]))
        {
            av->setSpreadDrift (static_cast<uint8_t> (idx * p.partBytes[3]));
            triggerVoice (av, sound, channel, note, velocity);
        }
    }
    else  // POLY / CYCLIC
    {
        // Firmware part.cc:721: drift = voice_index * spread.
        if (idx < n)
            if (auto* av = getAmbikaVoice (p.voiceIndices[(size_t) idx]))
            {
                av->setSpreadDrift (static_cast<uint8_t> (idx * p.partBytes[3]));
                triggerVoice (av, sound, channel, note, velocity);
            }
    }
}

void SynthEngine::releaseNoteInPart (int part, int note, int incomingChannel)
{
    if (! ok (part)) return;
    auto& p = parts_[(size_t) part];
    const uint8_t n8 = static_cast<uint8_t> (note);

    if (p.polyphonyMode == 0 /*MONO*/)
    {
        // Firmware InternalNoteOff MONO (part.cc:744): pop the note; if no keys
        // remain release all voices, else if the released note was the sounding
        // top, retrigger the new most-recent note (legato) on all voices.
        const uint8_t topNote = (p.monoStack.size() > 0) ? p.monoStack.most_recent_note().note : 0xff;
        p.monoStack.noteOff (n8);
        if (p.monoStack.size() == 0)
        {
            // No keys remain: release every allocated voice (unison size = the
            // Part's voice count, matching the MONO noteOn above).
            for (int vi : p.voiceIndices)
                if (auto* av = getAmbikaVoice (vi))
                    stopVoice (av, 1.0f, true);
        }
        else if (topNote == n8)
        {
            const uint8_t newNote = p.monoStack.most_recent_note().note;
            const float   newVel  = p.monoStack.most_recent_note().velocity / 127.0f;
            const int channel = juce::jlimit (1, 16, incomingChannel);
            auto* sound = getNumSounds() > 0 ? getSound (0).get() : nullptr;
            const uint8_t spread = p.partBytes[3];   // firmware part.cc:760: retrigger drift
            uint8_t drift = 0;
            for (int vi : p.voiceIndices)   // every allocated voice (see MONO noteOn)
                if (auto* av = getAmbikaVoice (vi))
                {
                    av->setLegatoNext (true);
                    av->setSpreadDrift (drift);
                    // Legato slide-back to the prior held note on release: same
                    // no-kill retrigger (a kill would silence the legato Trigger).
                    if (av->isVoiceActive())
                        retriggerVoice (av, sound, channel, newNote, newVel);
                    else
                        triggerVoice (av, sound, channel, newNote, newVel);
                    drift += spread;
                }
        }
        return;
    }

    const int idx = p.polyAlloc.noteOff (n8);
    const int n   = static_cast<int> (p.voiceIndices.size());
    if (idx == 0xff)
    {
        // Defensive fallback: the allocator lost track of this pitch (a slot
        // was stolen + overwritten, or a generated arp/seq note whose mapping
        // was never recorded). Without this a voice can strand in sustain --
        // the NOTE-sequencer "single pitch held forever on key-release" symptom
        // -- because releaseNoteInPart would otherwise bail and leave it
        // sounding. Scan this Part's voices and release any actually sounding
        // the released pitch. Safe: a no-op when no voice sounds n8.
        for (int vi : p.voiceIndices)
            if (auto* av = getAmbikaVoice (vi))
                if (av->isVoiceActive() && av->getCurrentlyPlayingNote() == static_cast<int> (n8))
                    stopVoice (av, 1.0f, true);
        return;
    }

    if (p.polyphonyMode == 2 /*UNISON_2X*/)
    {
        const int v0 = idx * 2;
        const int v1 = (v0 + 1 < n) ? v0 + 1 : 0;
        if (auto* av = getAmbikaVoice (p.voiceIndices[(size_t) v0])) stopVoice (av, 1.0f, true);
        if (auto* av = getAmbikaVoice (p.voiceIndices[(size_t) v1])) stopVoice (av, 1.0f, true);
    }
    else if (p.polyphonyMode == 4 /*CHAIN*/)  // internal 2x doubling (Option A)
    {
        if (auto* av = getAmbikaVoice (p.voiceIndices[(size_t) idx])) stopVoice (av, 1.0f, true);
    }
    else  // POLY / CYCLIC
    {
        if (idx < n)
            if (auto* av = getAmbikaVoice (p.voiceIndices[(size_t) idx])) stopVoice (av, 1.0f, true);
    }
}

//==========================================================================
// Sustain pedal (CC64) + all-notes-off (CC123/CC120) — firmware part.cc
// 335-390 / 540-565 semantics. AUDIO THREAD ONLY (handleController /
// processTransport callers); the per-part state is plain (see Part).
void SynthEngine::drainSustainedNotes (int part)
{
    if (! ok (part)) return;
    auto& p = parts_[(size_t) part];
    // Snapshot + clear FIRST: a note-off released below can re-enter
    // bookkeeping paths that must not see the store mid-drain.
    const int n = p.numSustainedNotes_;
    Part::SustainedNote notes[kMaxVoicesPerPart];
    for (int i = 0; i < n; ++i) notes[i] = p.sustainedNotes_[i];
    p.numSustainedNotes_ = 0;

    for (int i = 0; i < n; ++i)
    {
        const uint8_t note = notes[i].note;
        const int channel = juce::jlimit (1, 16, (int) notes[i].channel);
        if (p.arp.isActive() && p.arp.holdsNote (note))
        {
            p.arp.noteOff (note);
            if (! p.arp.hasHeldKeys())
                p.seq.allNotesOff();
        }
        else
        {
            releaseNoteInPart (part, note, channel);
        }
    }
}

void SynthEngine::partAllNotesOff (int part, bool allowTailOff)
{
    if (! ok (part)) return;
    auto& p = parts_[(size_t) part];
    // Firmware part.cc:540: a held sustain pedal makes AllNotesOff a no-op.
    if (p.sustainHold_)
        return;
    // Clear the sustain store defensively (it is empty whenever the pedal is
    // up — a straggler would release dead voices below, harmless but unclean).
    p.numSustainedNotes_ = 0;
    // Release the arp/sequencer's generated notes through their own path
    // (arp.allNotesOff fires the engine-side note-off callback), then drop all
    // held-key bookkeeping exactly like firmware Part::AllNotesOff.
    p.arp.stop();   // == allNotesOff(): releases the generated note(s) via the callback
    p.arp.clearHeldKeys();   // firmware pressed_keys_.Clear() (part.cc:549)
    p.seq.allNotesOff();
    p.monoStack.clear();
    p.polyAlloc.clearNotes();
    // Release every still-sounding allocated voice (firmware releases the
    // part's whole allocated-voice list). Tail-off stop == a normal key
    // release; voices already released are skipped (stopVoice on an inactive
    // voice would clear its note twice). allowTailOff=false (CC120 / direct
    // test calls) kills immediately.
    for (int vi : p.voiceIndices)
        if (auto* av = getAmbikaVoice (vi))
            if (av->isVoiceActive())
                stopVoice (av, 1.0f, allowTailOff);
}

void SynthEngine::allNotesOff (int midiChannel, bool allowTailOff)
{
    // CC123 / CC120 (W7; see the header note): per firmware Multi::AllNotesOff
    // -> Part::AllNotesOff, for every channel-matching part — clear the
    // bookkeeping AND release the voices. The base's voice-only stop is
    // replaced, not augmented: our per-part loop already covers every voice
    // the base would have matched, plus the parts' stacks). allowTailOff is
    // honored per-voice (CC123 -> tail-off release; CC120 / direct calls ->
    // immediate Kill, the base's contract for the parameter).
    forEachPartOnChannel (midiChannel, [this, allowTailOff] (int p)
    {
        partAllNotesOff (p, allowTailOff);
    });
}

void SynthEngine::noteOn (int midiChannel, int midiNoteNumber, float velocity)
{
    // MULTICAST (W8 item 4, firmware multi.h:120-131): every part whose
    // channel+zone accepts the note gets it — a layered Omni + per-channel
    // setup plays BOTH parts, like the hardware. processTransport has already
    // routed held-key notes for arp-active parts into their stacks; only
    // "play directly" notes reach here.
    forEachAcceptingPart (midiChannel, midiNoteNumber, [&] (int part)
    {
        // Firmware AcceptNote gate BEFORE the arp-hold stack: a muted note
        // class must not be held for arpeggiation either (firmware refuses it
        // at dispatch, part.cc:649-660, before any arp bookkeeping).
        if (! isNoteAcceptedByPartTuning (part, midiNoteNumber)) return;
        if (parts_[(size_t) part].arp.isActive())
            parts_[(size_t) part].arp.noteOn (midiNoteNumber, static_cast<uint8_t> (juce::jlimit (0, 127, (int) (velocity * 127))));
        else
            triggerNoteInPart (part, midiNoteNumber, velocity, midiChannel);
    });
}

void SynthEngine::noteOff (int midiChannel, int midiNoteNumber, float /*velocity*/, bool /*allowTailOff*/)
{
    // MULTICAST (W8 item 4): the same predicate that routed the note-on
    // delivers the release — the pairing is symmetric by construction (a
    // part that accepted the on also accepts the off). Zones/channels cannot
    // change between the two without an allocation-rebuild in between, which
    // re-tags/releases voices anyway.
    forEachAcceptingPart (midiChannel, midiNoteNumber, [&] (int part)
    {
        // SUSTAIN PEDAL (W7, firmware part.cc:347-362): while the part's pedal
        // is down, a key release is held back — the note keeps sounding and is
        // remembered. The pedal-up drain (drainSustainedNotes) replays it
        // through the normal release path below.
        if (parts_[(size_t) part].sustainHold_)
        {
            parts_[(size_t) part].addSustainedNote (static_cast<uint8_t> (midiNoteNumber),
                                                    static_cast<uint8_t> (midiChannel));
            return;
        }
        // NOTE: this override runs on the buffer processTransport already
        // filtered (held-key note-offs were routed to arp.noteOff + STRIPPED
        // there), so a note-off arriving here was deliberately NOT given to
        // the arp — either the mode is off, or the note was sounding DIRECTLY
        // before the mode was enabled and never entered the held-key stack.
        // A hand-off to arp.noteOff anyway (the old unconditional isActive()
        // gate) swallowed the release and sustained the direct voice forever.
        // The holdsNote() check keeps the defensive branch for a genuinely-
        // held note while releasing everything else through the direct path.
        if (parts_[(size_t) part].arp.isActive() && parts_[(size_t) part].arp.holdsNote (midiNoteNumber))
            parts_[(size_t) part].arp.noteOff (midiNoteNumber);
        else
            releaseNoteInPart (part, midiNoteNumber, midiChannel);
    });
}

//==========================================================================
// MPE / per-voice expression routing (unified per-channel). Each handler
// targets the ACTIVE voices whose MIDI channel matches (isVoiceActive() &&
// isPlayingChannel()). Under MPE a note's channel is unique => per-note;
// under standard single-channel MIDI all notes share one channel =>
// channel-wide (the historical intent). The base per-voice callbacks
// (pitchWheelMoved / channelPressureChanged / controllerMoved) are no-ops in
// AmbikaVoice, so these overrides are the real implementation.
void SynthEngine::handlePitchWheel (int midiChannel, int wheelValue)
{
    // Latch the standing bend per channel so voices triggered LATER (while the
    // wheel stays off-centre) inherit it — see applyStandingBend / lastWheel_.
    if (midiChannel >= 0 && midiChannel < (int) lastWheel_.size())
        lastWheel_[(size_t) midiChannel].store (static_cast<int16_t> (wheelValue),
                                                std::memory_order_relaxed);
    // Host wheel 0..16383 (centre 8192) -> semitones via the per-voice bend range.
    const float semis = wheelToSemitones (wheelValue);
    forEachActiveVoiceOnChannel (midiChannel, [semis] (AmbikaVoice* av)
    {
        av->setMpePitchBendSemitones (semis);
    });
}

void SynthEngine::handleChannelPressure (int midiChannel, int channelPressureValue)
{
    const float pressure = juce::jlimit (0.0f, 1.0f, static_cast<float> (channelPressureValue) / 127.0f);
    forEachActiveVoiceOnChannel (midiChannel, [pressure] (AmbikaVoice* av)
    {
        av->setMpePressure (pressure);
    });
}

void SynthEngine::handleAftertouch (int midiChannel, int midiNoteNumber, int aftertouchValue)
{
    // POLYPHONIC AFTERTOUCH (W8 item 3): firmware multi.h:156-162 routes the
    // (channel, note, value) through accept_channel_note to EVERY accepting
    // part, then part.cc:485-526 writes MOD_SRC_AFTERTOUCH per polyphony
    // mode. The value arrives 0..127; the firmware writes it VERBATIM to the
    // mod source (no <<1 shift — unlike CC1/2/4 whose controllers arrive
    // 0..127 and shift to 0..254; poly-AT bytes already span the full source
    // range in the firmware).
    const uint8_t idx = static_cast<uint8_t> (ambika::dsp::MOD_SRC_AFTERTOUCH);
    const uint8_t val = static_cast<uint8_t> (juce::jlimit (0, 127, aftertouchValue));
    forEachAcceptingPart (midiChannel, midiNoteNumber, [&] (int p)
    {
        auto& part = parts_[(size_t) p];
        const uint8_t mode = part.polyphonyMode;
        if (mode == 1 /*POLY*/ || mode == 3 /*CYCLIC*/ || mode == 4 /*CHAIN*/)
        {
            // Firmware: write the voice ALLOCATED to that note
            // (poly_allocator_.Find). The engine's per-voice currentlyPlaying
            // note is the equivalent live mapping (a re-stolen slot carries
            // the NEW note), so write the part's ACTIVE voice that plays this
            // note. Idle voices do not get the write (a future note-on picks
            // up 0, same as firmware). Keep scanning after a write: CHAIN can
            // duplicate a note across its (doubled) set.
            for (int vi : part.voiceIndices)
                if (auto* av = getAmbikaVoice (vi);
                    av != nullptr && av->isVoiceActive()
                    && av->getCurrentlyPlayingNote() == midiNoteNumber)
                    av->setModulationSource (idx, val);
        }
        else if (mode == 2 /*UNISON_2X*/)
        {
            // Firmware: the note's PAIR (voice_index << 1 and its sibling).
            // Both pair members carry the same note, so the same
            // currently-playing scan naturally writes both.
            for (int vi : part.voiceIndices)
                if (auto* av = getAmbikaVoice (vi);
                    av != nullptr && av->isVoiceActive()
                    && av->getCurrentlyPlayingNote() == midiNoteNumber)
                    av->setModulationSource (idx, val);
        }
        else   // MONO (0): firmware falls back to the channel-wide write.
        {
            for (int vi : part.voiceIndices)
                if (auto* av = getAmbikaVoice (vi))
                    av->setModulationSource (idx, val);
        }
    });
}

// GLOBAL mod-matrix write for the continuous controllers (mod wheel / breath /
// foot pedal). Faithful to firmware Part::WriteToAllVoices (part.cc:998):
// iterate every allocated voicecard and set the mod source. Parvati gives each
// Voice its own modulation_sources_[] (firmware's is a single shared static
// array), so a write to EVERY voice reproduces that shared-global semantics —
// a CC move is immediately visible to all sounding notes AND persists in idle
// voices, so the next note-on inherits the current value. This works because note-on
// does NOT reset these sources: Voice::Trigger only writes VELOCITY/RANDOM, and
// Kill()/stopNote only touch the envelope — verified against the firmware
// voicecard/voice.cc Trigger (which behaves identically). `value0to254` is
// already the firmware-scaled value (controllerValue << 1).
void SynthEngine::applyGlobalModSource (int modSrcEnum, uint8_t value0to254, int midiChannel)
{
    const uint8_t idx = static_cast<uint8_t> (modSrcEnum);
    // Per-PART routing (firmware multi.cc ControlChange -> Part::ControlChange
    // -> WriteToAllVoices): only the Parts whose receive channel matches the
    // CC's channel (Omni or exact) write the source — the old whole-pool loop
    // made a ch-3 mod wheel modulate ALL six parts of a multitimbral setup
    // (W7, lane-B finding 3). Within a matching Part every voice (sounding
    // AND idle) is written; that preserves the current-value pickup on the
    // next note-on of that part.
    forEachPartOnChannel (midiChannel, [&] (int p)
    {
        for (int vi : parts_[(size_t) p].voiceIndices)
            if (auto* av = getAmbikaVoice (vi))
                av->setModulationSource (idx, value0to254);
    });
}

void SynthEngine::applyStandingBend (AmbikaVoice* av, int channel)
{
    if (av == nullptr || channel <= 0 || channel >= (int) lastWheel_.size())
        return;
    // Same fixed ±2-semitone conversion as handlePitchWheel, from the latched
    // wheel of the note's channel — so the new voice starts where the sounding
    // ones already are (no re-centering glitch until the next wheel event).
    const int wheel = lastWheel_[(size_t) channel].load (std::memory_order_relaxed);
    av->setMpePitchBendSemitones (wheelToSemitones (wheel));
}

void SynthEngine::handleController (int midiChannel, int controllerNumber, int controllerValue)
{
    // GLOBAL continuous controllers (firmware part.cc:367-377): mod wheel (CC1)
    // -> MOD_SRC_WHEEL, breath (CC2) -> MOD_SRC_WHEEL_2, foot pedal (CC4) ->
    // MOD_SRC_EXPRESSION, each value<<1 (0..254). These are CHANNEL-GLOBAL (not
    // per-note), so they do NOT use the per-channel MPE routing that pitch bend /
    // channel pressure / CC74 use. applyGlobalModSource writes the value to EVERY
    // voice (firmware WriteToAllVoices over all allocated voicecards). Since
    // note-on does NOT reset these sources, a new note-on automatically inherits
    // the current wheel/breath/foot value (current-wheel pickup). Coexistence
    // with the parameter map: MidiParameterMap::handleBuffer runs independently
    // in processBlock; CC1/2/4 are UNMAPPED there (midi_cc_map[1/2/4]==255), so
    // this is the ONLY effect of these controllers.
    if (controllerNumber == 1)   // modulation wheel -> MOD_SRC_WHEEL
    {
        applyGlobalModSource (ambika::dsp::MOD_SRC_WHEEL,      static_cast<uint8_t> (controllerValue << 1), midiChannel);
        return;
    }
    if (controllerNumber == 2)   // breath controller -> MOD_SRC_WHEEL_2
    {
        applyGlobalModSource (ambika::dsp::MOD_SRC_WHEEL_2,    static_cast<uint8_t> (controllerValue << 1), midiChannel);
        return;
    }
    if (controllerNumber == 4)   // foot pedal -> MOD_SRC_EXPRESSION
    {
        applyGlobalModSource (ambika::dsp::MOD_SRC_EXPRESSION, static_cast<uint8_t> (controllerValue << 1), midiChannel);
        return;
    }

    // ---- Sustain pedal CC64 (W7, firmware part.cc:379-390) ----
    // Per-PART hold state routed by channel (multi.cc ControlChange): value
    // >= 64 sets the hold (note-offs for the part are held back + remembered);
    // value < 64 clears it and drains the remembered note-offs through their
    // normal release path. Handled HERE and returned — the base class's
    // sustain handling only sets a per-voice flag that nothing in AmbikaVoice
    // reads (the old noteOff override bypassed the base release gate that
    // consumed it), so a pass-on of CC64 would be a no-op.
    if (controllerNumber == 64)
    {
        forEachPartOnChannel (midiChannel, [controllerValue, this] (int p)
        {
            auto& part = parts_[(size_t) p];
            if (controllerValue >= 64)
            {
                part.sustainHold_ = true;
            }
            else
            {
                part.sustainHold_ = false;
                drainSustainedNotes (p);
            }
        });
        return;
    }

    // ---- All Notes Off / All Sound Off live in the allNotesOff() override
    // (juce dispatches CC123/CC120 there directly, never through here). ----

    // CC74 (MPE "slide" / brightness) -> per-voice MOD_SRC_EXPRESSION. Other
    // controllers defer to the base (sustain/sostenuto/soft pedal + the no-op
    // voice->controllerMoved). CC4 (global foot) and CC74 (per-note slide) both
    // write MOD_SRC_EXPRESSION — intentional; last writer wins per voice. The
    // hardware-parity CC/NRPN -> parameter map (MidiParameterMap) runs
    // independently in the processor's processBlock (before renderNextBlock), so
    // this does NOT interfere: CC74 still also drives filter1_cutoff there, and
    // the midi_param test stays green.
    if (controllerNumber == 74)
    {
        const float slide = juce::jlimit (0.0f, 1.0f, static_cast<float> (controllerValue) / 127.0f);
        forEachActiveVoiceOnChannel (midiChannel, [slide] (AmbikaVoice* av)
        {
            av->setMpeSlide (slide);
        });
        return;
    }
    juce::Synthesiser::handleController (midiChannel, controllerNumber, controllerValue);
}

//==========================================================================
void SynthEngine::processTransport (juce::MidiBuffer& midi, int numSamples,
                                    double bpm, bool isPlaying)
{
    // Service a deferred full voice reset (patch switch) ON THE AUDIO THREAD --
    // stopNote(.,false) must not run on the message thread while a voice is
    // mid-render (it Kill()s + clearCurrentNote() + clears the FIFO). Runs before
    // the allocationDirty service below, which rebuilds + re-primes the voices.
    if (resetAllVoicesPending_.exchange (false, std::memory_order_acq_rel))
        for (auto* av : voicePool_)
            av->stopNote (0.0f, false);

    // Service deferred live-edit frame writes ON THE AUDIO THREAD: a knob edit
    // (applyPatchByte/applyPartByte) staged a Part's patch/part bytes + flagged
    // frameDirty_; push the full frame to that Part's voices now (audio-thread-
    // only pushPartBytesToVoices). Replaces the message-thread voice write that
    // raced the renderer (torn read). Order vs allocationDirty_ is irrelevant
    // (a Part dirtied by both is pushed twice, idempotently).
    for (int p = 0; p < kNumParts; ++p)
        if (parts_[(size_t) p].frameDirty_.exchange (false, std::memory_order_acq_rel))
            pushPartBytesToVoices (p);

    // (The tuning table follows the frame: pushPartBytesToVoices ends with
    // pushTuningToVoices, so byte-4 raga-preset edits reach the voices in the
    // same pass. The former custom-table tuningDirty_ service loop was removed
    // with the custom-tuning subsystem — preset edits ride frameDirty_ above.)

    // Service deferred global-option writes ON THE AUDIO THREAD: VCA curve /
    // smoothing / filter drive were staged by the message-thread setters
    // (setVcaExponential/setParameterSmoothing/setFilterDrive) so the per-voice
    // plain-field writes never race the renderer.
    if (optionsDirty_.exchange (false, std::memory_order_acq_rel))
    {
        const bool vca = pendingVcaExp_.load (std::memory_order_relaxed);
        const bool sm  = pendingSmoothing_.load (std::memory_order_relaxed);
        const float fd = pendingFilterDrive_.load (std::memory_order_relaxed);
        for (auto* av : voicePool_)
        {
            av->setVcaExponential (vca);
            av->setSmoothingEnabled (sm);
            av->setFilterDrive (fd);
            }
    }

    // Service deferred voice-allocation / polyphony / patch changes ON THE AUDIO
    // THREAD. The message thread (Multi page edits, polyphony param, .MUL load)
    // only sets the Part fields + allocationDirty_; it never touches voiceIndices.
    // The flag's release-store publishes those field writes (voiceAllocation,
    // partBytes[15], patchBytes...) to this acquire-read. Thus voiceIndices —
    // which is read by trigger/release/inject below on this same thread — is
    // mutated only here, never under a concurrent reader. (Constructor/prepare
    // still call rebuildVoiceAllocation() directly, but they run before audio
    // starts.)
    if (allocationDirty_.exchange (false, std::memory_order_acq_rel))
    {
        // NOTE: we deliberately do NOT stop/kill voices here. A hard kill
        // (stopNote(.,false) => Voice::Kill + clearCurrentNote) zeroes the
        // envelope state that only Voice::Init() re-primes, so a voice killed
        // while IDLE would render SILENT on its next note -- heard as the
        // standalone going dead after a voice-mode / template switch (the
        // reported glitch). Instead let any sounding voice ring out naturally.
        // rebuildVoiceAllocation re-tags still-allocated voices to their
        // (possibly new) Part and pushPartBytesToVoices re-applies that Part's
        // patch, so a held note picks up the new Part's sound. A voice whose
        // voicecard is no longer allocated plays out its release and frees
        // itself. No permanent stuck notes, no dead-voice glitch.

        for (int p = 0; p < kNumParts; ++p)
            parts_[(size_t) p].polyphonyMode = static_cast<uint8_t> (juce::jlimit (0, 4, (int) parts_[(size_t) p].partBytes[15]));
        rebuildVoiceAllocation();
        for (int p = 0; p < kNumParts; ++p)
            pushPartBytesToVoices (p);   // also re-primes envelope increments
    }

    // Service deferred arp/seq config writes ON THE AUDIO THREAD, BEFORE the
    // killGeneratedNotes_ + clock loop below. The message-thread setters
    // (setArpMode/setArpDirection/.../setSequenceLength/setSequenceDataByte)
    // stage into pendingConfig_ + flag configDirty_; this applies the staged
    // config to the live arp/seq objects so they never race the clock loop. The
    // active->inactive transition (arp.stop() + killGeneratedNotes_) runs here,
    // single-threaded, and the kill is serviced in the same block (next).
    for (int p = 0; p < kNumParts; ++p)
    {
        if (! parts_[(size_t) p].configDirty_.exchange (false, std::memory_order_acq_rel))
            continue;
        auto& part = parts_[(size_t) p];
        // Snapshot the MT-authoritative arp/seq config under the seqlock (never a
        // torn read of pendingConfig_ while the message thread stages a new edit).
        // On retry exhaustion the bounded reader hands back the AT's last-good
        // snapshot; re-mark configDirty_ so the NEXT block retries the fresh
        // config (the exchange above already cleared it).
        bool exhausted = false;
        const auto cfg = part.readPendingConfig (&exhausted);
        if (exhausted)
            part.configDirty_.store (true, std::memory_order_release);
        // arp/seq mode (same byte drives both): handle transition on the AT.
        {
            const bool wasActive = part.arp.isActive();
            part.arp.setMode (cfg.arpMode);
            part.seq.setMode (cfg.arpMode);
            if (wasActive && ! part.arp.isActive())
            {
                part.arp.stop();
                part.seq.stop();   // release the note SEQUENCE's sounding note too
                part.killGeneratedNotes_.store (true, std::memory_order_release);
            }
        }
        part.arp.setDirection  (cfg.arpDirection);
        part.arp.setOctave     (cfg.arpOctave);
        part.arp.setPattern    (cfg.arpPattern);
        part.arp.setResolution (cfg.arpResolution);
        for (int i = 0; i < 3; ++i)
            part.seq.setSequenceLength (i, cfg.seqLength[i]);
        for (int i = 0; i < 64; ++i)
            part.seq.setSequenceDataByte (i, cfg.seqData[i]);

        // A consistent snapshot was fully applied: remember it as the AT's
        // last-good config (the bounded seqlock reader's exhaustion fallback).
        part.lastGoodConfig_ = cfg;
        part.hasLastGoodConfig_ = true;
    }

    // Service deferred arp/seq note-kills ON THE AUDIO THREAD. setArpMode
    // (message thread) flags a Part when its arp/seq is turned off; kill that
    // Part's generated voices here so stopNote() never touches a voice the audio
    // thread is mid-rendering (one-block latency, inaudible).
    for (int p = 0; p < kNumParts; ++p)
    {
        if (parts_[(size_t) p].killGeneratedNotes_.exchange (false, std::memory_order_acq_rel))
        {
            for (auto* av : voicePool_)
                if (av->getPartIndex() == p)
                    av->stopNote (0.0f, false);
        }
    }

    transport_.setTempo (bpm);
    applyTempo (bpm);

    // Tempo-synced delay tails (ClockedDelay) scale with the tempo: refresh the
    // tail cache when the tempo moves materially (>0.25 BPM — ignores jitter,
    // catches any real tempo change / ramp). Pure math + one atomic store.
    if (std::abs (bpm - tailBpmCache_.load (std::memory_order_relaxed)) > 0.25)
    {
        tailBpmCache_.store (bpm, std::memory_order_relaxed);
        recomputeTailCache();
    }

    // Push transport to every per-part FX chain so tempo-aware effects (the FV-1
    // Clocked Delay) can sync to the host. Polled once per block; the chain fans
    // it out to each slot's processor (default no-op).
    for (int p = 0; p < kNumParts; ++p)
        fxChains_[(size_t) p].setTempo (bpm, isPlaying);

    if (isPlaying && ! wasPlaying_)
    {  // NOLINT(bugprone-branch-clone): the true-branch starts arp+seq, the else stops arp -- different bodies, clang-tidy FP
        for (auto& part : parts_) { part.arp.start(); part.seq.start(); }
    }
    else if (! isPlaying && wasPlaying_)
    {
        for (auto& part : parts_) { part.arp.stop(); part.seq.stop(); }
    }
    wasPlaying_ = isPlaying;

    // Per-Part: route note on/off into a Part's held-key stack when that Part's
    // arp/sequencer is active (strip them so renderNextBlock does not also play
    // the raw held key). Non-arp Parts pass through to handleNoteOn/Off.
    processedMidi_.clear();
    for (const auto meta : midi)
    {
        const auto msg = meta.getMessage();
        const int channel = msg.getChannel();
        const int note = msg.getNoteNumber();

        if (msg.isNoteOn() && msg.getVelocity() > 0)
        {
            // MULTICAST (W8 item 4): every accepting part gets the note. An
            // arp-active part takes it into its held-key stack (stripped from
            // the direct pass); a direct part keeps the raw event so
            // renderNextBlock -> noteOn triggers it. A note can do BOTH (an
            // Omni direct part + a ch-2 arp part).
            bool anyDirect = false;
            forEachAcceptingPart (channel, note, [&] (int p)
            {
                if (parts_[(size_t) p].arp.isActive())
                {
                    // PHRASE RESTART (W8 item 2, firmware multi.h:120-125 /
                    // multi.cc:184-192): a NEW phrase — the stack was empty
                    // before this note — while the transport is STOPPED
                    // restarts that part's arp + sequencer at step 0
                    // (Multi::Start -> Part::Start: pattern mask 0x1, step 0,
                    // forced first clock). Firmware ordering: Start() runs
                    // BEFORE the part receives the note. Only when the stack
                    // was empty: adding a note mid-phrase keeps the pattern
                    // position (firmware's "running_" gate).
                    if (! isPlaying && ! parts_[(size_t) p].arp.hasHeldKeys())
                    {
                        parts_[(size_t) p].arp.start();
                        parts_[(size_t) p].seq.start();
                    }
                    parts_[(size_t) p].arp.noteOn (note, msg.getVelocity());   // held key (stripped)
                }
                else
                    anyDirect = true;   // this part plays it directly below
            });
            if (anyDirect)
                processedMidi_.addEvent (msg, meta.samplePosition);
        }
        else if (msg.isNoteOff() || (msg.isNoteOn() && msg.getVelocity() == 0))
        {
            // MULTICAST release, symmetric with the on above: every accepting
            // part handles the off through ITS mode (arp-held -> stack pop;
            // direct -> forwarded to renderNextBlock -> noteOff).
            bool anyDirect = false;
            forEachAcceptingPart (channel, note, [&] (int p)
            {
                if (parts_[(size_t) p].arp.isActive()
                    && parts_[(size_t) p].arp.holdsNote (note))
                {
                    // SUSTAIN PEDAL (W7): with the part's pedal down the key
                    // stays "held" for arpeggiation (exactly what a sustain
                    // pedal does to an arp — firmware flags the pressed_keys_
                    // entry; holding the stack entry achieves the same audible
                    // result) and the note-off is remembered for the pedal-up
                    // drain.
                    if (parts_[(size_t) p].sustainHold_)
                    {
                        parts_[(size_t) p].addSustainedNote (static_cast<uint8_t> (note),
                                                             static_cast<uint8_t> (channel));
                        return;   // stripped (never reaches the direct path)
                    }
                    parts_[(size_t) p].arp.noteOff (note);   // stripped
                    // If that emptied the held-key stack the arp already killed
                    // its own note; also release the note SEQUENCE's sounding
                    // note (firmware Part::NoteOff -> AllNotesOff on empty
                    // stack; the arp's allNotesOff does not touch the seq's
                    // previousNote_).
                    if (! parts_[(size_t) p].arp.hasHeldKeys())
                        parts_[(size_t) p].seq.allNotesOff();
                }
                else
                {
                    // Not held by the arp/sequencer: either the mode is off, or
                    // the note was sounding DIRECTLY before the mode was
                    // enabled (it never entered the held-key stack — enable-
                    // time transitions do not migrate sounding notes into it,
                    // and killGeneratedNotes_ only fires on the
                    // active->inactive direction). Forward the note-off through
                    // the normal path so the direct voice releases. A swallow
                    // here sustained that voice forever (firmware
                    // Part::NoteOff -> AllNotesOff on empty stack).
                    anyDirect = true;
                }
            });
            if (anyDirect)
                processedMidi_.addEvent (msg, meta.samplePosition);
        }
        else
        {
            processedMidi_.addEvent (msg, meta.samplePosition);
        }
    }
    midi.swapWith (processedMidi_);

    // Advance the shared 24-PPQN clock; each Part's arp self-prescales (own
    // clockCounter_/resolution) and drives its own arp + sequencer. The arp's
    // clockTick() returns true when a prescaled STEP fired — we gate the
    // Sequencer on it so BOTH run at the same rate (firmware part.cc:590-601
    // runs ClockSequencer + ClockArpeggiator in the SAME prescaled branch;
    // previously seq.clockTick ran every raw 24-PPQN tick = 24x too fast at the
    // default resolution). The arp prescaler still advances every call when its
    // mode is off, so the Sequencer's modulation seqs keep stepping under a
    // running transport. Run the clock while the host transport plays, OR while
    // any Part has its arp / note-sequencer active with held keys.
    auto anyPartClockActive = [this]() -> bool
    {
        return std::any_of (parts_.begin(), parts_.end(), [] (const Part& part) {
            return part.arp.isActive() && part.arp.hasHeldKeys();
        });
    };
    const bool runClock = isPlaying || anyPartClockActive();
    if (runClock)
    {
        const int ticks = transport_.advance (numSamples);
        for (int t = 0; t < ticks; ++t)
        {
            for (int p = 0; p < kNumParts; ++p)
            {
                auto& part = parts_[(size_t) p];
                if (part.arp.clockTick())   // a prescaled step fired -> advance the seq too
                {
                    const uint8_t heldNote = part.arp.mostRecentNote();
                    const bool keyHeld = part.arp.hasHeldKeys();
                    part.seq.clockTick (heldNote, keyHeld);
                }
            }
        }
    }
    else
    {
        transport_.advance (numSamples);   // keep the clock roughly in sync
    }

    injectSequencerModulation();
}

void SynthEngine::injectSequencerModulation()
{
    for (int p = 0; p < kNumParts; ++p)
    {
        auto& part = parts_[(size_t) p];
        for (int vi : part.voiceIndices)
        {
            auto* av = getAmbikaVoice (vi);
            if (! av) continue;
            if (part.seq.seqActive (0))
                av->setModulationSource (ambika::dsp::MOD_SRC_SEQ_1, part.seq.seqValue (0));
            if (part.seq.seqActive (1))
                av->setModulationSource (ambika::dsp::MOD_SRC_SEQ_2, part.seq.seqValue (1));
            if (part.arp.isActive())
                av->setModulationSource (ambika::dsp::MOD_SRC_ARP_STEP, part.arp.stepGateValue());
        }
    }
}

//==========================================================================
// Multi-output routing.
int SynthEngine::voiceCardForIndex (int voiceIndex)
{
    // Pre-rebuild default: pool voices spread round-robin over the 6 cards.
    // rebuildVoiceAllocation re-tags every allocated voice onto its own Part's
    // cards, so in practice this only covers unallocated pool voices.
    return juce::jlimit (0, kNumVoices - 1, voiceIndex) % kNumParts;
}

void SynthEngine::renderVoices (juce::AudioBuffer<float>& outputAudio,
                                int startSample, int numSamples)
{
    juce::ignoreUnused (outputAudio);
    if (numSamples <= 0)
        return;

    // Clear the per-voicecard buffers for this sub-block range. processNextBlock
    // splits the host block at MIDI events and calls renderVoices with
    // non-overlapping [startSample, startSample+numSamples) ranges, so each call
    // owns its range and a sample position is cleared exactly once per block.
    // Clearing ALL six keeps an enabled-but-idle aux bus silent.
    for (auto& b : voiceCardBuffers_)
        b.clear (startSample, numSamples);

    // Clear every voice's mod-source capture ring ONCE per host block (the first
    // sub-split starts at 0). Each voice then re-fills its ring during its lazy
    // FIFO refill (one entry per 40-sample internal block); renderPartFx reads
    // the first-active voice's ring at the ~980 Hz internal-block cadence.
    if (startSample == 0)
        for (auto* av : voicePool_)
            av->clearModRing();

    // Route each voice to its FIXED voicecard buffer (Ambika hardware: 6
    // individual voicecard outputs). The master outputAudio is left untouched
    // here; the processor fills the main + aux buses from these buffers after
    // renderNextBlock returns.
    for (auto* av : voicePool_)
    {
        const int vc = juce::jlimit (0, kNumParts - 1, av->getVoiceCard());
        av->renderNextBlock (voiceCardBuffers_[(size_t) vc], startSample, numSamples);
    }
}

//==========================================================================
// Per-part FX render (audio thread; called from PluginProcessor::processBlock
// AFTER renderNextBlock, BEFORE the main-bus sum). For each Part this:
//   1. Services fxDirty_ single-threaded (pushes staged FX params + the
//      mod-matrix routing into the chain + the AT cache).
//   2. Builds a per-part mono sum of its voicecard buffers.
//   3. Samples the first active voice's mod sources into lastModSources_[p].
//   4. Evaluates the 16-slot FX mod matrix at block rate, combining base + mod
//      into effective chain dryWet/param values.
//   5. Duplicates the mono sum to L+R and runs fxChains_[p] into fxOutputBuffers_.
// With all fx*_enabled=0 the chain is a dry copy (audibly-identical to the
// pre-FX path). No allocation on the AT (everything reserved in prepare).
void SynthEngine::renderPartFx (int numSamples)
{
    if (numSamples <= 0)
        return;

    bool anyFxDirtied = false;   // tail-cache refresh condition (see below)

    // UI live-modulation telemetry (docs/LIVE_MOD_FEEDBACK_DESIGN.md): ONE
    // relaxed load per block names the tracked part; when nothing tracks it
    // (-1, the pre-wiring default) the per-part cost below is a single compare.
    const int uiTelPartLoad = uiTelPart_.load (std::memory_order_relaxed);

    for (int p = 0; p < kNumParts; ++p)
    {
        auto& part = parts_[(size_t) p];
        auto& chain = fxChains_[(size_t) p];
        auto& cache = fxCached_[(size_t) p];
        const bool uiTelTrack = (p == uiTelPartLoad);

        // ---- 0. UI telemetry service stage (tracked part only) ----
        // Clears the frame when the tracked part changed or a reset was
        // requested (patch load / part switch / init), stamping the CURRENT
        // epoch + part so the reader sees fresh-but-empty until history
        // repopulates. Runs regardless of voice activity so a part switch
        // while silent still lands.
        if (uiTelTrack)
            uiTelServiceStage (p);

        // ---- 1. Service staged FX state (single-threaded on the AT) ----
        // (a) Install any staged type swaps FIRST (a staged swap is its own
        //     reason to service -- it must land before this block's process,
        //     regardless of the dirty flag). Pointer moves only: the processor
        //     was built + prepared on the message thread (audit F1; the old
        //     in-service createFxProcessor + ~512 KB free ran inside
        //     processBlock).
        chain.servicePendingTypeSwaps();

        // (b) Read the MT-staged fxState frame (published by fxDirty_ release-
        //     store) into the AT cache + push topology/order/enabled to the
        //     chain. Slot TYPES were consumed above.
        if (part.fxState.fxDirty_.exchange (false, std::memory_order_acq_rel))
        {
            anyFxDirtied = true;
            chain.setTopology (static_cast<FxTopology> (part.fxState.topology.load (std::memory_order_relaxed)));
            chain.setOrder (fxOrderPermutation (part.fxState.orderIdx.load (std::memory_order_relaxed)));

            for (int s = 0; s < kNumFxSlots; ++s)
            {
                const uint8_t newType = part.fxState.slotType[(size_t) s].load (std::memory_order_relaxed);
                chain.setSlotEnabled (s, part.fxState.slotEnabled[(size_t) s].load (std::memory_order_relaxed) != 0);
                cache.baseDryWet[(size_t) s] = (float) part.fxState.slotDryWet[(size_t) s].load (std::memory_order_relaxed) / 127.0f;
                for (int k = 0; k < kNumFxSlotParams; ++k)
                    cache.baseParam[(size_t) s][(size_t) k] = (float) part.fxState.slotParam[(size_t) s][(size_t) k].load (std::memory_order_relaxed) / 127.0f;
                // On a type change the param MEANINGS change entirely — snap the
                // smoothed base to the new values so it does not ramp through the
                // old effect's stale param values (which would be audibly wrong).
                if (newType != prevSlotType_[(size_t) p][(size_t) s])
                {
                    for (int k = 0; k < kNumFxSlotParams; ++k)
                        smoothedBase_[(size_t) p][(size_t) s][(size_t) k] = cache.baseParam[(size_t) s][(size_t) k];
                    prevSlotType_[(size_t) p][(size_t) s] = newType;
                }
            }
            for (int m = 0; m < kNumFxMatrixSlots; ++m)
            {
                cache.modSrc[(size_t) m] = part.fxState.modSource[(size_t) m].load (std::memory_order_relaxed);
                cache.modDst[(size_t) m] = part.fxState.modDest  [(size_t) m].load (std::memory_order_relaxed);
                cache.modAmt[(size_t) m] = part.fxState.modAmount[(size_t) m].load (std::memory_order_relaxed);
            }
            // Master-section frame (v3): push global mix + the 3-band master EQ
            // to this part's chain. (Tail retention is now unconditional inside
            // FxChain.)
            const uint8_t fxMixV       = part.fxState.mix.load (std::memory_order_relaxed);
            const uint8_t fxEqLowV     = part.fxState.eqLow.load (std::memory_order_relaxed);
            const uint8_t fxEqMidV     = part.fxState.eqMid.load (std::memory_order_relaxed);
            const uint8_t fxEqHighV    = part.fxState.eqHigh.load (std::memory_order_relaxed);
            chain.setMasterMix    ((float) fxMixV / 127.0f);
            chain.setMasterEqLow  (fxEqLowV);
            chain.setMasterEqMid  (fxEqMidV);
            chain.setMasterEqHigh (fxEqHighV);
        }

        // ---- 2. Per-part mono sum of this Part's voicecard buffers ----
        // (voiceCardBuffers_ holds the full block after renderNextBlock.)
        // Sum each OWNED card's buffer exactly once, per the bitmask resolved by
        // rebuildVoiceAllocation (partCardMask_). Indexing voiceCardBuffers_ by
        // the pool voice index instead (as this loop once did) is only correct
        // in the default single-part layout where pool index == card index.
        // With per-part voice slots or custom card bitmasks it cross-bleeds
        // other Parts' cards into this Part's FX input and leaves later Parts'
        // FX silent (pool slices >= kNumParts). Mask 0 => silence (a disabled
        // Part), which is correct.
#ifndef NDEBUG
        {
            // Debug consistency check: recomputing the owned-card mask from the
            // voices themselves must equal the persisted rebuild result.
            uint8_t recompute = 0;
            for (int vi : part.voiceIndices)
                if (auto* av = getAmbikaVoice (vi))
                    recompute = static_cast<uint8_t> (recompute | (1u << juce::jlimit (0, kNumParts - 1, av->getVoiceCard())));
            jassert (recompute == partCardMask_[(size_t) p]);
        }
#endif
        float* mono = fxMonoScratch_.getWritePointer (0);
        juce::FloatVectorOperations::clear (mono, numSamples);
        for (int vc = 0; vc < kNumParts; ++vc)
            if (partCardMask_[(size_t) p] & (1u << vc))
                juce::FloatVectorOperations::add (mono, voiceCardBuffers_[(size_t) vc].getReadPointer (0), numSamples);

        // ---- 2b. Chain-input safety ceiling ----
        // The mono voicecard sum is UNCLAMPED polyphony: up to 16 voices per
        // part, each with resonant filter peaks above unity, and the whole
        // chain (Wavefolder 6x SRC, Diffuser, WSOLA...) is gain-staged for
        // roughly unity-per-voice levels. A SoftLimit knee (unity-gain mapping
        // 8 * SoftLimit(s/8)) keeps ordinary playing TRANSPARENT (measured:
        // -0.04 dB at |s|=1, -0.35 dB at |s|=3 — a loud chord; -2.2 dB at |s|=8)
        // while it progressively limits runaway sums. The hard jlimit (+/-16)
        // is the final ceiling for pathological input (|s| >= ~100). The pair
        // guarantees downstream processors never see unbounded levels — the
        // class of over-range input that fed the Wavefolder LUT overrun (fixed
        // at the lookup itself too; this is defense in depth, finding 8).
        for (int i = 0; i < numSamples; ++i)
        {
            const float s = mono[i];
            mono[i] = juce::jlimit (-16.0f, 16.0f,
                                    8.0f * stmlib::SoftLimit (s * 0.125f));
        }

        // ---- 3. Representative voice = the MOST-RECENTLY-TRIGGERED active voice ----
        // ---- + crossfade on any voice change.                                  ----
        // The mod sources advance once per 40-sample internal block inside each
        // voice (980 Hz); fillInternalBlock pushes them into a per-voice ring.
        // The FX stage is per-part but sources are per-voice, so we sample ONE
        // voice per part: among the part's active voices, the one with the
        // highest triggerSeq() (the "last" note). A monotonic seq makes the pick
        // STABLE between note-on events (no churn). It follows the latest note-
        // on automatically, and on a release it falls back to the next-most-
        // recent active voice. On any voice IDENTITY change a short (~5 ms)
        // crossfade bridges the old voice's last effective source values
        // (lastModSources_) to the new voice's live values, so per-voice sources
        // (VELOCITY / NOTE / per-note MPE) glide instead of clicking. Global /
        // part-global sources are identical across voices so the crossfade is a
        // no-op there. When no voice is active we hold the last snapshot so
        // tails still modulate.
        AmbikaVoice* repVoice = nullptr;
        int newestIdx = -1;
        uint64_t newestSeq = 0;
        for (int vi : part.voiceIndices)
        {
            auto* av = getAmbikaVoice (vi);
            if (av == nullptr || ! av->isVoiceActive()) continue;
            const uint64_t s = av->triggerSeq();
            if (repVoice == nullptr || s > newestSeq)
            {
                repVoice = av; newestIdx = vi; newestSeq = s;
            }
        }
        if (repVoice != nullptr && newestIdx != fxTrackedVoice_[(size_t) p])
        {
            // Identity changed (a new note-on landed on a different voice, or
            // the tracked voice released and another active voice is now the
            // most recent). Arm the crossfade from the last effective values.
            // The very first selection (fxTrackedVoice_ < 0) has no prior values
            // to fade from, so the crossfade stays settled and the new voice is
            // used live directly (no fade-in from zero).
            if (fxTrackedVoice_[(size_t) p] >= 0)
            {
                fxFadeStart_[(size_t) p] = lastModSources_[(size_t) p];
                fxFadePhase_[(size_t) p] = 0.0f;
            }
            fxTrackedVoice_[(size_t) p] = newestIdx;
        }
        const int ringCount = repVoice != nullptr ? repVoice->modRingCount() : 0;
        debugLastFxRingCount_[(size_t) p] = ringCount;

        // ---- 4–5. Sub-chunk the host block at internal-block boundaries ----
        // An internal block (40 samples @ 39216 Hz) spans 40*sr/39216 host
        // samples (non-integer). A drift-free fractional phase tracks the
        // boundary across blocks so the ~980 Hz cadence is exact over time. For
        // each sub-chunk: read this internal block's mod sources (ring entry, or
        // held lastModSources_), evaluate the 16-slot FX mod matrix, push the
        // effective dryWet/param, and run the chain on just that sub-chunk.
        // Because every FX processor is block-size-invariant (state carries
        // across calls), this is bit-identical to one full-block call when the
        // params are constant.
        const double step = 40.0 * getSampleRate() / ambika::dsp::kInternalSampleRate;
        double nextBoundary = fxSubPhase_[(size_t) p];
        auto& out = fxOutputBuffers_[(size_t) p];
        float* outL = out.getWritePointer (0);
        float* outR = out.getWritePointer (1);

        int written = 0;
        int ringIdx = 0;
        while (written < numSamples)
        {
            int sub = (int) (nextBoundary - (double) written);
            if (sub <= 0) sub = 1;                                   // rounding guard
            if (sub > numSamples - written) sub = numSamples - written;

            // Mod sources for THIS internal block: the tracked voice's live
            // ring entry (or the held lastModSources_ when no voice is active /
            // the ring is empty), CROSSFADED from fxFadeStart_ when a voice
            // change is in progress. The lerp is a no-op for global/part-global
            // sources (identical across voices) and only blends the per-voice
            // ones (VELOCITY / NOTE / per-note MPE) so they glide on a switch.
            const uint8_t* liveSrcs = (ringCount > 0)
                ? repVoice->modRingEntry (juce::jmin (ringIdx, ringCount - 1))
                : lastModSources_[(size_t) p].data();
            const float fade = fxFadePhase_[(size_t) p];
            uint8_t effSrcs[ambika::dsp::MOD_SRC_LAST];
            if (fade >= 1.0f)
            {
                // Settled (the steady-state path): copy live directly, no lerp.
                for (int src = 0; src < ambika::dsp::MOD_SRC_LAST; ++src)
                    effSrcs[src] = liveSrcs[src];
            }
            else
            {
                const auto& start = fxFadeStart_[(size_t) p];
                for (int src = 0; src < ambika::dsp::MOD_SRC_LAST; ++src)
                {
                    const float v = juce::jmap (fade,
                        static_cast<float> (start[(size_t) src]),
                        static_cast<float> (liveSrcs[src]));
                    effSrcs[src] = static_cast<uint8_t> (juce::jlimit (0.0f, 255.0f, v));
                }
            }
            const uint8_t* srcs = effSrcs;
            // Mirror EFFECTIVE into lastModSources_ so held tails / later
            // sub-chunks keep the freshest value AND a later voice change
            // crossfades from here.
            for (int src = 0; src < ambika::dsp::MOD_SRC_LAST; ++src)
                lastModSources_[(size_t) p][(size_t) src] = effSrcs[src];
            // UI telemetry: decimated history append of THIS internal block's
            // sources — ALWAYS (2026-08-21 always-on contract, user request:
            // strips start at zero, keep scrolling, and show the modulator's
            // ACTUAL state; a released/idle part no longer freezes in place).
            // PURE OBSERVATION.
            //
            // STICKY VOICE (2026-08-21 — the "jumpy slow envelope" fix): while
            // a voice sounds, appends follow ONE voice per note, NOT the FX
            // representative (the rep voice switches to the NEWEST note on
            // every strike, so a slow release tail interleaved with fresh
            // attacks read as noise). The telemetry pick sticks to its voice
            // while that exact trigger is still active (slot + triggerSeq
            // identify it) and only re-picks when it stops. When NOTHING is
            // active the IDLE row carries the actual state: persisted
            // controllers (bend/wheels/expression) + literal constants, zeros
            // for the per-voice generators (LFO/ENV/... only run inside active
            // voices here — not running = zero, the user's LFO example).
            if (uiTelTrack)
            {
                // Sticky alive check: the exact trigger must still be sounding
                // (slot active + seq match; a recycled slot never masquerades).
                auto* sticky = (uiTelVoiceSlot_ >= 0)
                    ? getAmbikaVoice (uiTelVoiceSlot_) : nullptr;
                const bool stickyAlive = sticky != nullptr
                    && sticky->isVoiceActive()
                    && sticky->triggerSeq() == uiTelVoiceSeq_;
                if (! stickyAlive)
                {
                    uiTelVoiceSlot_ = newestIdx;   // re-pick: the newest active trigger
                    uiTelVoiceSeq_  = newestSeq;
                    sticky = (uiTelVoiceSlot_ >= 0)
                        ? getAmbikaVoice (uiTelVoiceSlot_) : nullptr;
                }
                if (sticky != nullptr && sticky->isVoiceActive()
                    && sticky->triggerSeq() == uiTelVoiceSeq_)
                {
                    const int telRing = sticky->modRingCount();
                    if (telRing > 0)
                        uiTelAppendHistory (
                            sticky->modRingEntry (juce::jmin (ringIdx, telRing - 1)),
                            part.seq);
                    uiTelLiveSeen_ = true;      // live truth seen; the next idle transition re-seeds
                    uiTelIdleSeeded_ = false;
                }
                else
                {
                    // IDLE (no active voice): the actual-state row — with a
                    // DRAG-OUT (2026-08-22 user request): the per-voice
                    // generators do not SNAP to zero on release (that read as
                    // the pill suddenly speeding). Their row values fall
                    // linearly at the strip's own scroll pace — 2 bytes per
                    // append = a full-scale 255->0 fall across exactly one
                    // history window (256 appends ~ 3.1 s), the same speed the
                    // trace itself progresses. Persisted controllers keep their true
                    // value: WHEEL/WHEEL_2/EXPRESSION from the PART'S VOICE TABLE
                    // (handleController CC1/2/4 writes them to every voice,
                    // sounding AND idle); PITCH_BEND from the standing-bend
                    // LATCH (see the override below); constants literal.
                    // uiTelIdlePrev_ seeds from the last effective row on the
                    // active->idle transition and decays only on real appends
                    // (the append's own decimation gate).
                    if (uiTelLiveSeen_ && ! uiTelIdleSeeded_)
                    {
                        // Seed ONLY on a genuine active->idle transition: after
                        // a telemetry wipe (fresh zero buffer) lastModSources_
                        // still holds the pre-wipe live row — FX-tail state the
                        // wipe deliberately does not clear — and seeding from
                        // it would resurrect stale motion into the fresh
                        // window ([3] reset contract).
                        std::copy (lastModSources_[(size_t) p].begin(),
                                   lastModSources_[(size_t) p].end(),
                                   uiTelIdlePrev_.begin());
                        uiTelIdleSeeded_ = true;
                    }
                    uint8_t idleRow[ambika::dsp::MOD_SRC_LAST];
                    for (int src = 0; src < ambika::dsp::MOD_SRC_LAST; ++src)
                    {
                        if (src >= 24)   // CONSTANT_*: a constant's state IS its value
                            idleRow[(size_t) src] = parvati::telemetryConstantByte (src);
                        else if (parvati::telemetrySourcePersistsWhenIdle (src))
                            idleRow[(size_t) src] = lastModSources_[(size_t) p][(size_t) src];
                        else
                            idleRow[(size_t) src] = uiTelIdlePrev_[(size_t) src];
                    }
                    if (! part.voiceIndices.empty())
                        if (auto* gv = getAmbikaVoice (part.voiceIndices[0]))
                            for (int src = ambika::dsp::MOD_SRC_WHEEL;
                                 src <= ambika::dsp::MOD_SRC_EXPRESSION; ++src)
                                idleRow[(size_t) src] = gv->getModulationSource (
                                    static_cast<uint8_t> (src));
                    // PITCH_BEND from the standing-bend LATCH (pre-existing
                    // bug fixed 2026-08-23): lastModSources_ is only written
                    // from a SOUNDING voice's ring. So at startup / after a
                    // telemetry wipe — before any note — the idle row carried
                    // 0 and the Pitch Bend pill strip scrolled from the FLOOR
                    // although the wheel rests at 128 = mid = 50%. The latch
                    // (lastWheel_, initialized to the centre, per MIDI channel,
                    // written by handlePitchWheel) IS the idle truth; the byte
                    // mapping mirrors the voice side (applyMpeToVoice:
                    // 128 + norm*127 — the bend-range factor cancels, so the
                    // host wheel maps linearly 0..16383 -> 1..255 about 128).
                    // An Omni part (channel 0) has no single channel: read the
                    // global master channel 1 latch, where a non-MPE wheel
                    // arrives.
                    {
                        const uint8_t pch = parts_[(size_t) p].midiChannel.load (std::memory_order_relaxed);
                        const int bendCh = (pch >= 1 && pch <= 16) ? (int) pch : 1;
                        const int wheel = lastWheel_[(size_t) bendCh].load (std::memory_order_relaxed);
                        const float bendNorm = juce::jlimit (-1.0f, 1.0f,
                            (static_cast<float> (wheel) - 8192.0f) * (1.0f / 8192.0f));
                        idleRow[(size_t) ambika::dsp::MOD_SRC_PITCH_BEND] =
                            static_cast<uint8_t> (juce::jlimit (0, 255,
                                juce::roundToInt (128.0f + bendNorm * 127.0f)));
                    }
                    const bool appended = uiTelAppendHistory (idleRow, part.seq,
                                                              uiTelNoteSeqLast_);
                    if (appended)
                    {
                        constexpr uint8_t kFall = 1;   // 255->0 in ~256 appends = one window
                        for (int src = 0; src < ambika::dsp::MOD_SRC_LAST; ++src)
                            if (! parvati::telemetrySourcePersistsWhenIdle (src))
                                uiTelIdlePrev_[(size_t) src] = (uiTelIdlePrev_[(size_t) src] > kFall)
                                    ? static_cast<uint8_t> (uiTelIdlePrev_[(size_t) src] - kFall)
                                    : uint8_t { 0 };
                        uiTelNoteSeqLast_ = (uiTelNoteSeqLast_ > kFall)
                            ? static_cast<uint8_t> (uiTelNoteSeqLast_ - kFall) : uint8_t { 0 };
                    }
                    uiTelVoiceSlot_ = -1;   // the pick is gone; re-pick on the next note
                }
            }
            // Advance the crossfade phase for the next sub-chunk (drift-free:
            // tau in seconds => sample-rate independent).
            if (fade < 1.0f)
                fxFadePhase_[(size_t) p] = juce::jmin (1.0f, fade
                    + static_cast<float> (sub)
                        / static_cast<float> (kFxCrossfadeTauSec * getSampleRate()));

            // ---- FX mod matrix (per sub-chunk) ----
            // modOffset[dest] += amount/63 * norm (dest = slot*(kNumFxSlotParams+1)
            // + field: 0=dryWet, 1..kNumFxSlotParams=param 0..N-1). AC/DC coupling
            // mirrors the SYNTH voice mod matrix (voice.cpp ProcessModulationMatrix):
            // LFO_1..4 / PITCH_BEND / NOTE are AC-coupled (128 = neutral, bipolar ±1);
            // all other sources are DC-coupled (0 = neutral, unipolar 0..1). Without
            // the AC branch an LFO/bend/note at rest (128) injected a static
            // +0.126 offset (at amount 63) instead of zero modulation.
            constexpr int kFxFields = kNumFxSlotParams + 1;   // 1 dry/wet + N params per slot
            float modOffset[kNumFxSlots * kFxFields] {};
            for (int m = 0; m < kNumFxMatrixSlots; ++m)
            {
                const int8_t amt = cache.modAmt[(size_t) m];
                if (amt == 0) continue;
                const int dst = (int) cache.modDst[(size_t) m];
                if (dst < 0 || dst >= (int) (sizeof (modOffset) / sizeof (float))) continue;
                const uint8_t sIdx = cache.modSrc[(size_t) m];
                if (sIdx >= ambika::dsp::MOD_SRC_LAST) continue;
                const bool ac = (sIdx >= ambika::dsp::MOD_SRC_LFO_1 && sIdx <= ambika::dsp::MOD_SRC_LFO_4)
                             || sIdx == ambika::dsp::MOD_SRC_PITCH_BEND
                             || sIdx == ambika::dsp::MOD_SRC_NOTE;
                const float norm = ac
                    ? ((static_cast<float> (srcs[sIdx]) - 128.0f) * (1.0f / 128.0f))
                    :  (static_cast<float> (srcs[sIdx]) * (1.0f / 255.0f));
                modOffset[dst] += ((float) amt / 63.0f) * norm;
            }

            // De-click coefficient for the BASE param (N-adaptive: computed from
            // the sub-chunk size so the 3 ms tau is correct at any block size).
            const float dcPc = 1.0f - std::exp (-static_cast<float> (sub)
                / static_cast<float> (kBaseDeClickTauSec * getSampleRate()));

            // Effective slot values = SMOOTHED base + RAW mod, clamped 0..1.
            // The base is one-pole-smoothed (de-clicks knob/preset jumps); the
            // mod-matrix offset passes through RAW (no slew on LFO/env/seq
            // modulation — audio-rate parity with the synth voice path). dryWet
            // keeps its own per-sample smoother in FxChain (smoothCoef_, 20 ms).
            for (int s = 0; s < kNumFxSlots; ++s)
            {
                chain.setSlotDryWet (s, juce::jlimit (0.0f, 1.0f,
                    cache.baseDryWet[(size_t) s] + modOffset[s * kFxFields + 0]));
                for (int k = 0; k < kNumFxSlotParams; ++k)
                {
                    float& sb = smoothedBase_[(size_t) p][(size_t) s][(size_t) k];
                    sb += (cache.baseParam[(size_t) s][(size_t) k] - sb) * dcPc;
                    const float eff = juce::jlimit (0.0f, 1.0f,
                        sb + modOffset[s * kFxFields + 1 + k]);
                    chain.setSlotParam (s, k, eff);
                    if (debugEffParamTracking_ && s == 0 && k == 0)
                    {
                        // Read what FxChain actually stored (params_), not the
                        // local eff — so a smoother injected between eff and the
                        // DSP (engine-side or via setSlotParam) IS caught.
                        const float pv = chain.debugGetParam (0, 0);
                        debugEffParamMin_[(size_t) p] = juce::jmin (debugEffParamMin_[(size_t) p], pv);
                        debugEffParamMax_[(size_t) p] = juce::jmax (debugEffParamMax_[(size_t) p], pv);
                    }
                }
            }

            // Run the chain on this sub-chunk (params_ is pushed raw at ~980 Hz).
            chain.process (mono + written, mono + written,
                           outL + written, outR + written, sub);

            written += sub;
            nextBoundary += step;
            if (ringIdx < ringCount - 1) ++ringIdx;   // hold last entry for any extra sub-chunks
        }
        fxSubPhase_[(size_t) p] = nextBoundary - (double) numSamples;   // drift-free carry

        // UI telemetry: once-per-block observables refresh (envelope stage /
        // progress, effective filter bytes, current sources, voiceActive). The
        // null-repVoice path fires exactly ONCE on the active->inactive
        // transition — a steady idle part never writes, so the frame keeps its
        // frozen tail until the next note.
        if (uiTelTrack)
            uiTelUpdateObservables (repVoice, lastModSources_[(size_t) p].data());
    }

    // FX state (types/enabled/params) changed this block: refresh the tail
    // cache so the host's getTailLengthSeconds follows the patch (reverb on ->
    // longer bounce tail; all-None -> floor).
    if (anyFxDirtied)
        recomputeTailCache();
}

//==========================================================================
// UI live-modulation telemetry (docs/LIVE_MOD_FEEDBACK_DESIGN.md).
// Thread contract (fixed):
//   * uiTelServiceStage / uiTelAppendHistory / uiTelUpdateObservables run on
//     the AUDIO thread, called from renderPartFx for the tracked part only.
//     All writes are bounded fixed-size stores under the seqlock — no
//     allocation, no locks, nothing that could block the audio thread.
//   * readUiTelemetry / resetUiTelemetry / setUiTelemetryPart run on the
//     message thread (the editor's LiveFeedbackHub + the patch-load reset
//     hooks). The reader is a bounded 64-retry seqlock read — the exact
//     Part::readPendingConfig discipline (single writer, single reader).
void SynthEngine::uiTelServiceStage (int p)
{
    // exchange (acq_rel) check-and-clear: a plain load()+store(false) could
    // drop a reset staged by the message thread between the two ops (a lost
    // wipe on a rapid patch switch) — same reasoning as the
    // osFactorDirty_/fxDirty_ services above.
    const bool resetReq = uiTelResetReq_.exchange (false, std::memory_order_acq_rel);
    if (uiTelWrittenPart_ == p && ! resetReq)
        return;   // already servicing this part and no wipe pending

    uiTelSeq_.fetch_add (1, std::memory_order_relaxed);          // begin (odd)
    std::atomic_thread_fence (std::memory_order_release);
    // Fixed-size wipe (the ~4 KB frame is zeroed with stores only; this fires
    // once per reset / part switch, never per block).
    uiTel_ = parvati::ModTelemetrySnapshot {};
    uiTel_.epoch = uiTelemetryEpoch_.load (std::memory_order_relaxed);
    uiTel_.part  = p;
    // historyHead / historyCount are already zeroed by the wipe above.
    uiTelDecim_    = 0;
    uiTelWasActive_ = false;
    uiTelVoiceSlot_ = -1;   // sticky pick dies with the wipe: the next append re-picks
    uiTelVoiceSeq_  = 0;
    uiTelIdlePrev_.fill (0);   // the drag-out restarts from a zero buffer
    uiTelIdleSeeded_ = false;
    uiTelLiveSeen_   = false;  // no live append since the wipe: the next idle must NOT seed
    uiTelNoteSeqLast_ = 0;
    std::atomic_thread_fence (std::memory_order_release);
    uiTelSeq_.fetch_add (1, std::memory_order_release);          // end (even, publishes)
    uiTelWrittenPart_ = p;
}

bool SynthEngine::uiTelAppendHistory (const uint8_t* effSrcs, const parvati::Sequencer& noteSeq,
                                     int noteSeqOverride)
{
    // Decimate: one append per kUiTelDecimBlocks internal ticks (~81.7 Hz at
    // the 980.4 Hz control cadence -> 128 samples ~= 1.57 s window). Returns
    // whether an append LANDED (the idle drag-out advances its decay state
    // only on real appends, keeping the fall pace equal to the scroll pace).
    if (++uiTelDecim_ < kUiTelDecimBlocks)
        return false;
    uiTelDecim_ = 0;

    constexpr int kLen = parvati::ModTelemetrySnapshot::kHistoryLen;
    const int idx = uiTel_.historyHead;   // guarded ring metadata (read pre-lock, re-stamped inside)

    uiTelSeq_.fetch_add (1, std::memory_order_relaxed);          // begin (odd)
    std::atomic_thread_fence (std::memory_order_release);
    // effSrcs holds MOD_SRC_LAST(31) bytes; the frame's spare slot carries
    // the NOTE-SEQ preview (kNoteSeqSlot): the tracked part's now-sounding
    // sounding sequencer note (0..127 -> 0..254, 0 = rest/gap) — a melody
    // trace with rests as gaps, driven PURELY by observation of the same
    // Sequencer object the audio path fires from.
    {
        // The spare-slot value this append (live note*2, or the idle decay
        // override); remembered in uiTelNoteSeqLast_ so the idle drag-out can
        // fall from where the melody trace actually was.
        const uint8_t v = (noteSeqOverride >= 0)
            ? static_cast<uint8_t> (noteSeqOverride)
            : ((noteSeq.liveNote() <= 127)
                   ? static_cast<uint8_t> (noteSeq.liveNote() * 2) : uint8_t { 0 });
        uiTelNoteSeqLast_ = v;
        uiTel_.history[(size_t) parvati::ModTelemetrySnapshot::kNoteSeqSlot * (size_t) kLen
                       + (size_t) idx] = v;
        uiTel_.sources[(size_t) parvati::ModTelemetrySnapshot::kNoteSeqSlot] = v;
    }
    for (int src = 0; src < ambika::dsp::MOD_SRC_LAST; ++src)
    {
        uiTel_.history[(size_t) src * (size_t) kLen + (size_t) idx] = effSrcs[(size_t) src];
        uiTel_.sources[(size_t) src] = effSrcs[(size_t) src];
    }
    uiTel_.historyHead = (idx + 1 >= kLen) ? 0 : idx + 1;
    if (uiTel_.historyCount < kLen)
        ++uiTel_.historyCount;
    std::atomic_thread_fence (std::memory_order_release);
    uiTelSeq_.fetch_add (1, std::memory_order_release);          // end (even, publishes)
    return true;
}

void SynthEngine::uiTelUpdateObservables (AmbikaVoice* repVoice, const uint8_t* currentSources)
{
    const bool active = repVoice != nullptr;
    // Transition gate: write every block while active, exactly ONCE on the
    // active->inactive fall, never on a steady idle part (the frame then keeps
    // its frozen tail values — matching the frozen history window).
    if (! active && ! uiTelWasActive_)
        return;

    uiTelSeq_.fetch_add (1, std::memory_order_relaxed);          // begin (odd)
    std::atomic_thread_fence (std::memory_order_release);
    if (active)
    {
        for (int e = 0; e < 3; ++e)
        {
            uiTel_.envStage[(size_t) e] = repVoice->envelopeStage (e);
            // Progress within the stage: phase/65536 while the segment is
            // advancing; 1.0 while parked (SUSTAIN/DEAD hold a zero increment
            // and never wrap — the marker then rests at the segment's end).
            uiTel_.envProgress[(size_t) e] = repVoice->envelopePhaseIncrement (e) > 0
                ? static_cast<float> (repVoice->envelopePhase (e)) * (1.0f / 65536.0f)
                : 1.0f;
            uiTel_.envLevel[(size_t) e] =
                static_cast<float> (repVoice->envelopeValueByte (e)) * (1.0f / 255.0f);
        }
        // EFFECTIVE filter bytes (modulation-applied — the same bytes the
        // analog filter consumes this block, not the knob bytes).
        uiTel_.effCutoff    = repVoice->effectiveCutoff();
        uiTel_.effResonance = repVoice->effectiveResonance();
        uiTel_.filterMode   = repVoice->filterMode();
        // EFFECTIVE OSC parameter bytes (modulation-applied — the same bytes
        // UpdateDestinations feeds the oscillators' set_parameter() this
        // block). Same readout discipline as the filter bytes above; drives
        // the OSC waveform preview's live overlay.
        uiTel_.effOscParam[0] = repVoice->effectiveOscParameter (0);
        uiTel_.effOscParam[1] = repVoice->effectiveOscParameter (1);
        // Current sources: the freshest effective bytes for this part (the
        // final sub-chunk's effSrcs, mirrored into lastModSources_ above).
        for (int src = 0; src < ambika::dsp::MOD_SRC_LAST; ++src)
            uiTel_.sources[(size_t) src] = currentSources[(size_t) src];
    }
    uiTel_.voiceActive = active;
    std::atomic_thread_fence (std::memory_order_release);
    uiTelSeq_.fetch_add (1, std::memory_order_release);          // end (even, publishes)
    uiTelWasActive_ = active;
}

void SynthEngine::resetUiTelemetry()
{
    // Message thread. The EPOCH bump is what the reader observes immediately
    // (readUiTelemetry fails on a stale epoch before the audio thread has
    // serviced anything, so the UI hides its overlays for that window). The
    // request flag then drives the audio-thread wipe at the next renderPartFx.
    uiTelemetryEpoch_.fetch_add (1, std::memory_order_relaxed);
    uiTelResetReq_.store (true, std::memory_order_release);
}

void SynthEngine::setUiTelemetryPart (int part)
{
    // Message thread. The audio thread's service stage observes the change on
    // the next block and clears the frame for the new part (the reader reports
    // invalid for that window — see readUiTelemetry's part check).
    uiTelPart_.store (juce::jlimit (0, kNumParts - 1, part), std::memory_order_relaxed);
}

bool SynthEngine::readUiTelemetry (parvati::ModTelemetrySnapshot& out) const
{
    // Message thread (the editor's LiveFeedbackHub poll). Bounded-retry
    // seqlock read — the Part::readPendingConfig discipline: copy the plain
    // frame, re-check the sequence, retry on a mismatch. Unlike the pending
    // config there is no lastGood fallback: a torn read simply reports false
    // and the caller polls again next tick (the hub keeps its previous cache).
    const int tracked = uiTelPart_.load (std::memory_order_relaxed);
    if (tracked < 0)
        return false;   // no part tracked yet (the editor has not wired the hub)

    constexpr int kLen  = parvati::ModTelemetrySnapshot::kHistoryLen;
    constexpr int kSrcs = parvati::ModTelemetrySnapshot::kNumSources;

    for (int attempt = 0; attempt < 64; ++attempt)
    {
        const uint32_t s1 = uiTelSeq_.load (std::memory_order_acquire);
        if (s1 & 1u)
            continue;                                       // writer mid-update
        parvati::ModTelemetrySnapshot copy = uiTel_;         // guarded by the seq check below
        std::atomic_thread_fence (std::memory_order_acquire);
        if (uiTelSeq_.load (std::memory_order_acquire) != s1)
            continue;                                       // torn: retry

        // Validity (see the public contract in the header): a stale epoch = a
        // reset landed that the audio thread has not serviced yet; a part
        // mismatch = the tracked part switched and the service has not run.
        // Both report false so the UI hides its live overlays for that window.
        if (copy.epoch != uiTelemetryEpoch_.load (std::memory_order_relaxed))
            return false;
        if (copy.part != tracked)
            return false;

        // Linearize the ring OLDEST-FIRST into the caller's frame. While the
        // ring is not yet full the valid samples sit left-aligned at [0,count)
        // and head == count; once full, head wraps to the OLDEST entry.
        const int count  = juce::jmin (copy.historyCount, kLen);
        const int oldest = (count < kLen) ? 0 : copy.historyHead;
        for (int src = 0; src < kSrcs; ++src)
        {
            const uint8_t* ring = copy.history + (size_t) src * (size_t) kLen;
            uint8_t* dst       = out.history  + (size_t) src * (size_t) kLen;
            for (int i = 0; i < count; ++i)
                dst[(size_t) i] = ring[(size_t) ((oldest + i) % kLen)];
            // Zero the unused tail so a caller reusing one snapshot object can
            // never observe a stale longer window after a reset/clear.
            for (int i = count; i < kLen; ++i)
                dst[(size_t) i] = 0;
        }
        out.historyCount = count;
        out.historyHead  = 0;   // linearized: no ring semantics in the UI frame
        out.epoch        = copy.epoch;
        out.part         = copy.part;
        for (int s = 0; s < kSrcs; ++s)
            out.sources[(size_t) s] = copy.sources[(size_t) s];
        for (int e = 0; e < 3; ++e)
        {
            out.envStage[(size_t) e]    = copy.envStage[(size_t) e];
            out.envProgress[(size_t) e] = copy.envProgress[(size_t) e];
            out.envLevel[(size_t) e]    = copy.envLevel[(size_t) e];
        }
        out.effCutoff     = copy.effCutoff;
        out.effResonance  = copy.effResonance;
        out.filterMode    = copy.filterMode;
        out.effOscParam[0] = copy.effOscParam[0];
        out.effOscParam[1] = copy.effOscParam[1];
        out.voiceActive   = copy.voiceActive;
        return true;
    }
    return false;   // retries exhausted (sustained writer churn — poll again)
}
// Tail-length cache. Pure math over the staged fxState atomics: max over every
// part's ENABLED slots of tailSecondsForFx, clamped to [floor, cap]. Called on
// the audio thread (renderPartFx after a dirty service / processTransport on a
// tempo move) — relaxed loads + one relaxed store, no allocation. A disabled
// slot is a passthrough and contributes nothing.
void SynthEngine::recomputeTailCache() noexcept
{
    const double bpm = tailBpmCache_.load (std::memory_order_relaxed);
    double worst = 0.0;
    for (int p = 0; p < kNumParts; ++p)
    {
        const auto& fx = parts_[(size_t) p].fxState;
        for (int s = 0; s < kNumFxSlots; ++s)
        {
            if (fx.slotEnabled[(size_t) s].load (std::memory_order_relaxed) == 0)
                continue;
            const auto t = static_cast<FxType> (fx.slotType[(size_t) s].load (std::memory_order_relaxed));
            std::array<float, kNumFxSlotParams> param {};
            for (int k = 0; k < kNumFxSlotParams; ++k)
                param[k] = (float) fx.slotParam[(size_t) s][(size_t) k].load (std::memory_order_relaxed) / 127.0f;
            const double t60 = tailSecondsForFx (t, param, bpm);
            if (t60 > worst) worst = t60;
        }
    }
    tailSecondsCache_.store ((float) clampTailSeconds (worst), std::memory_order_relaxed);
}

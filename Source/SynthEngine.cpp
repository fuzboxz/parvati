// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See SynthEngine.h.

#include "SynthEngine.h"

#include "ParameterLayout.h"   // getControllerInitPatchBytes (audible init patch)

SynthEngine::SynthEngine()
{
    for (int i = 0; i < kNumVoices; ++i)
        addVoice (new AmbikaVoice());

    // Tag each voice with its FIXED voicecard (Ambika hardware: 6 voicecards,
    // each with individual outputs). Unlike the Part assignment (which can
    // change via the Multi editor), the voicecard never changes — it is set
    // once here from the voice index via the fixed block mapping.
    for (int i = 0; i < kNumVoices; ++i)
        if (auto* av = getAmbikaVoice (i))
            av->setVoiceCard (voiceCardForIndex (i));

    addSound (new AmbikaSound());
    setNoteStealingEnabled (true);  // we steal within a Part anyway

    // Default voice allocation: SINGLE-PART — all 6 voicecards on Part 0, so a
    // player on MIDI channel 1 gets the full hardware polyphony (6 voices) out
    // of the box. This differs from the firmware factory multi
    // (controller/multi.cc: Part0=0x15, Part1=0x2a, a 3+3 multitimbral split),
    // which remains available by loading a factory .MUL or assigning voicecards
    // per Part via the Multi page. rebuildVoiceAllocation() maps each voicecard
    // to its single Parvati voice (voice i == voicecard i) with first-wins
    // across Parts.
    constexpr uint8_t kInitVoiceAllocation[kNumParts] = { 0x3f, 0, 0, 0, 0, 0 };
    for (int p = 0; p < kNumParts; ++p)
    {
        auto& part = parts_[p];
        part.voiceAllocation.store (kInitVoiceAllocation[p]);
        // Default: part i -> MIDI channel i+1 (so channel 1 -> Part 0).
        part.midiChannel.store  (static_cast<uint8_t> (p + 1));
        part.keyrangeLow.store  (0);
        part.keyrangeHigh.store (127);

        // Wire this Part's arp/seq generated notes to trigger a voice WITHIN
        // this Part (bypassing channel routing — generated notes always belong
        // to the generating Part).
        const int partIdx = p;
        parts_[p].arp.setNoteOnCallback ([this, partIdx] (int, int note, uint8_t velocity)
        {
            const int ch = juce::jmax (1, static_cast<int> (parts_[partIdx].midiChannel.load()));
            triggerNoteInPart (partIdx, note, velocity / 127.0f, ch);
        });
        parts_[p].arp.setNoteOffCallback ([this, partIdx] (int, int note)
        {
            const int ch = juce::jmax (1, static_cast<int> (parts_[partIdx].midiChannel.load()));
            releaseNoteInPart (partIdx, note, ch);
        });
        parts_[p].seq.setNoteOnCallback ([this, partIdx] (int, int note, uint8_t velocity)
        {
            const int ch = juce::jmax (1, static_cast<int> (parts_[partIdx].midiChannel.load()));
            triggerNoteInPart (partIdx, note, velocity / 127.0f, ch);
        });
        parts_[p].seq.setNoteOffCallback ([this, partIdx] (int, int note)
        {
            const int ch = juce::jmax (1, static_cast<int> (parts_[partIdx].midiChannel.load()));
            releaseNoteInPart (partIdx, note, ch);
        });
    }

    rebuildVoiceAllocation();
}

void SynthEngine::prepare (double sampleRate, int blockSize)
{
    setCurrentPlaybackSampleRate (sampleRate);

    // One mono buffer per voicecard, sized to the host block. renderVoices is
    // called with sub-blocks that tile [0, blockSize) without overlap, so each
    // buffer only needs `blockSize` samples of capacity.
    for (auto& b : voiceCardBuffers_)
        b.setSize (1, juce::jmax (1, blockSize), false, true, true);

    for (auto* v : voices)
        if (auto* av = dynamic_cast<AmbikaVoice*> (v))
            av->prepare (sampleRate, blockSize);

    transport_.prepare (sampleRate);

    // Tag each voice with its Part, and seed each Part's patch/part storage
    // from the faithful init patch (once).
    for (int p = 0; p < kNumParts; ++p)
        for (int vi : parts_[p].voiceIndices)
            if (auto* av = getAmbikaVoice (vi))
                av->setPartIndex (p);

    if (! partsSeeded_)
    {
        // Seed EVERY Part with the CONTROLLER init patch (Part::InitPatch(DEFAULT):
        // osc1=Saw, audible) + the firmware init_part PartData (volume 120; arp
        // octave 1 / resolution 10; seq length 16; POLY), and mirror those arp/seq
        // values into the live per-part Arpeggiator/Sequencer OBJECTS so
        // loadPartIntoApvts (which reads arp/seq from the objects) sees consistent
        // state. Previously this seeded the VOICECARD silence fallback
        // (osc1=None, inaudible) for Parts 1..5 and only set Part 0 via the APVTS
        // sync, so a freshly-loaded plugin left Parts 1..5 silent until visited --
        // incorrect vs the firmware (init_part, part.cc:83-100).
        const uint8_t* const initPatch = getControllerInitPatchBytes();
        for (int p = 0; p < kNumParts; ++p)
        {
            parts_[p].patchBytes.loadFrom (initPatch);
            parts_[p].partBytes.fill (0);
            parts_[p].partBytes[0]  = 120;   // volume (init_part)
            parts_[p].partBytes[7]  = 0;     // arp / sequencer mode
            parts_[p].partBytes[8]  = 0;     // arp direction
            parts_[p].partBytes[9]  = 1;     // arp octave range (init_part)
            parts_[p].partBytes[10] = 0;     // arp pattern
            parts_[p].partBytes[11] = 10;    // arp resolution / divider (init_part => 1/16)
            parts_[p].partBytes[12] = 16;    // sequence length 1
            parts_[p].partBytes[13] = 16;    // sequence length 2
            parts_[p].partBytes[14] = 16;    // sequence length 3
            parts_[p].partBytes[15] = 1;     // polyphony_mode = POLY
            // Mirror the init arp/seq config into BOTH the live objects AND
            // pendingConfig_ (the message-thread-authoritative config the audio
            // thread applies via servicePendingConfig, and that serialize /
            // loadPartIntoApvts read). Seeding pendingConfig_ here keeps it in
            // sync with the live objects, so a later live edit (which stages only
            // its own field into pendingConfig_ + flags configDirty_) does not
            // re-apply stale defaults and clobber these init values.
            parts_[p].arp.setMode (0);              parts_[p].seq.setMode (0);
            parts_[p].arp.setDirection (0);
            parts_[p].arp.setOctave (1);
            parts_[p].arp.setPattern (0);
            parts_[p].arp.setResolution (10);
            parts_[p].seq.setSequenceLength (0, 16);
            parts_[p].seq.setSequenceLength (1, 16);
            parts_[p].seq.setSequenceLength (2, 16);
            auto& pc = parts_[p].pendingConfig_;
            pc.arpMode = 0;  pc.arpDirection = 0;  pc.arpOctave = 1;
            pc.arpPattern = 0;  pc.arpResolution = 10;
            pc.seqLength[0] = 16;  pc.seqLength[1] = 16;  pc.seqLength[2] = 16;
            // seqData stays 0 (matches the init zero-fill); configDirty_ stays
            // false: pendingConfig_ == live objects, nothing for the AT to apply.

            // Pre-size the per-Part voice-index vector to the voice count so a
            // rebuild (rebuildVoiceAllocation, audio thread) never triggers a
            // heap reallocation on the audio thread.
            parts_[p].voiceIndices.reserve (kNumVoices);
        }
        partsSeeded_ = true;
    }

    // Arm every Part's allocator for its (default POLY) mode + voice set.
    for (int p = 0; p < kNumParts; ++p)
        initAllocator (parts_[p]);
}

//==========================================================================
// APVTS byte bridge — CURRENT part only (also mirrors into Part storage).
void SynthEngine::applyPatchByte (int offset, uint8_t value)
{
    auto& part = parts_[currentPart_];
    if (offset >= 0 && offset < 112) part.patchBytes[(size_t) offset] = value;
    // DEFER the voice write to the audio thread: setPatchByte mutates voice_.patch_
    // which the renderer reads every block, so writing it here (message thread)
    // was a torn read. Stage it in Part storage (above) + flag frameDirty_; the
    // audio thread pushes the full frame (pushPartBytesToVoices) at the block top.
    // (release publishes the byte write to the audio-thread acquire.)
    part.frameDirty_.store (true, std::memory_order_release);
}

void SynthEngine::applyPartByte (int offset, uint8_t value)
{
    auto& part = parts_[currentPart_];
    // Capture the previous polyphony mode before the generic write so we only
    // defer work on an ACTUAL change (syncAllParamsToEngine re-applies every
    // param, including part_polyphony, with its current value — re-servicing
    // that would needlessly rebuild+push every block and perturb render state).
    const uint8_t prevMode = (offset == 15) ? part.partBytes[15] : 0;
    if (offset >= 0 && offset < 84) part.partBytes[(size_t) offset] = value;
    // PartData byte 15 = polyphony_mode. Defer the mode engage (and, for CHAIN,
    // the voice-set rebuild) to the audio thread via markAllocationDirty() so
    // voiceIndices is never mutated under a concurrent audio-thread reader. The
    // audio-thread service syncs polyphonyMode from partBytes[15] and rebuilds.
    if (offset == 15)
    {
        if (value != prevMode)   // only on a real polyphony-mode change
            markAllocationDirty();
        return;
    }
    // DEFER the voice write to the audio thread (same fence as applyPatchByte):
    // setPartByte mutates voice_.part_ which the renderer reads; writing it on the
    // message thread was a torn read. Stage in Part storage + frameDirty_.
    part.frameDirty_.store (true, std::memory_order_release);
}

void SynthEngine::applyTempo (double bpm)
{
    for (auto* v : voices)
        if (auto* av = dynamic_cast<AmbikaVoice*> (v))
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
    parts_[currentPart_].writePendingConfig ([mode] (auto& c) { c.arpMode = mode; });
    parts_[currentPart_].configDirty_.store (true, std::memory_order_release);
}

void SynthEngine::stageArpSeqFromPartBytes (int part)
{
    // Message-thread entry point for FILE LOADS (.MUL / .parvati multi): stage
    // the full arp/seq config from a Part's PartData into pendingConfig_ + flag
    // configDirty_, exactly like the live setters do. The audio thread is the
    // sole writer of the live Arpeggiator/Sequencer objects (it services
    // configDirty_), so writing them here would race the clock loop; staging
    // keeps pendingConfig_ (the serialize / loadPartIntoApvts source) in sync
    // with the loaded values too. Reads parts_[part].partBytes (atomic).
    if (! ok (part))
        return;
    auto& p  = parts_[part];
    // Stage under the pendingConfig_ seqlock so the audio-thread reader
    // (servicePendingConfig) never sees a torn snapshot.
    p.writePendingConfig ([&] (Part::PendingConfig& pc) {
        pc.arpMode       = p.partBytes[7];
        pc.arpDirection  = p.partBytes[8];
        pc.arpOctave     = p.partBytes[9];
        pc.arpPattern    = p.partBytes[10];
        pc.arpResolution = p.partBytes[11];
        pc.seqLength[0]  = p.partBytes[12];
        pc.seqLength[1]  = p.partBytes[13];
        pc.seqLength[2]  = p.partBytes[14];
        for (int i = 0; i < 64; ++i)
            pc.seqData[i] = p.partBytes[16 + i];
    });
    p.configDirty_.store (true, std::memory_order_release);
}

//==========================================================================
// Host plugin-state capture/restore (full 6-Part multitimbral persistence).
static const char kEngineStateMagic[4] = { 'P','V','S','T' };

void SynthEngine::captureState (juce::MemoryBlock& dest) const
{
    // Byte-oriented payload (endian-independent): magic + version + current
    // part, then per Part: patch[112], part[84] (with the arp/seq region overlaid
    // from the authoritative pendingConfig_), midi channel / keyzone / voice
    // allocation. polyphony rides in partBytes[15]; arp/seq lives in
    // pendingConfig_ (overlaid here) and is re-staged on restore.
    juce::MemoryOutputStream out (dest, false);
    out.write (kEngineStateMagic, 4);
    out.writeByte (1);                                                       // version
    out.writeByte ((char) currentPart_);
    for (int p = 0; p < kNumParts; ++p)
    {
        const auto& part = parts_[p];
        std::array<uint8_t, 112> patch {};   part.patchBytes.copyTo (patch);  out.write (patch.data(), 112);
        std::array<uint8_t, 84>  pb   {};   part.partBytes.copyTo (pb);
        const auto pc = part.readPendingConfig();   // overlay authoritative arp/seq (seqlock-protected read)
        pb[7]  = pc.arpMode;  pb[8]  = pc.arpDirection;  pb[9]  = pc.arpOctave;
        pb[10] = pc.arpPattern; pb[11] = pc.arpResolution;
        pb[12] = pc.seqLength[0]; pb[13] = pc.seqLength[1]; pb[14] = pc.seqLength[2];
        for (int i = 0; i < 64; ++i) pb[(size_t) (16 + i)] = pc.seqData[i];
        out.write (pb.data(), 84);
        out.writeByte ((char) part.midiChannel.load (std::memory_order_relaxed));
        out.writeByte ((char) part.keyrangeLow.load (std::memory_order_relaxed));
        out.writeByte ((char) part.keyrangeHigh.load (std::memory_order_relaxed));
        out.writeByte ((char) part.voiceAllocation.load (std::memory_order_relaxed));
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
    if (in.readByte() != 1)   // version
        return false;
    const int savedCurrent = in.readByte();
    for (int p = 0; p < kNumParts; ++p)
    {
        auto& part = parts_[p];
        std::array<uint8_t, 112> patch {};
        if (in.read (patch.data(), 112) != 112) return false;
        part.patchBytes.loadFrom (patch.data());
        std::array<uint8_t, 84> pb {};
        if (in.read (pb.data(), 84) != 84) return false;
        part.partBytes.loadFrom (pb.data());
        stageArpSeqFromPartBytes (p);   // re-stage arp/seq from the restored PartData
        part.midiChannel.store  ((uint8_t) in.readByte());
        part.keyrangeLow.store  ((uint8_t) in.readByte());
        part.keyrangeHigh.store ((uint8_t) in.readByte());
        part.voiceAllocation.store ((uint8_t) in.readByte());
    }
    setCurrentPart (juce::jlimit (0, kNumParts - 1, savedCurrent));
    resetAllVoices();        // clean slate for the restored config (deferred to AT)
    markAllocationDirty();   // AT rebuilds voiceIndices + pushes every Part's frame
    return true;
}

void SynthEngine::setCurrentPart (int part)
{
    if (ok (part))
        currentPart_ = part;
}

//==========================================================================
// Voice allocation from the firmware 6-voicecard bitmask.
void SynthEngine::rebuildVoiceAllocation()
{
    // 1 voice per voicecard (voice i == voicecard i); faithful Ambika 6-note
    // max polyphony total across Parts.
    bool claimed[kNumParts] = {};   // voicecard already taken by an earlier Part
    for (int p = 0; p < kNumParts; ++p)
    {
        auto& part = parts_[p];
        part.voiceIndices.clear();
        const uint8_t voiceAllocation = part.voiceAllocation.load (std::memory_order_relaxed);
        for (int vc = 0; vc < kNumParts; ++vc)
        {
            if ((voiceAllocation & (1u << vc)) == 0)
                continue;
            if (claimed[vc])          // first-wins (firmware AssignVoices)
                continue;
            claimed[vc] = true;
            part.voiceIndices.push_back (vc);   // voice i == voicecard i
        }
    }
    // CHAIN (Option A): each CHAIN Part doubles its voice set by claiming FREE
    // voicecards as an internal "chain partner" — the plugin equivalent of the
    // firmware's 2-unit chain (2N voices via midi_dispatcher.ForwardNote,
    // midi_dispatcher.h:228) but fully internal (no MIDI output). Base claims
    // (above) are honoured first, so a CHAIN Part only takes voicecards no other
    // Part uses; if free cards run out it gets a partial chain (graceful).
    for (int p = 0; p < kNumParts; ++p)
    {
        if (parts_[p].polyphonyMode != 4 /*CHAIN*/) continue;
        const size_t baseN = parts_[p].voiceIndices.size();
        if (baseN == 0) continue;
        size_t partnerVoices = 0;
        for (int vc = 0; vc < kNumParts && partnerVoices < baseN; ++vc)
        {
            if (claimed[vc]) continue;          // only truly free cards
            claimed[vc] = true;
            parts_[p].voiceIndices.push_back (vc);
            ++partnerVoices;
        }
    }
    // Re-tag each voice with its (possibly changed) Part.
    for (int p = 0; p < kNumParts; ++p)
        for (int vi : parts_[p].voiceIndices)
            if (auto* av = getAmbikaVoice (vi))
                av->setPartIndex (p);

    // Voice sets changed -> re-arm every Part's allocator for its mode.
    for (int p = 0; p < kNumParts; ++p)
    {
        initAllocator (parts_[p]);
        parts_[p].voiceCount_.store (static_cast<int> (parts_[p].voiceIndices.size()), std::memory_order_relaxed);
    }
}

void SynthEngine::setPartVoiceAllocation (int part, uint8_t bitmask)
{
    if (! ok (part))
        return;

    const uint8_t old = parts_[part].voiceAllocation.load (std::memory_order_relaxed);
    if (old == bitmask)
        return;   // no change -> nothing to defer

    parts_[part].voiceAllocation.store (bitmask, std::memory_order_relaxed);

    // Exclusive voicecard ownership: a card newly claimed by `part` (added) is
    // removed from every OTHER Part, so a voicecard belongs to AT MOST one Part.
    // (The default Part0=0x3f would otherwise silently starve any Part that later
    // claims vc0..5.) rebuildVoiceAllocation's first-wins claimed[] stays as a
    // safety net, but this keeps the stored bitmasks themselves consistent so
    // getPartVoiceAllocation reflects the exclusive split immediately.
    const uint8_t added = static_cast<uint8_t> (bitmask & static_cast<uint8_t> (~old));
    if (added)
        for (int q = 0; q < kNumParts; ++q)
            if (q != part)
            {
                const uint8_t qa = parts_[q].voiceAllocation.load (std::memory_order_relaxed);
                if (qa & added)
                    parts_[q].voiceAllocation.store (static_cast<uint8_t> (qa & static_cast<uint8_t> (~added)),
                                                     std::memory_order_relaxed);
            }

    markAllocationDirty();   // defer the rebuild to the audio thread (next block)
}

void SynthEngine::pushPartBytesToVoices (int part)
{
    if (! ok (part))
        return;
    // Audio-thread path (called only from the allocationDirty service): push this
    // Part's stored patch/part bytes into every voice owned by it. Iterate the
    // Synthesiser's stable `voices` array filtered by partIndex (NOT voiceIndices)
    // so this is robust even if called before a rebuild has settled voiceIndices.
    // (polyphonyMode / voiceAllocation are synced + rebuilt by the service before
    // this is called, so voice ownership is final here.)
    const auto& p = parts_[part];
    for (auto* v : voices)
    {
        auto* av = dynamic_cast<AmbikaVoice*> (v);
        if (av == nullptr || av->getPartIndex() != part)
            continue;
        for (int o = 0; o < 112; ++o)
            av->setPatchByte (o, p.patchBytes[(size_t) o]);
        // Only the 7-byte dsp::Part is voicecard-relevant (volume/octave/tuning/
        // spread/_/legato/portamento at offsets 0..6); the rest of PartData is
        // controller-side (arp/seq/polyphony) and MUST NOT be pushed -- pushing
        // offsets 7..83 was an out-of-bounds write that clobbered the Voice's
        // envelopes/LFOs/oscillators on every patch/mode switch.
        for (int o = 0; o < static_cast<int>(sizeof (ambika::dsp::Part)); ++o)
            av->setPartByte (o, p.partBytes[(size_t) o]);
        // Re-prime the envelope increments from the just-pushed patch: an idle
        // voice is gated out of renderNextBlock, so without this the pushed A/D/S/R
        // bytes leave its attack increment stale/0 and the next note is SILENT
        // (the standalone "dead after a voice-mode / template switch" glitch).
        av->reprimeEnvelopes();
    }
}

void SynthEngine::resetAllVoices()
{
    // DEFER the kill to the audio thread. stopNote(.,false) runs Voice::Kill +
    // clearCurrentNote + FIFO clear on a voice the audio thread may be
    // mid-rendering -- calling it here (message thread) raced the audio thread
    // and crashed in hosts (e.g. Ableton). The kill is serviced at the top of
    // the next processTransport() block; the caller has already pushed the new
    // patch bytes + flagged allocationDirty_, whose service rebuilds and
    // re-primes (pushPartBytesToVoices) every voice.
    resetAllVoicesPending_.store (true, std::memory_order_release);
}

//==========================================================================
// MIDI routing (channel + keyzone -> Part), faithful to multi.h PartMapping.
int SynthEngine::findPartForNote (int channel, int note) const
{
    for (int p = 0; p < kNumParts; ++p)
    {
        const auto& pm = parts_[p];
        // receive_channel: Omni (0) or exact 1-based channel match. (midiChannel /
        // keyrange are atomic: written on the message thread, read here on audio.)
        const uint8_t ch = pm.midiChannel.load();
        const bool chanOk = (ch == 0) || (channel == ch);
        if (! chanOk) continue;
        // accept_note.
        if (note >= pm.keyrangeLow.load() && note <= pm.keyrangeHigh.load())
            return p;   // first-match wins (firmware NoteOn routing)
    }
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

void SynthEngine::triggerNoteInPart (int part, int note, float velocity, int incomingChannel)
{
    if (! ok (part)) return;
    auto& p = parts_[part];
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
        uint8_t drift = 0;   // 14-bit units (1/128 semitone); uint8 wrap is faithful
        for (int vi : p.voiceIndices)
            if (auto* av = getAmbikaVoice (vi))
            {
                av->setLegatoNext (legato);
                av->setSpreadDrift (drift);
                // Overlap on a sounding voice: re-trigger WITHOUT the kill that
                // startVoice does (stopNote(0,false) -> Voice::Kill zeroes the
                // envelope, so the legato Trigger -- which skips the re-attack --
                // would then render silence). The first note (voice idle) still
                // uses startVoice for a fresh attack.
                if (legato && av->isVoiceActive())
                    av->retriggerNote (sound, note, velocity);
                else
                    startVoice (av, sound, channel, note, velocity);
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
        if (auto* av = getAmbikaVoice (p.voiceIndices[(size_t) v0])) { av->setSpreadDrift (0);      startVoice (av, sound, channel, note, velocity); }
        if (auto* av = getAmbikaVoice (p.voiceIndices[(size_t) v1])) { av->setSpreadDrift (spread); startVoice (av, sound, channel, note, velocity); }
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
            startVoice (av, sound, channel, note, velocity);
        }
    }
    else  // POLY / CYCLIC
    {
        // Firmware part.cc:721: drift = voice_index * spread.
        if (idx < n)
            if (auto* av = getAmbikaVoice (p.voiceIndices[(size_t) idx]))
            {
                av->setSpreadDrift (static_cast<uint8_t> (idx * p.partBytes[3]));
                startVoice (av, sound, channel, note, velocity);
            }
    }
}

void SynthEngine::releaseNoteInPart (int part, int note, int incomingChannel)
{
    if (! ok (part)) return;
    auto& p = parts_[part];
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
            for (int vi : p.voiceIndices)
                if (auto* av = getAmbikaVoice (vi)) stopVoice (av, 1.0f, true);
        }
        else if (topNote == n8)
        {
            const uint8_t newNote = p.monoStack.most_recent_note().note;
            const float   newVel  = p.monoStack.most_recent_note().velocity / 127.0f;
            const int channel = juce::jlimit (1, 16, incomingChannel);
            auto* sound = getNumSounds() > 0 ? getSound (0).get() : nullptr;
            const uint8_t spread = p.partBytes[3];   // firmware part.cc:760: retrigger drift
            uint8_t drift = 0;
            for (int vi : p.voiceIndices)
                if (auto* av = getAmbikaVoice (vi))
                {
                    av->setLegatoNext (true);
                    av->setSpreadDrift (drift);
                    // Legato slide-back to the prior held note on release: same
                    // no-kill retrigger (a kill would silence the legato Trigger).
                    if (av->isVoiceActive())
                        av->retriggerNote (sound, newNote, newVel);
                    else
                        startVoice (av, sound, channel, newNote, newVel);
                    drift += spread;
                }
        }
        return;
    }

    const int idx = p.polyAlloc.noteOff (n8);
    const int n   = static_cast<int> (p.voiceIndices.size());
    if (idx == 0xff) return;

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

void SynthEngine::noteOn (int midiChannel, int midiNoteNumber, float velocity)
{
    // processTransport has routed notes for Parts whose arp/sequencer is active
    // into their held-key stacks; only "play directly" notes reach here.
    const int part = findPartForNote (midiChannel, midiNoteNumber);
    if (part < 0) return;
    if (parts_[part].arp.isActive())
        parts_[part].arp.noteOn (midiNoteNumber, static_cast<uint8_t> (juce::jlimit (0, 127, (int) (velocity * 127))));
    else
        triggerNoteInPart (part, midiNoteNumber, velocity, midiChannel);
}

void SynthEngine::noteOff (int midiChannel, int midiNoteNumber, float /*velocity*/, bool /*allowTailOff*/)
{
    const int part = findPartForNote (midiChannel, midiNoteNumber);
    if (part < 0) return;
    if (parts_[part].arp.isActive())
        parts_[part].arp.noteOff (midiNoteNumber);
    else
        releaseNoteInPart (part, midiNoteNumber, midiChannel);
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
    // Host wheel 0..16383 (centre 8192) -> semitones via the per-voice bend range.
    const float semis = (static_cast<float> (wheelValue) - 8192.0f) / 8192.0f * bendRangeSemitones_;
    for (auto* v : voices)
    {
        auto* av = dynamic_cast<AmbikaVoice*> (v);
        if (av == nullptr) continue;
        if (av->isVoiceActive() && av->isPlayingChannel (midiChannel))
            av->setMpePitchBendSemitones (semis);
    }
}

void SynthEngine::handleChannelPressure (int midiChannel, int channelPressureValue)
{
    const float pressure = juce::jlimit (0.0f, 1.0f, static_cast<float> (channelPressureValue) / 127.0f);
    for (auto* v : voices)
    {
        auto* av = dynamic_cast<AmbikaVoice*> (v);
        if (av == nullptr) continue;
        if (av->isVoiceActive() && av->isPlayingChannel (midiChannel))
            av->setMpePressure (pressure);
    }
}

// GLOBAL mod-matrix write for the continuous controllers (mod wheel / breath /
// foot pedal). Faithful to firmware Part::WriteToAllVoices (part.cc:998):
// iterate every allocated voicecard and set the mod source. Parvati gives each
// Voice its own modulation_sources_[] (firmware's is a single shared static
// array), so writing EVERY voice reproduces that shared-global semantics — a CC
// move is immediately visible to all sounding notes AND persists in idle voices,
// so the next note-on inherits the current value. This works because note-on
// does NOT reset these sources: Voice::Trigger only writes VELOCITY/RANDOM, and
// Kill()/stopNote only touch the envelope — verified against the firmware
// voicecard/voice.cc Trigger (which behaves identically). `value0to254` is
// already the firmware-scaled value (controllerValue << 1).
void SynthEngine::applyGlobalModSource (int modSrcEnum, uint8_t value0to254)
{
    const uint8_t idx = static_cast<uint8_t> (modSrcEnum);
    for (auto* v : voices)
        if (auto* av = dynamic_cast<AmbikaVoice*> (v))
            av->setModulationSource (idx, value0to254);
}

void SynthEngine::handleController (int midiChannel, int controllerNumber, int controllerValue)
{
    // GLOBAL continuous controllers (firmware part.cc:367-377): mod wheel (CC1)
    // -> MOD_SRC_WHEEL, breath (CC2) -> MOD_SRC_WHEEL_2, foot pedal (CC4) ->
    // MOD_SRC_EXPRESSION, each value<<1 (0..254). These are CHANNEL-GLOBAL (not
    // per-note), so they do NOT use the per-channel MPE routing that pitch bend /
    // channel pressure / CC74 use. applyGlobalModSource writes the value to EVERY
    // voice (firmware WriteToAllVoices over all allocated voicecards); since
    // note-on does NOT reset these sources, a new note-on automatically inherits
    // the current wheel/breath/foot value (current-wheel pickup). Coexistence
    // with the parameter map: MidiParameterMap::handleBuffer runs independently
    // in processBlock; CC1/2/4 are UNMAPPED there (midi_cc_map[1/2/4]==255), so
    // this is the ONLY effect of these controllers.
    if (controllerNumber == 1)   // modulation wheel -> MOD_SRC_WHEEL
    {
        applyGlobalModSource (ambika::dsp::MOD_SRC_WHEEL,      static_cast<uint8_t> (controllerValue << 1));
        return;
    }
    if (controllerNumber == 2)   // breath controller -> MOD_SRC_WHEEL_2
    {
        applyGlobalModSource (ambika::dsp::MOD_SRC_WHEEL_2,    static_cast<uint8_t> (controllerValue << 1));
        return;
    }
    if (controllerNumber == 4)   // foot pedal -> MOD_SRC_EXPRESSION
    {
        applyGlobalModSource (ambika::dsp::MOD_SRC_EXPRESSION, static_cast<uint8_t> (controllerValue << 1));
        return;
    }

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
        for (auto* v : voices)
        {
            auto* av = dynamic_cast<AmbikaVoice*> (v);
            if (av == nullptr) continue;
            if (av->isVoiceActive() && av->isPlayingChannel (midiChannel))
                av->setMpeSlide (slide);
        }
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
        for (auto* v : voices)
            if (auto* av = dynamic_cast<AmbikaVoice*> (v))
                av->stopNote (0.0f, false);

    // Service deferred live-edit frame writes ON THE AUDIO THREAD: a knob edit
    // (applyPatchByte/applyPartByte) staged a Part's patch/part bytes + flagged
    // frameDirty_; push the full frame to that Part's voices now (audio-thread-
    // only pushPartBytesToVoices). Replaces the message-thread voice write that
    // raced the renderer (torn read). Order vs allocationDirty_ is irrelevant
    // (a Part dirtied by both is pushed twice, idempotently).
    for (int p = 0; p < kNumParts; ++p)
        if (parts_[p].frameDirty_.exchange (false, std::memory_order_acq_rel))
            pushPartBytesToVoices (p);

    // Service deferred global-option writes ON THE AUDIO THREAD: VCA curve /
    // smoothing / filter drive were staged by the message-thread setters
    // (setVcaExponential/setParameterSmoothing/setFilterDrive) so the per-voice
    // plain-field writes never race the renderer.
    if (optionsDirty_.exchange (false, std::memory_order_acq_rel))
    {
        const bool vca = pendingVcaExp_.load (std::memory_order_relaxed);
        const bool sm  = pendingSmoothing_.load (std::memory_order_relaxed);
        const float fd = pendingFilterDrive_.load (std::memory_order_relaxed);
        for (auto* v : voices)
            if (auto* av = dynamic_cast<AmbikaVoice*> (v))
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
    // partBytes[15], patchBytes...) to this acquire-read, so voiceIndices — which
    // is read by trigger/release/inject below on this same thread — is mutated
    // only here, never under a concurrent reader. (Constructor/prepare still call
    // rebuildVoiceAllocation() directly, but they run before audio starts.)
    if (allocationDirty_.exchange (false, std::memory_order_acq_rel))
    {
        // NOTE: we deliberately do NOT stop/kill voices here. A hard kill
        // (stopNote(.,false) => Voice::Kill + clearCurrentNote) zeroes the
        // envelope state that only Voice::Init() re-primes, so a voice killed
        // while IDLE would render SILENT on its next note -- heard as the
        // standalone going dead after a voice-mode / template switch (the
        // reported glitch). Instead let any sounding voice ring out naturally:
        // rebuildVoiceAllocation re-tags still-allocated voices to their
        // (possibly new) Part and pushPartBytesToVoices re-applies that Part's
        // patch, so a held note picks up the new Part's sound; a voice whose
        // voicecard is no longer allocated plays out its release and frees
        // itself. No permanent stuck notes, no dead-voice glitch.

        for (int p = 0; p < kNumParts; ++p)
            parts_[p].polyphonyMode = static_cast<uint8_t> (juce::jlimit (0, 4, (int) parts_[p].partBytes[15]));
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
        if (! parts_[p].configDirty_.exchange (false, std::memory_order_acq_rel))
            continue;
        auto& part = parts_[p];
        // Snapshot the MT-authoritative arp/seq config under the seqlock (never a
        // torn read of pendingConfig_ while the message thread stages a new edit).
        const auto cfg = part.readPendingConfig();
        // arp/seq mode (same byte drives both): handle transition on the AT.
        {
            const bool wasActive = part.arp.isActive();
            part.arp.setMode (cfg.arpMode);
            part.seq.setMode (cfg.arpMode);
            if (wasActive && ! part.arp.isActive())
            {
                part.arp.stop();
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
    }

    // Service deferred arp/seq note-kills ON THE AUDIO THREAD. setArpMode
    // (message thread) flags a Part when its arp/seq is turned off; kill that
    // Part's generated voices here so stopNote() never touches a voice the audio
    // thread is mid-rendering (one-block latency, inaudible).
    for (int p = 0; p < kNumParts; ++p)
    {
        if (parts_[p].killGeneratedNotes_.exchange (false, std::memory_order_acq_rel))
        {
            for (auto* v : voices)
                if (auto* av = dynamic_cast<AmbikaVoice*> (v))
                    if (av->getPartIndex() == p)
                        av->stopNote (0.0f, false);
        }
    }

    transport_.setTempo (bpm);
    applyTempo (bpm);

    if (isPlaying && ! wasPlaying_)
    {  // NOLINT(bugprone-branch-clone): the true-branch starts arp+seq, the else stops arp -- different bodies, clang-tidy FP
        for (auto& part : parts_) { part.arp.start(); part.seq.start(); }
    }
    else if (! isPlaying && wasPlaying_)
    {
        for (auto& part : parts_) part.arp.stop();
    }
    wasPlaying_ = isPlaying;

    // Per-Part: route note on/off into a Part's held-key stack when that Part's
    // arp/sequencer is active (strip them so renderNextBlock doesn't also play
    // the raw held key). Non-arp Parts pass through to handleNoteOn/Off.
    processedMidi_.clear();
    for (const auto meta : midi)
    {
        const auto msg = meta.getMessage();
        const int channel = msg.getChannel();
        const int note = msg.getNoteNumber();

        if (msg.isNoteOn() && msg.getVelocity() > 0)
        {
            const int p = findPartForNote (channel, note);
            if (p >= 0 && parts_[p].arp.isActive())
                parts_[p].arp.noteOn (note, msg.getVelocity());   // held key (stripped)
            else
                processedMidi_.addEvent (msg, meta.samplePosition);
        }
        else if (msg.isNoteOff() || (msg.isNoteOn() && msg.getVelocity() == 0))
        {
            const int p = findPartForNote (channel, note);
            if (p >= 0 && parts_[p].arp.isActive())
                parts_[p].arp.noteOff (note);   // stripped
            else
                processedMidi_.addEvent (msg, meta.samplePosition);
        }
        else
        {
            processedMidi_.addEvent (msg, meta.samplePosition);
        }
    }
    midi.swapWith (processedMidi_);

    // Advance the shared 24-PPQN clock; each Part's arp self-prescales (own
    // clockCounter_/resolution) and drives its own arp + sequencer. Run the
    // clock while the host transport plays, OR while any Part has its arp /
    // note-sequencer active with held keys — the firmware runs the internal
    // clock on note activity, so the arpeggiator keeps running in a stopped
    // DAW (the note-sequencer shares this clock and needs a held key for
    // transpose, so isActive()+hasHeldKeys() covers both).
    auto anyPartClockActive = [this]() -> bool
    {
        for (const auto& part : parts_)
            if (part.arp.isActive() && part.arp.hasHeldKeys())
                return true;
        return false;
    };
    const bool runClock = isPlaying || anyPartClockActive();
    if (runClock)
    {
        const int ticks = transport_.advance (numSamples);
        for (int t = 0; t < ticks; ++t)
        {
            for (int p = 0; p < kNumParts; ++p)
            {
                auto& part = parts_[p];
                part.arp.clockTick();
                const uint8_t heldNote = part.arp.mostRecentNote();
                const bool keyHeld = part.arp.hasHeldKeys();
                part.seq.clockTick (heldNote, keyHeld);
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
        auto& part = parts_[p];
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
    // Voice i == voicecard i (one voice per voicecard), so the voicecard IS the
    // (clamped) voice index.
    return juce::jlimit (0, kNumParts - 1, voiceIndex);
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

    // Route each voice to its FIXED voicecard buffer (Ambika hardware: 6
    // individual voicecard outputs). The master outputAudio is left untouched
    // here; the processor fills the main + aux buses from these buffers after
    // renderNextBlock returns.
    for (size_t i = 0; i < voices.size(); ++i)
    {
        auto* av = dynamic_cast<AmbikaVoice*> (voices[i]);
        if (av == nullptr)
            continue;   // all voices are AmbikaVoice; nothing else to render
        const int vc = juce::jlimit (0, kNumParts - 1, av->getVoiceCard());
        av->renderNextBlock (voiceCardBuffers_[(size_t) vc], startSample, numSamples);
    }
}

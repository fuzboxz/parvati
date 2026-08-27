// Copyright (c) 2026 805Labs Kft. / Hellcat.
//
// Sequencer — a faithful port of Ambika's controller-side step sequencer
// (controller/part.cc Part::ClockSequencer). It shares the arpeggiator's
// transport clock and has two halves:
//
//  (A) Two MODULATION sequences (SEQ_1 / SEQ_2): on each step, value =
//      PartData sequence_data step (16-step sequences) -> emitted to the engine,
//      which writes them into every voice's modulation_sources_[MOD_SRC_SEQ_1/2].
//      These run whenever the clock advances and sequence_length[i] > 0,
//      independent of arp mode (firmware ClockSequencer runs every prescaled
//      tick — Hellcat gates seq.clockTick on the arp's prescaled step).
//
//  (B) The NOTE sequence (ArpSequencerMode == NOTE): when sequence_length[2] > 0
//      and a key is held, generates notes from sequence_data bytes 32..63
//      (per step: a (note|gate) byte + a (velocity|legato) byte), transposed by
//      the most-recently-played key; gate/legato are honoured.
//
// sequence_data layout (controller PartData; see part.h):
//   0..15  : step sequence 1 (modulation values)
//   16..31 : step sequence 2 (modulation values)
//   32..63 : note sequence (16 steps x 2 bytes: note|gate, velocity|legato)
//
// Per-step gate (rest) and velocity are honoured: the note byte's bit 7 is the
// gate, and the velocity byte carries velocity (bits 0-6) + legato (bit 7).
// Both bytes are exposed as APVTS params (seqnote_step / seqnote_vel, 0..255).

#ifndef HELLCAT_SEQUENCER_H_
#define HELLCAT_SEQUENCER_H_

#include <cstdint>
#include <functional>

namespace hellcat
{

class Sequencer
{
public:
    using NoteOnFn  = std::function<void (int channel, int note, uint8_t velocity)>;
    using NoteOffFn = std::function<void (int channel, int note)>;

    Sequencer() = default;

    void setNoteOnCallback (NoteOnFn fn)  { onNoteOn_  = std::move (fn); }
    void setNoteOffCallback (NoteOffFn fn) { onNoteOff_ = std::move (fn); }

    // mode: 0 = STEP (off), 1 = ARPEGGIATOR, 2 = NOTE (note sequence).
    void setMode (uint8_t mode) { mode_ = mode; }
    uint8_t getMode() const { return mode_; }

    // Controller PartData sequence fields.
    void setSequenceLength (int i, uint8_t len)
    {
        if (i >= 0 && i < 3) sequenceLength_[i] = len;
    }
    uint8_t getSequenceLength (int i) const { return (i >= 0 && i < 3) ? sequenceLength_[i] : 0; }
    // Raw byte into the 64-byte sequence_data array.
    void setSequenceDataByte (int offset, uint8_t value)
    {
        if (offset >= 0 && offset < 64) sequenceData_[offset] = value;
    }
    uint8_t getSequenceDataByte (int offset) const { return (offset >= 0 && offset < 64) ? sequenceData_[offset] : 0; }

    // Reset on transport start (mirrors Part::Start()). Releases any sounding
    // note FIRST so a stranded previousNote_ can never be orphaned by the reset
    // (firmware Part::Start runs after Part::Stop -> AllNotesOff has released it).
    void start()
    {
        allNotesOff();
        sequencerStep_[0] = sequencerStep_[1] = sequencerStep_[2] = 0;
    }

    // Release the now-sounding note sequence note (twin of
    // Arpeggiator::allNotesOff). Called by the engine wherever the arp's notes
    // are killed: key-release emptying the held-key stack, transport stop, and
    // before start(). Idempotent (no-op when previousNote_ == 0xff).
    void allNotesOff()
    {
        internalNoteOff (previousNote_);
        previousNote_ = 0xff;
    }

    // Transport stop (twin of Arpeggiator::stop).
    void stop() { allNotesOff(); }

    // Test/debug: the now-sounding (last generated) note, or 0xff if none.
    uint8_t debugPreviousNote() const noexcept { return previousNote_; }

    // LIVE observation of the same value (0xff = none): used by the UI
    // telemetry append for the Note-Sequencer pill preview. A pure reader —
    // no audio-path state is touched.
    uint8_t liveNote() const noexcept { return previousNote_; }

    // Advance one (prescaled) clock step. `heldNote`/`keyHeld` feed the note
    // sequence transpose (most-recently-played key).
    void clockTick (uint8_t heldNote, bool keyHeld);

    // Current modulation-sequence values + whether each is active (length>0),
    // for the engine to inject into voices each block.
    uint8_t seqValue (int i) const { return seqValue_[i]; }
    bool    seqActive (int i) const { return seqActive_[i]; }

private:
    struct NoteStep
    {
        uint8_t note;
        uint8_t velocity;
        bool    gate;
        bool    legato;
    };

    // ---- exact firmware accessors (part.h) ----
    uint8_t stepValue (uint8_t seq, uint8_t step) const
    {
        return sequenceData_[(step + (seq << 4)) & 0x1f];
    }

    NoteStep noteStep (uint8_t step) const
    {
        const uint8_t offset = (32 + (step << 1)) & 0x3f;
        NoteStep n;
        n.note     = sequenceData_[offset] & 0x7f;
        n.velocity = sequenceData_[offset + 1] & 0x7f;
        // Gate is bit 7 of the note byte: a cleared bit means a rest (the
        // clockTick() logic honours this — gate false => note-off + silence).
        // Velocity/legato come from the velocity byte (exposed as seqnote_vel).
        n.gate     = (sequenceData_[offset] & 0x80) != 0;
        n.legato   = (sequenceData_[offset + 1] & 0x80) != 0;
        if (n.velocity == 0)
            n.velocity = 100;
        return n;
    }

    void internalNoteOn (uint8_t note, uint8_t velocity)
    {
        if (onNoteOn_) onNoteOn_ (midiChannel_, note, velocity);
    }
    void internalNoteOff (uint8_t note)
    {
        if (note != 0xff && onNoteOff_) onNoteOff_ (midiChannel_, note);
    }

    NoteOnFn  onNoteOn_;
    NoteOffFn onNoteOff_;

    uint8_t mode_ = 0;
    uint8_t sequenceLength_[3] {};
    uint8_t sequenceData_[64] {};
    uint8_t sequencerStep_[3] {};
    uint8_t seqValue_[2] { 0, 0 };
    bool    seqActive_[2] { false, false };
    uint8_t previousNote_ = 0xff;
    // Channel source for the note callbacks. The engine routes the callbacks
    // by Part, so nothing assigns this field; it stays at its default (1).
    int     midiChannel_ = 1;
};

}  // namespace hellcat

#endif  // HELLCAT_SEQUENCER_H_

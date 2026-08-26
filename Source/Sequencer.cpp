// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.  See Sequencer.h.

#include "Sequencer.h"

namespace hellcat
{

void Sequencer::clockTick (uint8_t heldNote, bool keyHeld)
{
    // (A) Modulation sequences SEQ_1 / SEQ_2 — independent of arp mode, like
    // firmware ClockSequencer (only emitted when sequence_length[i] > 0).
    for (int i = 0; i < 2; ++i)  // NOLINT(modernize-loop-convert): faithful port of firmware ClockSequencer
    {
        if (sequenceLength_[i])
        {
            seqValue_[i] = stepValue (static_cast<uint8_t> (i), sequencerStep_[i]);
            seqActive_[i] = true;
        }
        else
        {
            seqActive_[i] = false;
        }
    }

    // (B) Note sequence (ArpSequencerMode == NOTE).
    if (mode_ == 2 /* NOTE */ && keyHeld && sequenceLength_[2])
    {
        const NoteStep n = noteStep (sequencerStep_[2]);
        int note = static_cast<int> (n.note) + static_cast<int> (heldNote) - 60;
        if (note < 0) note = 0;
        else if (note > 127) note = 127;
        const uint8_t un = static_cast<uint8_t> (note);

        if (! n.gate)
        {  // NOLINT(bugprone-branch-clone): gate / non-legato / legato branches are distinct; clang-tidy FP
            internalNoteOff (previousNote_);
            previousNote_ = 0xff;
        }
        else if (! n.legato)
        {
            internalNoteOff (previousNote_);
            internalNoteOn (un, n.velocity);
            previousNote_ = un;
        }
        else
        {
            if (previousNote_ != un)
            {
                internalNoteOn (un, n.velocity);
                internalNoteOff (previousNote_);
            }
            previousNote_ = un;
        }
    }
    else if (previousNote_ != 0xff)
    {
        // Defensive self-clean (PATH A / D): the note-block guard is false (key
        // released, length set to 0, or mode != NOTE) but a note is still held.
        // Release it so it cannot strand. (The engine ALSO calls allNotesOff() on
        // key-release / transport stop, which fires even when the clock has
        // stopped — this branch covers the case where the clock is still running.)
        internalNoteOff (previousNote_);
        previousNote_ = 0xff;
    }

    // Advance every sequence's step (firmware advances all kNumSequences;
    // a 0-length sequence wraps back to 0 immediately).
    for (int i = 0; i < 3; ++i)
    {
        ++sequencerStep_[i];
        if (sequencerStep_[i] >= sequenceLength_[i])
            sequencerStep_[i] = 0;
    }
}

}  // namespace hellcat

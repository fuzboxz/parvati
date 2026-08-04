// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See Arpeggiator.h.

#include "Arpeggiator.h"

namespace parvati
{

void Arpeggiator::setDirection (uint8_t dir)
{
    direction_ = dir;
    // Mirrors firmware: arp_direction_ is -1 for DOWN, +1 otherwise.
    arpDirection_ = (dir == static_cast<uint8_t> (ArpDirection::Down)) ? -1 : 1;
}

void Arpeggiator::start()
{
    // Mirrors Part::Start(): forces the first step on the next clock tick,
    // resets the pattern mask and arpeggio position.
    clockCounter_ = prescaler_;   // forces immediate first step
    previousNote_ = 0xff;
    arpPatternMask_ = 1;
    arpDirection_ = (direction_ == static_cast<uint8_t> (ArpDirection::Down)) ? -1 : 1;
    startArpeggio();
}

void Arpeggiator::clockTick()
{
    // Mirrors Part::Clock(): prescale the 24-PPQN ticks into arp steps.
    if (++clockCounter_ >= prescaler_)
    {
        clockCounter_ = 0;
        clockArpeggiator();
    }
}

void Arpeggiator::startArpeggio()
{
    // Mirrors Part::StartArpeggio().
    if (arpDirection_ == 1)  // NOLINT(bugprone-branch-clone): up resets step to 0; down/random resets to the last step -- distinct, FP
    {
        arpOctave_ = 0;
        arpStep_ = 0;
    }
    else
    {
        arpStep_ = static_cast<int8_t> (pressedKeys_.size()) - 1;
        arpOctave_ = static_cast<int8_t> (octaveRange_) - 1;
    }
}

void Arpeggiator::clockArpeggiator()
{
    const uint16_t pattern = (pattern_ < 22) ? kArpPatterns[pattern_] : kArpPatterns[0];
    const uint8_t hasNote = (arpPatternMask_ & pattern) ? 255 : 0;

    if (isEnabled() && pressedKeys_.size() > 0 && hasNote)
    {
        if (direction_ != static_cast<uint8_t> (ArpDirection::Chord))
        {
            internalNoteOff (previousNote_);
            stepArpeggio();

            const NoteEntry* arpNote;
            if (direction_ == static_cast<uint8_t> (ArpDirection::AsPlayed))
                arpNote = &pressedKeys_.played_note (static_cast<uint8_t> (arpStep_));
            else
                arpNote = &pressedKeys_.sorted_note (static_cast<uint8_t> (arpStep_));

            uint8_t note = arpNote->note;
            uint8_t velocity = arpNote->velocity & 0x7f;
            note += 12 * arpOctave_;
            while (note > 127)  // NOLINT(bugprone-infinite-loop): note -= 12 each iter terminates the loop; clang-tidy FP
                note -= 12;

            internalNoteOn (note, velocity);
            previousNote_ = note;
        }
        else
        {
            // CHORD: trigger every held note each step.
            for (uint8_t i = 0; i < pressedKeys_.size(); ++i)
            {
                const auto& n = pressedKeys_.sorted_note (i);
                internalNoteOn (n.note, n.velocity & 0x7f);
            }
            previousNote_ = 60;  // arbitrary sentinel (firmware uses 60)
        }
    }
    else
    {
        // No arp note this step: kill the previous note(s).
        if (direction_ != static_cast<uint8_t> (ArpDirection::Chord))
        {
            internalNoteOff (previousNote_);
        }
        else
        {
            if (previousNote_ != 0xff)
            {
                for (uint8_t i = 0; i < pressedKeys_.size(); ++i)
                    internalNoteOff (pressedKeys_.sorted_note (i).note);
            }
        }
        previousNote_ = 0xff;
    }

    // Advance the pattern mask.
    arpPatternMask_ <<= 1;
    if (! arpPatternMask_)
        arpPatternMask_ = 1;
}

void Arpeggiator::stepArpeggio()
{
    const uint8_t numNotes = pressedKeys_.size();

    if (direction_ == static_cast<uint8_t> (ArpDirection::Random))
    {
        const uint8_t r = randomByte();
        arpOctave_ = r & 0x0f;
        arpStep_ = (r & 0xf0) >> 4;
        while (arpOctave_ >= static_cast<int8_t> (octaveRange_))
            arpOctave_ -= static_cast<int8_t> (octaveRange_);
        while (numNotes && arpStep_ >= static_cast<int8_t> (numNotes))
            arpStep_ -= static_cast<int8_t> (numNotes);
    }
    else
    {
        arpStep_ += arpDirection_;
        uint8_t changeOctave = 0;
        if (arpStep_ >= static_cast<int8_t> (numNotes))
        {
            arpStep_ = 0;
            changeOctave = 1;
        }
        else if (arpStep_ < 0)
        {
            arpStep_ = static_cast<int8_t> (numNotes) - 1;
            changeOctave = 1;
        }

        if (changeOctave)
        {
            arpOctave_ += arpDirection_;
            if (arpOctave_ >= static_cast<int8_t> (octaveRange_) || arpOctave_ < 0)
            {
                if (direction_ == static_cast<uint8_t> (ArpDirection::UpDown))
                {
                    arpDirection_ = -arpDirection_;
                    startArpeggio();
                    if (numNotes > 1 || octaveRange_ > 1)
                        stepArpeggio();
                }
                else
                {
                    startArpeggio();
                }
            }
        }
    }
}

void Arpeggiator::allNotesOff()
{
    if (previousNote_ != 0xff && direction_ != static_cast<uint8_t> (ArpDirection::Chord))
    {  // NOLINT(bugprone-branch-clone): single noteOff vs loop-over-held; distinct bodies, FP
        internalNoteOff (previousNote_);
    }
    else if (previousNote_ != 0xff)
    {
        for (uint8_t i = 0; i < pressedKeys_.size(); ++i)
            internalNoteOff (pressedKeys_.sorted_note (i).note);
    }
    previousNote_ = 0xff;
}

}  // namespace parvati

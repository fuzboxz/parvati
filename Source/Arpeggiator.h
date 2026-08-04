// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// Arpeggiator — a faithful port of Ambika's controller-side arpeggiator
// (controller/part.cc ClockArpeggiator / StepArpeggio / StartArpeggio).
// Driven by 24-PPQN clock ticks from the host transport (TransportClock in
// SynthEngine) instead of incoming MIDI clock.
//
// The arpeggiator tracks held keys (NoteStack<12>) and, on each clock step,
// selects a note according to direction/pattern/octave, emitting note-on/off
// events via callbacks to the SynthEngine.

#ifndef PARVATI_ARPEGGIATOR_H_
#define PARVATI_ARPEGGIATOR_H_

#include <cstdint>
#include <functional>

#include "NoteStack.h"
#include "dsp/random.h"

namespace parvati
{

// Direction values (match ambika ArpeggiatorDirection enum).
enum class ArpDirection : uint8_t
{
    Up = 0,
    Down,
    UpDown,
    AsPlayed,
    Random,
    Chord
};

// Mode: Off (notes pass through), Arp (arpeggiate), or Sequencer (note seq).
enum class ArpMode : uint8_t
{
    Off = 0,
    Arp,
    Sequencer
};

// Exact firmware data: MIDI-clock ticks per step, indexed by arp divider.
static constexpr uint8_t kMidiClockTickPerStep[15] = {
    96, 72, 64, 48, 36, 32, 24, 16, 12, 8, 6, 4, 3, 2, 1
};

// 16-bit step-gate patterns (lut_res_arpeggiator_patterns[22] from firmware).
static constexpr uint16_t kArpPatterns[22] = {
    21845, 62965, 46517, 54741, 43861, 22869, 38293, 2313,
    37449, 21065, 18761, 54553, 27499, 23387, 30583, 28087,
    22359, 28527, 30431, 43281, 28609, 53505
};

class Arpeggiator
{
public:
    using NoteOnFn  = std::function<void (int channel, int note, uint8_t velocity)>;
    using NoteOffFn = std::function<void (int channel, int note)>;

    Arpeggiator() = default;

    void setNoteOnCallback (NoteOnFn fn)  { onNoteOn_  = std::move (fn); }
    void setNoteOffCallback (NoteOffFn fn) { onNoteOff_ = std::move (fn); }

    // ---- parameter setters (from APVTS) ----
    void setMode (uint8_t mode)        { mode_ = mode; }
    void setDirection (uint8_t dir);
    void setOctave (uint8_t octave)    { octaveRange_ = octave; }
    void setPattern (uint8_t pattern)  { pattern_ = pattern; }
    void setResolution (uint8_t res)   { divider_ = res; recomputePrescaler(); }

    // Read-back (for multitimbral part-swap).
    uint8_t getMode() const       { return mode_; }
    uint8_t getDirection() const  { return direction_; }
    uint8_t getOctave() const     { return octaveRange_; }
    uint8_t getPattern() const    { return pattern_; }
    uint8_t getResolution() const { return divider_; }

    // Test/inspection: the most recently generated arp note (0xff = none).
    uint8_t lastNote() const { return previousNote_; }

    // MOD_SRC_ARP_STEP value (firmware has_arpeggiator_note): 255 while an arp
    // note is currently sounding, 0 otherwise. Injected per block by the engine.
    uint8_t stepGateValue() const { return (previousNote_ != 0xff) ? 255 : 0; }

    // ---- held-key tracking (from MIDI, when arp is on) ----
    void noteOn (int note, uint8_t velocity)
    {
        pressedKeys_.noteOn (static_cast<uint8_t> (note), velocity);
    }

    void noteOff (int note)
    {
        pressedKeys_.noteOff (static_cast<uint8_t> (note));
        // If no keys held, kill the last arp note.
        if (pressedKeys_.size() == 0)
            allNotesOff();
    }

    // ---- transport ----
    void start();
    void stop() { allNotesOff(); }

    // Feed one 24-PPQN clock tick. Internally prescaled by the divider.
    void clockTick();

    bool isEnabled() const { return mode_ == static_cast<uint8_t> (ArpMode::Arp); }

    // Active = any note-generating/routing mode (Arp or Sequencer). Used by the
    // engine to decide whether to route MIDI to the held-key stack + advance the
    // shared transport clock.
    bool isActive() const { return mode_ != static_cast<uint8_t> (ArpMode::Off); }

    // Held-key access for the note-sequence transpose (shared with Sequencer).
    bool    hasHeldKeys() const { return pressedKeys_.size() > 0; }
    uint8_t mostRecentNote() const
    {
        return pressedKeys_.size() ? pressedKeys_.most_recent_note().note : 60;
    }

private:
    void recomputePrescaler()
    {
        prescaler_ = kMidiClockTickPerStep[divider_ < 15 ? divider_ : 14];
    }

    void clockArpeggiator();
    void stepArpeggio();
    void startArpeggio();

    void internalNoteOn (uint8_t note, uint8_t velocity)
    {
        if (onNoteOn_)
            onNoteOn_ (midiChannel_, note, velocity);
    }

    void internalNoteOff (uint8_t note)
    {
        if (note != 0xff && onNoteOff_)
            onNoteOff_ (midiChannel_, note);
    }

    void allNotesOff();

    // Callbacks to the engine.
    NoteOnFn  onNoteOn_;
    NoteOffFn onNoteOff_;

    // Held-key stack.
    NoteStack<12> pressedKeys_;

    // Parameters (mirrors the firmware PartData arp fields).
    uint8_t mode_       = static_cast<uint8_t> (ArpMode::Off);
    uint8_t direction_  = static_cast<uint8_t> (ArpDirection::Up);  // ArpDirection enum
    uint8_t octaveRange_ = 1;
    uint8_t pattern_    = 0;
    uint8_t divider_    = 6;   // default 1/16 (24 ticks)
    uint8_t prescaler_  = 24;

    // Arp sequencer state.
    int8_t   arpDirection_   = 1;    // +1 or -1
    int8_t   arpStep_        = 0;
    int8_t   arpOctave_      = 0;
    uint16_t arpPatternMask_ = 1;
    uint8_t  previousNote_   = 0xff; // 0xff = none
    uint8_t  clockCounter_   = 0;

    int midiChannel_ = 1;

    // RANDOM direction draws from the shared global Galois LFSR, matching the
    // firmware (controller/part.cc uses Random::GetByte()).
    uint8_t randomByte()
    {
        return ambika::dsp::random().GetByte();
    }
};

}  // namespace parvati

#endif  // PARVATI_ARPEGGIATOR_H_

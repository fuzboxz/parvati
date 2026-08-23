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
#include "dsp/constants.h"   // midi_clock_tick_per_step (single-source tick table)
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

// Exact firmware data (controller/part.cc:30-32): MIDI-clock ticks per step,
// indexed by arp divider. SINGLE SOURCE: the identical table
// ambika::dsp::midi_clock_tick_per_step (dsp/constants.h) — also indexed by
// the tempo-synced LFO rate — aliased here under its historical name so the
// arp prescaler and the synced-LFO rate can never silently diverge.
static constexpr const uint8_t (&kMidiClockTickPerStep)[15] =
    ambika::dsp::midi_clock_tick_per_step;
static_assert (ambika::dsp::kNumSyncedLfoRates == 15,
               "kMidiClockTickPerStep historically had 15 entries (dividers 0..14)");

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
    // note now sounds, 0 otherwise. Injected per block by the engine.
    uint8_t stepGateValue() const { return (previousNote_ != 0xff) ? 255 : 0; }

    // ---- held-key tracking (from MIDI, when arp is on) ----
    void noteOn (int note, uint8_t velocity)
    {
        pressedKeys_.noteOn (static_cast<uint8_t> (note), velocity);
    }

    void noteOff (int note)
    {
        pressedKeys_.noteOff (static_cast<uint8_t> (note));
        // W11 (F-eng-2, firmware part.cc:341-354): in CHORD direction the
        // chord trigger mode "doesn't really clean after itself" — the
        // firmware kills the note DIRECTLY at key-up (same branch as STEP
        // mode) to avoid stuck notes. The port dropped this: the released
        // pitch kept ringing (no off was ever sent — each step only
        // re-triggers held keys). On the LAST key-up, allNotesOff()'s
        // chord branch looped the already-empty stack, stranding every chord
        // voice until CC123/voice-steal.
        if (direction_ == static_cast<uint8_t> (ArpDirection::Chord))
            internalNoteOff (static_cast<uint8_t> (note));
        // If no keys held, kill the last arp note.
        if (pressedKeys_.size() == 0)
            allNotesOff();
    }

    // ---- transport ----
    void start();
    void stop() { allNotesOff(); }

    // Forget every held key WITHOUT releasing the generated note (firmware
    // Part::AllNotesOff calls pressed_keys_.Clear() itself, alongside the
    // generated-note release). Used by SynthEngine::partAllNotesOff (CC123).
    void clearHeldKeys() { pressedKeys_.clear(); }

    // Feed one 24-PPQN clock tick. Internally prescaled by the divider. Returns
    // true when the prescaler rolled over and a step fired (clockArpeggiator ran)
    // — the engine gates the Sequencer on this so BOTH run at the same prescaled
    // rate (firmware part.cc:590-601 runs ClockSequencer + ClockArpeggiator in
    // the SAME prescaled branch). The prescaler advances every call regardless
    // of arp mode, so the Sequencer's modulation seqs keep stepping when the arp
    // is off but the transport is running.
    bool clockTick();

    bool isEnabled() const { return mode_ == static_cast<uint8_t> (ArpMode::Arp); }

    // Active = any note-generating/routing mode (Arp or Sequencer). Used by the
    // engine to decide whether to route MIDI to the held-key stack + advance the
    // shared transport clock.
    bool isActive() const { return mode_ != static_cast<uint8_t> (ArpMode::Off); }

    // Held-key access for the note-sequence transpose (shared with Sequencer).
    bool    hasHeldKeys() const { return pressedKeys_.size() > 0; }
    // Is @p note in the held-key stack? The engine's note-off routing uses
    // this to distinguish two key kinds: a key the arp/sequencer is HOLDING
    // (note-off goes to the stack), and a key that was sounding DIRECTLY
    // before the mode was enabled (never entered the stack). In the second
    // case the note-off must release the direct voice through the normal
    // MIDI path — otherwise it sustains forever.
    bool    holdsNote (int note) const
    {
        return pressedKeys_.contains (static_cast<uint8_t> (note));
    }
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
    uint8_t divider_    = 10;  // factory default (part.cc init arp bytes {0,1,0,10}): index 10 = 6 ticks = 1/16
    uint8_t prescaler_  = 6;   // kMidiClockTickPerStep[10]

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

// Desktop shim: controller/midi_dispatcher.h — RECORDING replacement for the
// firmware's MIDI dispatcher (tests/firmware_parity_test).
//
// The firmware Part reports generated/forwarded note events and edits through
// midi_dispatcher.OnNote / ForwardNote / OnEdit, and Multi::Clock/Start/Stop
// signal the transport through OnClock/OnStart/OnStop. The real dispatcher
// queues AVR MIDI-out bytes; on the host we record the callbacks so the
// parity oracle can diff the controller's generated-note stream against our
// engine's. The real header is shadowed through the shim include path.
#pragma once

#include <cstdint>
#include <vector>

#include "avrlib/op.h"   // desktop shim (also opens namespace avrlib)

#include "controller/controller.h"
#include "controller/storage.h"   // no-op host shim (the real dispatcher pulls it)

namespace ambika
{

// The REAL firmware closure exposes avrlib:: fixed-point helpers unqualified
// to controller TUs through headers this shim replaces (their real versions
// carry using-directives); multi.cc calls U8U8Mul unqualified. Reproduce that
// visibility here so the vendored sources compile unmodified.
using namespace avrlib;


class Part;

struct FwMidiEvent
{
    enum Kind { kOnNote, kForwardNote, kOnEdit, kOnClock, kOnStart, kOnStop };
    Kind     kind     = kOnNote;
    int      part     = -1;    // resolved from the Part* by the oracle
    uint8_t  note     = 0;
    uint8_t  velocity = 0;
    uint8_t  address  = 0;     // OnEdit
    uint8_t  value    = 0;     // OnEdit
    const void* partPtr = nullptr;
};

class MidiDispatcher
{
public:
    MidiDispatcher() {}

    // Part-side callbacks (part.cc).
    static void OnNote (Part* part, uint8_t note, uint8_t velocity)
    {
        FwMidiEvent e;
        e.kind = FwMidiEvent::kOnNote; e.partPtr = part; e.note = note; e.velocity = velocity;
        log().push_back (e);
    }
    static void ForwardNote (Part* part, uint8_t note, uint8_t velocity)
    {
        FwMidiEvent e;
        e.kind = FwMidiEvent::kForwardNote; e.partPtr = part; e.note = note; e.velocity = velocity;
        log().push_back (e);
    }
    static void OnEdit (Part* part, uint8_t address, uint8_t value)
    {
        FwMidiEvent e;
        e.kind = FwMidiEvent::kOnEdit; e.partPtr = part; e.address = address; e.value = value;
        log().push_back (e);
    }

    // Transport callbacks (multi.cc).
    static void OnClock() { push (FwMidiEvent::kOnClock); }
    static void OnStart() { push (FwMidiEvent::kOnStart); }
    static void OnStop()  { push (FwMidiEvent::kOnStop); }

    // ---- recorder plumbing (test-side) ----
    static std::vector<FwMidiEvent>& log()
    {
        static std::vector<FwMidiEvent> g_log;
        return g_log;
    }
    static void clearLog() { log().clear(); }

private:
    static void push (FwMidiEvent::Kind k)
    {
        FwMidiEvent e;
        e.kind = k;
        log().push_back (e);
    }
    MidiDispatcher (const MidiDispatcher&);
    void operator= (const MidiDispatcher&);
};

extern MidiDispatcher midi_dispatcher;

}  // namespace ambika

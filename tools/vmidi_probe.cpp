// Virtual MIDI source probe for the real Standalone app: creates a CoreMIDI
// virtual source named "HellcatProbe", waits for the app to boot, then sends a
// single note (on ch1) after a delay, holds it, releases it.
// Build: clang++ -std=c++17 -framework CoreMIDI -framework CoreFoundation vmidi_probe.cpp -o vmidi_probe
// Run: ./vmidi_probe <noteDelaySec> <holdSec> [note] [vel]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>

#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>

int main (int argc, char** argv)
{
    const double delaySec = (argc > 1) ? std::atof (argv[1]) : 6.0;
    const double holdSec  = (argc > 2) ? std::atof (argv[2]) : 2.0;
    const int note        = (argc > 3) ? std::atoi (argv[3]) : 60;
    const int vel         = (argc > 4) ? std::atoi (argv[4]) : 100;

    MIDIClientRef client = 0;
    if (MIDIClientCreate (CFSTR ("HellcatProbeClient"), nullptr, nullptr, &client) != noErr)
    { std::printf ("MIDIClientCreate failed\n"); return 1; }
    MIDIEndpointRef src = 0;
    if (MIDISourceCreate (client, CFSTR ("HellcatProbe"), &src) != noErr)
    { std::printf ("MIDISourceCreate failed\n"); return 1; }
    std::printf ("virtual source 'HellcatProbe' up; sleeping %.1f s...\n", delaySec);
    std::fflush (stdout);

    std::this_thread::sleep_for (std::chrono::duration<double> (delaySec));

    Byte on[3]  = { 0x90, (Byte) note, (Byte) vel };
    Byte off[3] = { 0x80, (Byte) note, 0 };
    MIDIPacketList pl;
    MIDIPacket* p = MIDIPacketListInit (&pl);
    MIDIPacketListAdd (&pl, sizeof (pl), p, 0, 3, on);
    MIDIReceived (src, &pl);   // deliver from the virtual source to all connected clients
    std::printf ("note-on %d vel %d sent; holding %.1f s...\n", note, vel, holdSec);
    std::fflush (stdout);

    std::this_thread::sleep_for (std::chrono::duration<double> (holdSec));

    p = MIDIPacketListInit (&pl);
    MIDIPacketListAdd (&pl, sizeof (pl), p, 0, 3, off);
    MIDIReceived (src, &pl);
    std::printf ("note-off sent\n");

    // ---- second note (note2, +2 semitones) after a 0.6 s gap ----
    std::this_thread::sleep_for (std::chrono::duration<double> (0.6));
    Byte on2[3]  = { 0x90, (Byte) (note + 2), (Byte) vel };
    p = MIDIPacketListInit (&pl);
    MIDIPacketListAdd (&pl, sizeof (pl), p, 0, 3, on2);
    MIDIReceived (src, &pl);
    std::printf ("note2-on sent\n");
    std::this_thread::sleep_for (std::chrono::duration<double> (holdSec));
    Byte off2[3] = { 0x80, (Byte) (note + 2), 0 };
    p = MIDIPacketListInit (&pl);
    MIDIPacketListAdd (&pl, sizeof (pl), p, 0, 3, off2);
    MIDIReceived (src, &pl);
    std::printf ("note2-off sent\n");
    std::this_thread::sleep_for (std::chrono::duration<double> (0.5));
    return 0;
}

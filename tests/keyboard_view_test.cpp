// KeyboardView touch/mouse interaction test: real glissando retargeting.
//
// The virtual keyboard's inner juce::MidiKeyboardComponent subclass routes
// presses/drags/releases to the engine through KeyboardView::NoteCallback.
// This test drives that routing DIRECTLY with synthetic MouseEvents (no
// peer/event loop needed) and asserts the engine-visible NOTE ORDER:
//
//   [0] window contract — exactly two octaves visible (C3..C5, 48..72) and
//       the 15 white keys stretched to fill the strip width (large keys)
//   [1] press            -> note-on
//   [2] drag to new key  -> note-off(old) THEN note-on(new)   (T8 glissando;
//                            previously the engine held the first note = stuck)
//   [3] drag to same key -> no events
//   [4] drag back        -> off/on pair again
//   [5] release on key   -> note-off(last)
//   [5b] press/release the window's TOP key (C5, 72) — edge key routing
//   [6] press + release OFF the keys (base mouseUp path) -> exactly one
//       note-off, no stuck note (per-source release guard)
//   [7] Ableton-style keyboard controls: X/Z shift the octave (the visible
//       window follows, clamped at the MIDI edges) and V/C step the typing
//       velocity by 20/127 (default 100/127)
//
// All tested notes are inside the visible two-octave window (48..72) — the
// old 5-octave [36,96] range is gone; the drag/release notes 60/62/64 were
// already in-window, so the routing assertions keep their exact semantics.
//
// Multitouch (two fingers colliding on one key via retarget) needs two
// MouseInputSources; macOS Desktop only ever exposes source 0 headlessly, so
// the cross-source dedup helper is covered by the single-source ordering here
// and by code inspection (see KeyboardView::noteHeldByOtherSource).

#include <cmath>
#include <cstdio>
#include <vector>

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "ui/KeyboardView.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg) { std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg); if (! cond) ++g_failures; }

// One engine-visible event: (note, isOn).
struct Ev
{
    int  note;
    bool on;
};
bool operator== (const Ev& a, const Ev& b) { return a.note == b.note && a.on == b.on; }
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    KeyboardView kb;
    kb.setSize (1200, 246);   // the TALL strip the editor hosts (covers the bottom matrix row); resized() lays the fixed two-octave window (48..72) full-width

    // Engine-visible event log (the seam the editor wires into the processor).
    std::vector<Ev> events;
    kb.setNoteCallback ([&] (int note, bool isOn, float velocity)
                        {
                            juce::ignoreUnused (velocity);
                            events.push_back ({ note, isOn });
                        });

    // The KeyComp is KeyboardView's only child component; reach it through the
    // public base pointer (the subclass itself is private to KeyboardView).
    juce::MidiKeyboardComponent* keys = nullptr;
    for (auto* child : kb.getChildren())
        if (auto* k = dynamic_cast<juce::MidiKeyboardComponent*> (child))
            keys = k;
    check (keys != nullptr, "inner MidiKeyboardComponent found as a child");

    // ---- [0]: the two-octave large-key window contract ----
    std::printf ("[0] Two-octave large-key window\n");
    check (keys->getRangeStart() == 48 && keys->getRangeEnd() == 72,
           "visible range is exactly two octaves, C3..C5 (48..72)");
    check (std::abs (keys->getKeyWidth() - 1200.0f / 15.0f) < 0.5f,
           "the window's 15 white keys fill the 1200pt strip (~80pt each)");
    check (keys->getHeight() == 246,
           "the KeyComp fills the FULL component height (tall strip, no fixed-height override)");

    // mouseDownOnKey grabs keyboard focus for musical typing, which asserts
    // unless the component is on the desktop — host it in a temporary window.
    kb.addToDesktop (juce::ComponentPeer::windowIsTemporary);
    check (kb.isOnDesktop(), "keyboard hosted on the desktop for focus handling");

    // Build MouseEvents on the real main mouse source (index 0, exactly what a
    // desktop mouse drag produces). Position is in KeyComp coordinates; y=8 is
    // near the key top => QUIET velocity under the y-position=pressure rule
    // (lower on the key = louder; only press velocity is observable here).
    const auto source = juce::Desktop::getInstance().getMainMouseSource();
    auto eventAt = [&] (juce::Point<float> pos, bool dragged)
    {
        return juce::MouseEvent (source, pos,
                                 juce::ModifierKeys().withFlags (juce::ModifierKeys::leftButtonModifier),
                                 juce::MouseInputSource::defaultPressure,
                                 juce::MouseInputSource::defaultOrientation,
                                 juce::MouseInputSource::defaultRotation,
                                 juce::MouseInputSource::defaultTiltX,
                                 juce::MouseInputSource::defaultTiltY,
                                 keys, keys,
                                 juce::Time::getCurrentTime(), pos,
                                 juce::Time::getCurrentTime(), 1, dragged);
    };

    // ---- [1]-[2]-[3]-[4]-[5]: press, drag across keys, release ----
    std::printf ("[1] Press C4 (60)\n");
    check (keys->mouseDownOnKey (60, eventAt ({ 100.0f, 8.0f }, false)),
           "mouseDownOnKey returns true (base lights the key)");
    check ((events == std::vector<Ev> { { 60, true } }), "press fires exactly one note-on(60)");

    std::printf ("[2] Drag onto D4 (62): real glissando retarget\n");
    events.clear();
    check (keys->mouseDraggedToKey (62, eventAt ({ 160.0f, 8.0f }, true)),
           "mouseDraggedToKey returns true (base lights the swept key)");
    check ((events == std::vector<Ev> { { 60, false }, { 62, true } }),
           "drag to a new key fires off(60) THEN on(62) — in that order");

    std::printf ("[3] Drag within the SAME key (62): no retrigger\n");
    events.clear();
    keys->mouseDraggedToKey (62, eventAt ({ 165.0f, 8.0f }, true));
    check (events.empty(), "drag to the held key fires nothing");

    std::printf ("[4] Drag back onto C4 (60)\n");
    events.clear();
    keys->mouseDraggedToKey (60, eventAt ({ 100.0f, 8.0f }, true));
    check ((events == std::vector<Ev> { { 62, false }, { 60, true } }),
           "drag back fires off(62) THEN on(60)");

    std::printf ("[5] Release on the key\n");
    events.clear();
    keys->mouseUpOnKey (60, eventAt ({ 100.0f, 8.0f }, true));
    check ((events == std::vector<Ev> { { 60, false } }), "release fires exactly one off(60)");

    std::printf ("[5b] Press/release C5 (72) — the window's top edge key\n");
    events.clear();
    check (keys->mouseDownOnKey (72, eventAt ({ 1180.0f, 8.0f }, false)),
           "mouseDownOnKey returns true on the top key");
    keys->mouseUpOnKey (72, eventAt ({ 1180.0f, 8.0f }, true));
    check ((events == std::vector<Ev> { { 72, true }, { 72, false } }),
           "top-edge key fires on(72) then exactly one off(72)");

    // ---- [6]: release landing OFF the keys (per-source stuck-note guard) ----
    std::printf ("[6] Press E4 (64), release off the keys\n");
    events.clear();
    keys->mouseDownOnKey (64, eventAt ({ 220.0f, 8.0f }, false));
    // Position outside any key (below the 246pt keyboard strip): the base
    // mouseUp must still clear the source's held note via the fallback guard.
    keys->mouseUp (eventAt ({ 220.0f, 900.0f }, true));
    check ((events == std::vector<Ev> { { 64, true }, { 64, false } }),
           "press + off-key release fires on(64) then exactly one off(64)");

    // ---- [7]: Ableton-style computer-keyboard controls (Z/X octave + C/V velocity) ----
    // keyPressed() is the public Component hook; drive it directly with bare
    // KeyPresses (no modifiers). Velocity is recorded via a dedicated log so
    // the [1..6] event semantics stay untouched.
    std::printf ("[7] Ableton-style keyboard controls (octave + velocity)\n");
    {
        // A bare KeyPress(int) carries NO text character — build them with the
        // full ctor so keyPressed()'s getTextCharacter() path sees the letter.
        // A bare KeyPress(int) carries NO text character — build them with the
        // full ctor so keyPressed()'s getTextCharacter() path sees the letter.
        // The musical-typing controls are BARE Z/X/C/V (modifier combos stay
        // the app's Undo/Cut/Copy/Paste + zoom shortcuts).
        auto key = [] (char c)
        { return juce::KeyPress ((int) (unsigned char) c, juce::ModifierKeys::noModifiers, (juce::juce_wchar) c); };
        std::vector<std::pair<int, float>> velEvents;   // (note, velocity) on note-ons only
        kb.setNoteCallback ([&] (int note, bool isOn, float velocity)
                            {
                                events.push_back ({ note, isOn });
                                if (isOn) velEvents.push_back ({ note, velocity });
                            });

        // Octave up (X): the window FOLLOWS the base (60..84) and 'a' plays
        // the NEW bottom C (60) at the default velocity 100/127.
        check (kb.keyPressed (key ('x')), "X (octave up) handled");
        check (keys->getRangeStart() == 60 && keys->getRangeEnd() == 84,
               "after X the visible window is C4..C6 (60..84)");
        kb.keyPressed (key ('a'));
        check (velEvents.size() == 1 && velEvents[0].first == 60
                   && std::abs (velEvents[0].second - 100.0f / 127.0f) < 0.001f,
               "after X, 'a' plays C4 (60) at the default velocity 100/127");

        // Velocity up (V) then a new key: 120/127. Velocity down twice (C):
        // 80/127 then 60/127.
        kb.keyPressed (key ('v'));
        kb.keyPressed (key ('s'));
        check (velEvents.size() == 2 && velEvents[1].first == 62
                   && std::abs (velEvents[1].second - 120.0f / 127.0f) < 0.001f,
               "V raises the typing velocity to 120/127");
        kb.keyPressed (key ('c'));
        kb.keyPressed (key ('c'));
        kb.keyPressed (key ('d'));
        check (velEvents.size() == 3 && velEvents[2].first == 64
                   && std::abs (velEvents[2].second - 80.0f / 127.0f) < 0.001f,
               "C twice lowers the typing velocity to 80/127");

        // Octave down (Z, once — we are one octave up): back to the default
        // window (48..72); the clamp at the MIDI edges holds (Z below 0 / X
        // above 127-window).
        kb.keyPressed (key ('z'));
        check (keys->getRangeStart() == 48 && keys->getRangeEnd() == 72,
               "after Z the window is back to C3..C5 (48..72)");
        for (int i = 0; i < 12; ++i) kb.keyPressed (key ('z'));
        check (keys->getRangeStart() == 0,
               "Z at the bottom clamps the window at MIDI 0");
        for (int i = 0; i < 20; ++i) kb.keyPressed (key ('x'));
        check (keys->getRangeStart() == 127 - 24,
               "X at the top clamps the window at 127-24");

        // ---- [7b]: the settings-changed feedback channel (the editor wires
        //      this into the status/tooltip bar). One report per change, with
        //      the correct (base, velocity) pair; plus the accessors. ----
        std::printf ("[7b] Settings-changed feedback channel\n");
        {
            std::vector<std::pair<int, int>> reports;
            kb.setSettingsChangedCallback ([&] (int base, int vel)
                                           { reports.push_back ({ base, vel }); });
            check (reports.size() == 1,
                   "setSettingsChangedCallback primes with the initial state");
            // [7] left the state at base 103, velocity 80 (C twice from 100).
            kb.keyPressed (key ('v'));
            check (reports.size() == 2 && reports[1].first == 127 - 24
                       && reports[1].second == 100,
                   "V reports (current base, velocity 80+20=100)");
            kb.keyPressed (key ('c'));
            check (reports.size() == 3 && reports[2].second == 80,
                   "C reports the stepped velocity (100-20=80)");
            kb.keyPressed (key ('z'));
            check (reports.size() == 4 && reports[3].first == 127 - 24 - 12
                       && reports[3].second == 80,
                   "Z reports the shifted octave base with the velocity kept");
            check (kb.qwertyOctaveBase() == 127 - 24 - 12
                       && kb.qwertyVelocity127() == 80,
                   "accessors mirror the reported state");
        }

        // ---- [7c]: continuous Y-pressure channel (channel-pressure/AT).
        //      A press reports its strike Y; EVERY drag move re-reports
        //      (lower on the key = harder). ----
        std::printf ("[7c] Continuous Y-pressure (aftertouch) channel\n");
        {
            std::vector<float> pressures;
            kb.setPressureCallback ([&] (float p) { pressures.push_back (p); });
            // Press near the top (y=8/246 => quiet ~0.97), drag down to y=230
            // (hard ~0.07): the stream must descend monotonically-ish through
            // the drag moves.
            pressures.clear();
            keys->mouseDownOnKey (60, eventAt ({ 100.0f, 8.0f }, false));
            check (! pressures.empty() && pressures.front() > 0.9f,
                   "press at the key top reports a quiet initial pressure (>0.9)");
            const int nBefore = (int) pressures.size();
            keys->mouseDrag (eventAt ({ 100.0f, 230.0f }, true));
            check (pressures.size() > (size_t) nBefore && pressures.back() < 0.15f,
                   "dragging to the key bottom drives pressure hard (<0.15)");
            // Release: pressure stream simply stops (no zero-flush event).
            const int nAtRelease = (int) pressures.size();
            keys->mouseUp (eventAt ({ 100.0f, 230.0f }, true));
            check (pressures.size() == (size_t) nAtRelease,
                   "release emits no extra pressure events (stream holds last value)");
        }
    }

    std::printf ("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}

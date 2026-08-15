// KeyboardView touch/mouse interaction test: real glissando retargeting.
//
// The virtual keyboard's inner juce::MidiKeyboardComponent subclass routes
// presses/drags/releases to the engine through KeyboardView::NoteCallback.
// This test drives that routing DIRECTLY with synthetic MouseEvents (no
// peer/event loop needed) and asserts the engine-visible NOTE ORDER:
//
//   [1] press            -> note-on
//   [2] drag to new key  -> note-off(old) THEN note-on(new)   (T8 glissando;
//                            previously the engine held the first note = stuck)
//   [3] drag to same key -> no events
//   [4] drag back        -> off/on pair again
//   [5] release on key   -> note-off(last)
//   [6] press + release OFF the keys (base mouseUp path) -> exactly one
//       note-off, no stuck note (per-source release guard)
//
// Multitouch (two fingers colliding on one key via retarget) needs two
// MouseInputSources; macOS Desktop only ever exposes source 0 headlessly, so
// the cross-source dedup helper is covered by the single-source ordering here
// and by code inspection (see KeyboardView::noteHeldByOtherSource).

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
    kb.setSize (1200, 80);   // resized() lays the fixed [36,96] range out full-width

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

    // mouseDownOnKey grabs keyboard focus for musical typing, which asserts
    // unless the component is on the desktop — host it in a temporary window.
    kb.addToDesktop (juce::ComponentPeer::windowIsTemporary);
    check (kb.isOnDesktop(), "keyboard hosted on the desktop for focus handling");

    // Build MouseEvents on the real main mouse source (index 0, exactly what a
    // desktop mouse drag produces). Position is in KeyComp coordinates; y=8 is
    // near the key top => loud velocity (only press velocity is observable).
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

    // ---- [6]: release landing OFF the keys (per-source stuck-note guard) ----
    std::printf ("[6] Press E4 (64), release off the keys\n");
    events.clear();
    keys->mouseDownOnKey (64, eventAt ({ 220.0f, 8.0f }, false));
    // Position outside any key (below the keyboard strip): the base mouseUp
    // must still clear the source's held note via the fallback guard.
    keys->mouseUp (eventAt ({ 220.0f, 900.0f }, true));
    check ((events == std::vector<Ev> { { 64, true }, { 64, false } }),
           "press + off-key release fires on(64) then exactly one off(64)");

    std::printf ("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}

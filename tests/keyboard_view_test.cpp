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
//   [8] themed key palette: for EVERY shipped theme the resolved
//       KeyboardColours are theme-token-driven (none equals a stock JUCE
//       MidiKeyboardComponent default incl. the ivory 0xfff0f0f0 family),
//       pressed differs from idle, naturals vs sharps keep a >=1.6:1 WCAG
//       contrast (legibility on light AND dark themes), and a live theme
//       switch re-resolves the palette (the refresh() seam)
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
#include "unified_test_runner.h"
#include <cstdio>
#include <vector>

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "ui/KeyboardView.h"
#include "ui/WheelsComponent.h"
#include "ui/ParvatiLookAndFeel.h"
#include "ui/ParvatiTheme.h"

// Exact float comparison is deliberate: these asserts pin values,
// not ranges.
#pragma clang diagnostic ignored "-Wfloat-equal"

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

TEST(keyboard_view_test)
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


    // ---- [8]: themed key palette (theme-driven, legible, re-resolving) ----
    std::printf ("[8] Themed key palette\n");
    {
        // WCAG relative luminance + contrast ratio (the legibility metric).
        const auto channel = [] (float c01)
        {
            return (c01 <= 0.03928f) ? c01 / 12.92f
                                     : std::pow ((c01 + 0.055f) / 1.055f, 2.4f);
        };
        const auto luminance = [&] (const juce::Colour& c)
        {
            return 0.2126f * channel (c.getFloatRed())
                 + 0.7152f * channel (c.getFloatGreen())
                 + 0.0722f * channel (c.getFloatBlue());
        };
        const auto contrast = [&] (const juce::Colour& a, const juce::Colour& b)
        {
            const float la = luminance (a), lb = luminance (b);
            const float hi = std::max (la, lb), lo = std::min (la, lb);
            return (hi + 0.05f) / (lo + 0.05f);
        };

        // Stock juce::MidiKeyboardComponent / LookAndFeel_V4 key defaults the
        // themed palette must NEVER equal (the bone/ivory complaint): V4 uses
        // pure white naturals + raw black sharps; V2's family is the classic
        // 0xfff0f0f0 bone; the yellow press/hover overlays are stock too.
        const juce::uint32 stockDefaults[] = {
            0xffffffff,   // whiteNoteColourId (V4)
            0xfff0f0f0,   // whiteNoteColourId (V2 "bone" family)
            0xff000000,   // blackNoteColourId / textLabelColourId
            0x80ffff00,   // mouseOverKeyOverlayColourId
            0xffb6b600    // keyDownOverlayColourId
        };
        const auto isStock = [&] (const juce::Colour& c)
        {
            for (const auto argb : stockDefaults)
                if (c.getARGB() == argb)
                    return true;
            return false;
        };

        ParvatiLookAndFeel lnf;   // defaults to Carbon
        juce::Array<juce::uint32> naturalsSeen;
        for (const auto& theme : getBuiltinThemes())
        {
            lnf.setTheme (theme);
            const auto pal = KeyboardView::resolveColours (lnf);

            char msg[128];

            // (a) theme-resolved: no key colour is a stock JUCE default.
            std::snprintf (msg, sizeof (msg), "%s: natural/sharp/pressed are not stock JUCE key colours", theme.name.toRawUTF8());
            check (! isStock (pal.natural) && ! isStock (pal.sharp) && ! isStock (pal.pressed), msg);

            // The palette is TOKEN-driven: the resolver mirrors the theme tokens.
            std::snprintf (msg, sizeof (msg), "%s: natural == theme.keyWhite, sharp == theme.keyBlack", theme.name.toRawUTF8());
            check (pal.natural.getARGB() == theme.keyWhite.getARGB()
                       && pal.sharp.getARGB() == theme.keyBlack.getARGB(), msg);

            // (b) pressed (accent) differs from both idle fills.
            std::snprintf (msg, sizeof (msg), "%s: pressed differs from idle naturals AND sharps", theme.name.toRawUTF8());
            check (pal.pressed.getARGB() != pal.natural.getARGB()
                       && pal.pressed.getARGB() != pal.sharp.getARGB(), msg);

            // (c) legibility: naturals vs sharps keep >= 1.6:1 WCAG contrast.
            std::snprintf (msg, sizeof (msg), "%s: natural/sharp contrast %.2f:1 (>= 1.6)", theme.name.toRawUTF8(), (double) contrast (pal.natural, pal.sharp));
            check (contrast (pal.natural, pal.sharp) >= 1.6f, msg);

            // Legibility against the strip panel, polarity-aware: on a DARK
            // theme the ELEVATED naturals must step off the recessed panel; on
            // a LIGHT theme the near-white naturals legitimately sit on a
            // light panel (hairline separators + the dark sharps delineate
            // them), so the check asserts the DARK sharps step off it instead.
            const float panelStep = theme.isDark ? contrast (pal.natural, pal.panel)
                                                  : contrast (pal.sharp, pal.panel);
            std::snprintf (msg, sizeof (msg), "%s: %s step off the panel (>= 1.3:1)",
                           theme.name.toRawUTF8(), theme.isDark ? "naturals" : "sharps");
            check (panelStep >= 1.3f, msg);

            // (d-part1) themes are individually resolved (all six naturals distinct).
            naturalsSeen.add (pal.natural.getARGB());
        }
        check (naturalsSeen.size() == static_cast<int> (getBuiltinThemes().size()),
               "every theme resolves a DISTINCT natural-key colour (all themes)");

        // (d-part2) a LIVE keyboard re-resolves on theme switch (refresh seam).
        kb.removeFromDesktop();
        kb.setLookAndFeel (&lnf);
        lnf.setTheme (carbonTheme());
        const auto before = KeyboardView::resolveColours (kb.getLookAndFeel());
        lnf.setTheme (immutableTheme());
        kb.refresh();
        const auto after = KeyboardView::resolveColours (kb.getLookAndFeel());
        check (before.natural.getARGB() != after.natural.getARGB()
                   && before.sharp.getARGB() != after.sharp.getARGB(),
               "theme switch + refresh() re-resolves the live keyboard palette");
        kb.setLookAndFeel (nullptr);   // detach before lnf leaves scope
    }

    // ---- [8] Wheels: mouse-wheel scrolling must NEVER tweak their values ----
    // (The ParamControl idiom, applied to the pitch/mod wheels: a wheel scroll
    // over the strip is page scrolling. Synthetic MouseWheelDetails are fed to
    // each slider's mouseWheelMove directly; the values must not move.)
    std::printf ("[9] Wheel-scroll does not tweak the pitch/mod wheels\n");
    {
        WheelsComponent wheels;
        wheels.setBounds (0, 0, 200, 160);

        float pitch = 0.0f, mod = 0.0f;
        wheels.onPitch = [&pitch] (float v) { pitch = v; };
        wheels.onMod   = [&mod]   (float v) { mod = v; };

        // Find the two Sliders inside (pitch is bipolar vertical, mod is 0..1).
        juce::Slider* pitchSlider = nullptr;
        juce::Slider* modSlider = nullptr;
        juce::Array<juce::Component*> nodes { &wheels };
        for (int i = 0; i < nodes.size(); ++i)
        {
            auto* c = nodes.getUnchecked (i);
            if (auto* s = dynamic_cast<juce::Slider*> (c))
            {
                if (s->getMinimum() < 0.0f && pitchSlider == nullptr)
                    pitchSlider = s;
                else if (s->getMinimum() >= 0.0f && modSlider == nullptr)
                    modSlider = s;
            }
            for (auto* ch : c->getChildren())
                nodes.add (ch);
        }
        check (pitchSlider != nullptr && modSlider != nullptr,
               "wheels expose a pitch (-1..1) and a mod (0..1) slider");

        const auto mouseSrc = juce::Desktop::getInstance().getMainMouseSource();
        auto wheelOn = [&] (juce::Slider* sl)
        {
            const auto centre = sl->getLocalBounds().getCentre().toFloat();
            const auto me = juce::MouseEvent (mouseSrc, centre,
                                              juce::ModifierKeys().withFlags (juce::ModifierKeys::noModifiers),
                                              juce::MouseInputSource::defaultPressure,
                                              juce::MouseInputSource::defaultOrientation,
                                              juce::MouseInputSource::defaultRotation,
                                              juce::MouseInputSource::defaultTiltX,
                                              juce::MouseInputSource::defaultTiltY,
                                              sl, sl,
                                              juce::Time::getCurrentTime(), centre,
                                              juce::Time::getCurrentTime(), 1, false);
            juce::MouseWheelDetails wheel;
            wheel.deltaX = 0.0f;
            wheel.deltaY = 0.5f;         // a healthy scroll notch
            wheel.isSmooth = false;
            wheel.isReversed = false;
            for (int k = 0; k < 4; ++k)
                sl->mouseWheelMove (me, wheel);
        };

        if (pitchSlider != nullptr)
        {
            const double before = pitchSlider->getValue();
            wheelOn (pitchSlider);
            check (pitchSlider->getValue() == before && pitch == 0.0f,
                   "4 scroll notches over the PITCH wheel leave the value untouched");
        }
        if (modSlider != nullptr)
        {
            const double before = modSlider->getValue();
            wheelOn (modSlider);
            check (modSlider->getValue() == before && mod == 0.0f,
                   "4 scroll notches over the MOD wheel leave the value untouched");
        }
    }

    std::printf ("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}

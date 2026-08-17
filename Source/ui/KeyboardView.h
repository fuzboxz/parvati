// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// KeyboardView — a virtual keyboard that (a) reflects notes currently sounding
// in the engine (driven by the editor's timer via latchNoteOn/Off) and (b) lets
// the user click-and-drag to play notes (routed to the engine through the
// NoteCallback set by the editor). It wraps a juce::MidiKeyboardComponent fed by
// an owned juce::MidiKeyboardState used purely for visual latching — no engine
// coupling lives inside this component (Phase 4 wires the editor in).
//
// TWO-OCTAVE LARGE-KEY design: the strip shows EXACTLY C3..C5 (48..72 — 25
// keys, 15 white), never the full MIDI range, and resized() stretches one
// white key to width/15, so the keys are LARGE (~80pt per white key at a
// 1200pt strip) and fill the whole component — the editor hosts the strip
// TALL (it covers the bottom matrix row when [KBD] is toggled on). The window
// centres on middle C (C4), the old musical-typing default base and the centre
// of typical playing. Engine activity outside the window does not light a key
// (see latchNoteOn); the QWERTY musical-typing row is clamped inside the
// window (see keyPressed).
//
// Colours are read from the editor-wide ParvatiLookAndFeel (inherited through
// the component tree) in applyThemeColours(); call refresh() after adding the
// component and whenever the theme changes. Phase 3 of docs/UI_MODERNIZATION_PLAN.md.

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>   // juce::MidiKeyboardComponent
#include <juce_gui_basics/juce_gui_basics.h>     // juce::MidiKeyboardState, Component

#include <functional>
#include <map>
#include <memory>

#include "ParvatiLookAndFeel.h"   // theme accessor via getLookAndFeel()

//==============================================================================
class KeyboardView : public juce::Component
{
public:
    /** Fired on user click/drag of a key. midiNote, isOn (true=press, false=release),
        velocity (0..1, 0 on release). */
    using NoteCallback = std::function<void (int midiNote, bool isOn, float velocity)>;

    /** Fired continuously while a key is held (press or drag) with the current
        Y-position "pressure" (0..1, higher = lower on the key — the same
        mapping as velocityFromEvent). The editor routes this to the engine as
        channel pressure (Aftertouch), so moving the finger up/down a held key
        modulates AT-routed destinations live. Called on every drag move AND
        right after each note-on. */
    using PressureCallback = std::function<void (float pressure01)>;

    /** Wire the continuous-pressure channel (see PressureCallback). */
    void setPressureCallback (PressureCallback cb);

    KeyboardView();
    ~KeyboardView() override;

    //==========================================================================
    // Engine-activity reflection (the editor's timer calls these so played notes
    // light up). Uses an internal channel so latching never collides with the
    // keyboard's own click highlight.
    void latchNoteOn  (int midiNote, float velocity);
    void latchNoteOff (int midiNote);

    //==========================================================================
    // User click -> engine injection (set by the editor in Phase 4).
    void setNoteCallback (NoteCallback cb);

    /** Fired whenever the Ableton-style computer-keyboard settings change
        (Z/X octave, C/V velocity). Carries the new values so the editor can
        surface feedback in the status/tooltip bar. Also fires once from
        setSettingsChangedCallback (initial state). */
    using SettingsChangedCallback = std::function<void (int octaveBaseMidiNote, int velocity0to127)>;
    void setSettingsChangedCallback (SettingsChangedCallback cb);

    /** Current musical-typing settings (for tooltip/status display). */
    int  qwertyOctaveBase()  const noexcept { return octaveBase_; }
    int  qwertyVelocity127() const noexcept { return juce::roundToInt (qwertyVelocity_ * 127.0f); }

    /** Shift the octave base by @p semitones (clamped to whole-octave steps by
        callers; e.g. the wheels panel's [<][>] buttons pass +/-12). Moves the
        visible window (applyQwertyWindow) and fires the settings-changed
        callback exactly like the Z/X keys. */
    void shiftOctave (int semitones);

    //==========================================================================
    // Computer-keyboard (musical-typing) playback. When enabled (default), the
    // KeyboardView takes keyboard focus and the QWERTY keys (A W S D F G H J
    // K L ; etc.) play notes INSIDE the visible two-octave window; Z/X shift
    // the base octave down/up (the visible window follows, clamped at the
    // MIDI edges) and C/V step the typing velocity. Pass false to disable
    // (e.g. when a host/component needs the keyboard focus elsewhere).
    void setComputerKeyboardEnabled (bool enabled);

    //==========================================================================
    // Re-apply theme colours. Call after addAndMakeVisible (so the inherited
    // LookAndFeel is visible) and on every theme switch.
    void refresh();

    //==========================================================================
    void paint (juce::Graphics&) override;
    void resized() override;

    // Re-apply the theme colours whenever this component inherits a new
    // LookAndFeel (e.g. when added to the editor tree), so the keys are never
    // briefly shown in the fallback palette.
    void lookAndFeelChanged() override;

    //==========================================================================
    // Computer-keyboard play + accessibility.
    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool isKeyDown) override;
    void mouseDown (const juce::MouseEvent&) override;
    void focusLost (FocusChangeType) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    // Channel used for latching note activity into the owned state_.
    static constexpr int kLatchChannel = 1;

    // ---- The visible window: exactly TWO octaves, C3..C5 inclusive ----
    // 25 keys / 15 white keys. One shared pair of constants feeds the ctor's
    // setAvailableRange, resized()'s white-key fill math and the musical-typing
    // clamp in keyPressed, so the three can never drift apart. The window
    // centres on middle C (C4 = 60).
    static constexpr int kRangeLow  = 48;   // C3 — DEFAULT window bottom (Z/X shift the window; see octaveBase_)
    static constexpr int kRangeHigh = 72;   // C5 — DEFAULT window top (inclusive). Window span = kRangeHigh - kRangeLow

    // juce::MidiKeyboardComponent subclass that intercepts key clicks and routes
    // them to the NoteCallback. Defined in the .cpp (needs the full KeyboardView
    // type to reach owner_).
    struct KeyComp;
    friend struct KeyComp;

    // Declared before keyboard_ so the KeyComp (constructed in the body) can
    // safely reference owner.state_.
    juce::MidiKeyboardState state_;
    std::unique_ptr<KeyComp> keyboard_;

    NoteCallback noteCallback_;
    PressureCallback pressureCallback_;   // continuous Y-pressure while a key is held (-> channel pressure / AT)
    // Per-MouseInputSource -> the midi note it currently holds. The desktop
    // mouse is source 0 (identical to the old single int); on a touchscreen each
    // finger is a separate source, so several notes can sound at once (chords).
    std::map<int, int> mouseDownNotesBySource_;

    // ---- Computer-keyboard (musical-typing) playback ----
    bool computerKeyboardEnabled_ = true;

    // Ableton-style musical-typing base. The base is ALWAYS a C-note (Z/X move
    // it by whole octaves) and doubles as the VISIBLE window's bottom: Z/X
    // shift the base and the two-octave window FOLLOWS (applyQwertyWindow),
    // clamped so the window stays inside 0..127. The QWERTY row (base ..
    // base + kQwertyTopOffset) always fits inside the 24-semitone window.
    int octaveBase_ = kRangeLow;           // musical-typing base = C3 (default window bottom)

    // Ableton-style musical-typing VELOCITY: C/V adjust it in steps of 20/127
    // (default 100/127, Live's computer-keyboard default). Every QWERTY
    // note-on uses it; mouse clicks keep their own event-derived velocity.
    float qwertyVelocity_ { 100.0f / 127.0f };

    std::map<char, int> heldNotes_;        // QWERTY char -> MIDI note currently held

    // Editor-side feedback channel for the Ableton-style settings (Z/X octave,
    // C/V velocity): fired on every change with the new values.
    SettingsChangedCallback onSettingsChanged_;

    // The standard musical-typing layout: char -> semitone offset from octaveBase_.
    // White keys A S D F G H J K L ; black keys W E T Y U O P; spans C..E (1.5 oct).
    static const std::map<char, int>& qwertyMap();
    void releaseHeldComputerNotes();

    // Re-apply the QWERTY window (octaveBase_ .. octaveBase_ + span) to the
    // inner MidiKeyboardComponent and re-layout after a Z/X octave shift.
    void applyQwertyWindow();

    void applyThemeColours();
    void fireNoteCallback (int midiNote, bool isOn, float velocity);

    // Push the event's Y-position through the pressure channel (channel-
    // pressure/AT semantics): the same 0..1 mapping as velocityFromEvent.
    void firePressureFromEvent (const juce::MouseEvent& e);
    float velocityFromEvent (const juce::MouseEvent& e) const;

    // True when a MouseInputSource OTHER than exceptSource currently holds the
    // given note. Multitouch overlap dedup for the KeyComp note routing: the
    // engine must hear each note exactly once even when two sources (fingers /
    // finger + mouse) land on the same key via press or glissando retarget.
    bool noteHeldByOtherSource (int midiNote, int exceptSource) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KeyboardView)
};

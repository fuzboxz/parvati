// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// KeyboardView — a virtual keyboard that (a) reflects notes currently sounding
// in the engine (driven by the editor's timer via latchNoteOn/Off) and (b) lets
// the user click-and-drag to play notes (routed to the engine through the
// NoteCallback set by the editor). It wraps a juce::MidiKeyboardComponent fed by
// an owned juce::MidiKeyboardState used purely for visual latching — no engine
// coupling lives inside this component (Phase 4 wires the editor in).
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

    //==========================================================================
    // Computer-keyboard (musical-typing) playback. When enabled (default), the
    // KeyboardView takes keyboard focus and the QWERTY keys (A W S D F G H J
    // K L ; etc.) play notes; Z/X shift the base octave down/up. Pass false to
    // disable (e.g. when a host/component needs the keyboard focus elsewhere).
    void setComputerKeyboardEnabled (bool enabled);

    //==========================================================================
    // Re-apply theme colours. Call after addAndMakeVisible (so the inherited
    // LookAndFeel is visible) and on every theme switch.
    void refresh();

    //==========================================================================
    void paint (juce::Graphics&) override;
    void resized() override;

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
    int mouseDownNote_ = -1;   // note currently held by the mouse (for clean release)

    // ---- Computer-keyboard (musical-typing) playback ----
    bool computerKeyboardEnabled_ = true;
    int  octaveBase_ = 60;                  // MIDI note of the lowest playable key (C4)
    std::map<char, int> heldNotes_;        // QWERTY char -> MIDI note currently held

    // The standard musical-typing layout: char -> semitone offset from octaveBase_.
    // White keys A S D F G H J K L ; black keys W E T Y U O P; spans C..E (1.5 oct).
    static const std::map<char, int>& qwertyMap();
    void releaseHeldComputerNotes();

    void applyThemeColours();
    void fireNoteCallback (int midiNote, bool isOn, float velocity);
    float velocityFromEvent (const juce::MouseEvent& e) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KeyboardView)
};

// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See KeyboardView.h.

#include "KeyboardView.h"

#include "ParvatiTheme.h"

//==============================================================================
// Internal juce::MidiKeyboardComponent subclass that intercepts key clicks and
// routes them to the owning KeyboardView's NoteCallback. Returning true from
// mouseDownOnKey / mouseDraggedToKey lets the base class light the key through
// the shared MidiKeyboardState (visual latching only).
struct KeyboardView::KeyComp : public juce::MidiKeyboardComponent
{
    explicit KeyComp (KeyboardView& o)
        : juce::MidiKeyboardComponent (o.state_, juce::MidiKeyboardComponent::horizontalKeyboard),
          owner (o)
    {
    }

    bool mouseDownOnKey (int midiNoteNumber, const juce::MouseEvent& e) override
    {
        // Grab focus for the parent so musical-typing works after a click.
        owner.grabKeyboardFocus();

        // If a different note was somehow still held by the mouse, release it
        // cleanly before arming the new one (defensive; should be rare).
        if (owner.mouseDownNote_ >= 0 && owner.mouseDownNote_ != midiNoteNumber)
            owner.fireNoteCallback (owner.mouseDownNote_, false, 0.0f);

        owner.mouseDownNote_ = midiNoteNumber;
        owner.fireNoteCallback (midiNoteNumber, true, owner.velocityFromEvent (e));
        return true;   // base lights the key via the shared state_
    }

    bool mouseDraggedToKey (int midiNoteNumber, const juce::MouseEvent&) override
    {
        // Glissando: let the base re-light each dragged key (return true) but do
        // NOT fire engine note-ons for every key swept — only the originally
        // pressed note is sent to the engine, and its note-off arrives on
        // mouseUp. This keeps the engine free of stuck/duplicate notes while the
        // keyboard still shows the glissando visually.
        juce::ignoreUnused (midiNoteNumber);
        return true;
    }

    void mouseUpOnKey (int midiNoteNumber, const juce::MouseEvent&) override
    {
        juce::ignoreUnused (midiNoteNumber);
        if (owner.mouseDownNote_ >= 0)
            owner.fireNoteCallback (owner.mouseDownNote_, false, 0.0f);
        owner.mouseDownNote_ = -1;
    }

    // NIT-3 stuck-note guard: the base MidiKeyboardComponent::mouseUp only calls
    // mouseUpOnKey when the release lands on a key (note >= 0). A release off
    // the keys — or off the component — would otherwise leave mouseDownNote_
    // held. Route the release through the base first (so on-key releases still
    // clear via mouseUpOnKey), then release anything still held as a fallback.
    void mouseUp (const juce::MouseEvent& e) override
    {
        juce::MidiKeyboardComponent::mouseUp (e);
        if (owner.mouseDownNote_ >= 0)
        {
            owner.fireNoteCallback (owner.mouseDownNote_, false, 0.0f);
            owner.mouseDownNote_ = -1;
        }
    }

    KeyboardView& owner;
};

//==============================================================================
KeyboardView::KeyboardView()
{
    keyboard_ = std::make_unique<KeyComp> (*this);
    addAndMakeVisible (*keyboard_);

    // ~5 octaves (C2..C7) with scroll arrows so narrow windows can still pan.
    keyboard_->setAvailableRange (36, 96);
    keyboard_->setScrollButtonsVisible (true);

    // Computer-keyboard play: this component owns the focus so it receives the
    // musical-typing keys (the inner KeyComp never takes focus itself).
    setWantsKeyboardFocus (true);
    keyboard_->setWantsKeyboardFocus (false);

    // Accessibility name/description/help (read by the default handler).
    setTitle ("Virtual Keyboard");
    setDescription ("Virtual keyboard");
    setHelpText ("Click a key, or type on the computer keyboard (A W S D F G H J K L ; with "
                 "W E T Y U O P for the sharps) to play notes. Z and X shift the octave "
                 "down and up.");

    applyThemeColours();   // fallback colours until added to the themed tree
}

KeyboardView::~KeyboardView() {}

//==============================================================================
void KeyboardView::latchNoteOn (int midiNote, float velocity)
{
    state_.noteOn (kLatchChannel, midiNote, juce::jlimit (0.0f, 1.0f, velocity));
}

void KeyboardView::latchNoteOff (int midiNote)
{
    state_.noteOff (kLatchChannel, midiNote, 0.0f);
}

void KeyboardView::setNoteCallback (NoteCallback cb)
{
    noteCallback_ = std::move (cb);
}

//==============================================================================
void KeyboardView::refresh()
{
    applyThemeColours();
    repaint();
}

void KeyboardView::paint (juce::Graphics& g)
{
    const ParvatiTheme* t = nullptr;
    if (auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel()))
        t = lnf->getTheme();

    g.fillAll (t != nullptr ? t->windowBackground : juce::Colour (0xff141419));
}

void KeyboardView::resized()
{
    // Span the full width: size one white key so the fixed range [36,96]
    // fills the component. Without this the MidiKeyboardComponent left-aligns
    // the keys at its default width and leaves an empty block on the right.
    const int lo = 36, hi = 96;
    int whiteKeys = 0;
    for (int n = lo; n <= hi; ++n)
    {
        const int d = n % 12;
        if (d == 0 || d == 2 || d == 4 || d == 5 || d == 7 || d == 9 || d == 11)
            ++whiteKeys;
    }
    if (whiteKeys > 0 && getWidth() > 0)
        keyboard_->setKeyWidth (static_cast<float> (getWidth()) / static_cast<float> (whiteKeys));
    keyboard_->setScrollButtonsVisible (false);   // keys fill the width: nothing to scroll
    keyboard_->setBounds (getLocalBounds());
}

//==============================================================================
void KeyboardView::applyThemeColours()
{
    if (keyboard_ == nullptr)
        return;

    const ParvatiTheme* t = nullptr;
    if (auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel()))
        t = lnf->getTheme();

    juce::Colour white, black, down, over, line, text, shadow;

    if (t != nullptr)
    {
        // Thematic keys: white/black derive from the theme palette instead of a
        // fixed piano light/dark, so the keyboard recolours on every theme. The
        // accent still colours pressed keys (the down overlay) so activity reads
        // on every theme. Dark themes lift the panel toward a light key; the
        // light Paper theme uses the warm panel header / a deepened window fill.
        white  = t->isDark ? t->panelBackground2.brighter (1.6f)
                           : t->panelHeader;
        black  = t->isDark ? t->windowBackground
                           : t->windowBackground.darker (0.25f);
        down   = t->accent;
        over   = t->accent2.withAlpha (0.45f);
        line   = t->outline;
        text   = t->textDim;
        shadow = t->divider;
    }
    else
    {
        // Fallbacks = Carbon-derived values (in case the component is shown
        // before it inherits the editor's ParvatiLookAndFeel).
        white  = juce::Colour (0xffdcdce4);
        black  = juce::Colour (0xff141419);
        down   = juce::Colour (0xffe8b84b);
        over   = juce::Colour (0xff5b8db8).withAlpha (0.45f);
        line   = juce::Colour (0xff3c3c4a);
        text   = juce::Colour (0xff9a9aa8);
        shadow = juce::Colour (0xff24242e);
    }

    using MK = juce::MidiKeyboardComponent;
    keyboard_->setColour (MK::whiteNoteColourId,            white);
    keyboard_->setColour (MK::blackNoteColourId,            black);
    keyboard_->setColour (MK::keySeparatorLineColourId,     line);
    keyboard_->setColour (MK::mouseOverKeyOverlayColourId,  over);
    keyboard_->setColour (MK::keyDownOverlayColourId,       down);
    keyboard_->setColour (MK::textLabelColourId,            text);
    keyboard_->setColour (MK::shadowColourId,               shadow);

    keyboard_->repaint();
}

void KeyboardView::fireNoteCallback (int midiNote, bool isOn, float velocity)
{
    if (noteCallback_)
        noteCallback_ (midiNote, isOn, velocity);
}

float KeyboardView::velocityFromEvent (const juce::MouseEvent& e) const
{
    const int h = juce::jmax (1, getHeight());
    // Click near the top of a key => louder (like striking the end of a key).
    const float norm = juce::jlimit (0.0f, 1.0f, e.position.y / static_cast<float> (h));
    return 0.3f + 0.7f * (1.0f - norm);
}

//==========================================================================
// Computer-keyboard (musical-typing) playback.
const std::map<char, int>& KeyboardView::qwertyMap()
{
    // Standard musical-typing layout, value = semitone offset from octaveBase_.
    // White keys: A S D F G H J K L  (C D E F G A B C D)
    // Black keys: W E   T Y U   O P  (C# D#  F# G# A#  D#... in next octave)
    // Spans 17 semitones (C..E of the next octave).
    static const std::map<char, int> m = {
        { 'a', 0  }, { 'w', 1  }, { 's', 2  }, { 'e', 3  }, { 'd', 4  },
        { 'f', 5  }, { 't', 6  }, { 'g', 7  }, { 'y', 8  }, { 'h', 9  },
        { 'u', 10 }, { 'j', 11 }, { 'k', 12 }, { 'o', 13 }, { 'l', 14 },
        { 'p', 15 }, { ';', 16 }
    };
    return m;
}

void KeyboardView::setComputerKeyboardEnabled (bool enabled)
{
    computerKeyboardEnabled_ = enabled;
    if (! enabled)
        releaseHeldComputerNotes();
}

void KeyboardView::releaseHeldComputerNotes()
{
    for (const auto& kv : heldNotes_)
    {
        fireNoteCallback (kv.second, false, 0.0f);
        latchNoteOff (kv.second);
    }
    heldNotes_.clear();
}

bool KeyboardView::keyPressed (const juce::KeyPress& key)
{
    if (! computerKeyboardEnabled_)
        return false;

    // Never hijack modifier combos (Cmd/Ctrl/Alt) so the editor's Cmd +/-/0
    // zoom and host shortcuts still work while the keyboard has focus.
    const auto& mods = key.getModifiers();
    if (mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown())
        return false;

    const auto wc = key.getTextCharacter();
    const char c = static_cast<char> (juce::CharacterFunctions::toLowerCase (wc));

    // Octave shift: Z = down an octave, X = up an octave.
    if (c == 'z')
    {
        octaveBase_ = juce::jmax (0, octaveBase_ - 12);
        return true;
    }
    if (c == 'x')
    {
        octaveBase_ = juce::jmin (127 - 16, octaveBase_ + 12);
        return true;
    }

    const auto& m = qwertyMap();
    const auto it = m.find (c);
    if (it == m.end())
        return false;   // not a musical key -> let the parent handle it

    // Ignore auto-repeat of an already-held key (no re-trigger).
    if (heldNotes_.find (c) != heldNotes_.end())
        return true;

    const int note = juce::jlimit (0, 127, octaveBase_ + it->second);
    heldNotes_[c] = note;
    fireNoteCallback (note, true, 0.8f);
    latchNoteOn (note, 0.8f);
    return true;
}

bool KeyboardView::keyStateChanged (bool isKeyDown)
{
    juce::ignoreUnused (isKeyDown);
    if (! computerKeyboardEnabled_)
        return false;

    bool changed = false;
    // Release any held computer-key whose key is no longer physically down.
    for (auto it = heldNotes_.begin(); it != heldNotes_.end(); )
    {
        const int keyCode = static_cast<int> (static_cast<unsigned char> (it->first));
        if (! juce::KeyPress::isKeyCurrentlyDown (keyCode))
        {
            fireNoteCallback (it->second, false, 0.0f);
            latchNoteOff (it->second);
            it = heldNotes_.erase (it);
            changed = true;
        }
        else
        {
            ++it;
        }
    }
    return changed;
}

void KeyboardView::mouseDown (const juce::MouseEvent&)
{
    // Clicking the keyboard's own background grabs focus for musical typing.
    grabKeyboardFocus();
}

void KeyboardView::focusLost (FocusChangeType)
{
    // Avoid stuck notes if focus moves away while a computer-key is held.
    releaseHeldComputerNotes();
}

//==========================================================================
std::unique_ptr<juce::AccessibilityHandler> KeyboardView::createAccessibilityHandler()
{
    // Role `group`: there is no dedicated piano/keyboard role in JUCE. The
    // name/description/help set on the component are announced to screen
    // readers by the default handler (getTitle/getDescription/getHelp).
    return std::make_unique<juce::AccessibilityHandler> (*this,
            juce::AccessibilityRole::group);
}

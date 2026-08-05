// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See KeyboardView.h.

#include "KeyboardView.h"

#include "ParvatiTheme.h"

#include <cmath>

//==============================================================================
// LCD "pixel screen" rendering for the on-screen keyboard. The keys are drawn as
// fields of square "pixels" on an integer grid so the keyboard reads as the same
// blocky segmented display as the envelope/LFO preview (EnvelopeDisplay), rather
// than smooth vector keys. Every cell is a whole-pixel Rectangle<int> (drawn via
// the integer fillRect overload) so there are no fractional edges / cut-off
// pixels, even on scaled (HiDPI / zoomed) surfaces. The cell constants match
// EnvelopeDisplay (3px cells) so the two surfaces share one visual language.
namespace
{
    constexpr int kLcdCell  = 3;   // lit pixel size (px)  — matches EnvelopeDisplay
    constexpr int kLcdPitch = 4;   // cell + 1px grid gap  — matches EnvelopeDisplay

    struct LcdPalette
    {
        juce::Colour screenBase;   // panelBackground — LCD gap / between-key fill
        juce::Colour outline;
        juce::Colour accent;       // lit natural keys
        juce::Colour accent2;      // pressed / active keys
        juce::Colour track;        // faint backdrop grid
        juce::Colour blackBase;    // unlit sharp keys (darkest)
    };

    LcdPalette resolveLcd (const juce::LookAndFeel& lnf)
    {
        const ParvatiTheme* t = nullptr;
        if (const auto* p = dynamic_cast<const ParvatiLookAndFeel*> (&lnf))
            t = p->getTheme();

        if (t != nullptr)
            return { t->panelBackground, t->outline,    t->accent,  t->accent2,
                     t->knobTrack,       t->windowBackground };

        // Carbon-derived fallback (only before the editor's L&F is inherited).
        return { juce::Colour (0xff24242e), juce::Colour (0xff3c3c4a), juce::Colour (0xffe8b84b),
                 juce::Colour (0xff5b8db8), juce::Colour (0xff3c3c4a), juce::Colour (0xff141419) };
    }

    // Snap a float rect to the nearest whole-pixel rect (no fractional edges).
    juce::Rectangle<int> roundedInt (juce::Rectangle<float> a)
    {
        return { static_cast<int> (std::round (a.getX())),
                 static_cast<int> (std::round (a.getY())),
                 static_cast<int> (std::round (a.getWidth())),
                 static_cast<int> (std::round (a.getHeight())) };
    }

    // Fill an integer @p area with a grid of @p cell-sized cells on a @p pitch
    // lattice, starting at the area's top-left corner. Every cell is an integer
    // Rectangle<int> (drawn via the integer fillRect overload) so each renders
    // as a clean whole-pixel square with no fractional / cut-off edges. The last
    // row/column stops short of the edge (leaving a whole-pixel margin) instead
    // of drawing a partial cell. Rows are shared across keys (same Y range);
    // columns begin at each key's left edge so every key fills cleanly.
    void fillLcdCells (juce::Graphics& g, juce::Rectangle<int> area, juce::Colour col,
                       int cell = kLcdCell, int pitch = kLcdPitch)
    {
        const int right  = area.getRight();
        const int bottom = area.getBottom();
        g.setColour (col);
        for (int y = area.getY(); y + cell <= bottom; y += pitch)
            for (int x = area.getX(); x + cell <= right; x += pitch)
                g.fillRect (juce::Rectangle<int> (x, y, cell, cell));
    }
} // namespace

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

    //----------------------------------------------------------------------
    // Integer key layout: snap every key's position/width to whole pixels at the
    // source so white AND black keys land on one shared integer grid (no
    // fractional sizes, no misaligned sharps). The base layout is fractional
    // (keyWidth * ratio maths); rounding here fixes geometry for BOTH drawing
    // (getRectangleForKey) and hit-testing (getNoteAndVelocityAtPosition), which
    // both derive from this, so they stay perfectly consistent. White notes stay
    // contiguous because adjacent whites share an exact boundary value that
    // rounds identically for both keys.
    juce::Range<float> getKeyPosition (int midiNoteNumber, float targetKeyWidth) const override
    {
        auto r = juce::MidiKeyboardComponent::getKeyPosition (midiNoteNumber, targetKeyWidth);
        const float s = std::round (r.getStart());
        const float e = std::round (r.getEnd());
        return { s, juce::jmax (e, s + 1.0f) };   // never collapse a key to zero width
    }

    //----------------------------------------------------------------------
    // Pixelated LCD key rendering: naturals are fields of LIT cells (accent),
    // sharps are dark recessed cells with the faint backdrop grid — the same
    // segmented-display look as EnvelopeDisplay. Overrides the stock smooth
    // vector keys (drawWhiteNote/drawBlackNote are the virtual hooks the final
    // drawWhiteKey/drawBlackKey delegate to).
    void drawWhiteNote (int midiNoteNumber, juce::Graphics& g, juce::Rectangle<float> area,
                        bool isDown, bool isOver, juce::Colour lineColour, juce::Colour textColour) override
    {
        juce::ignoreUnused (lineColour);   // pixel grid + seam replace the separator line
        const auto lcd = resolveLcd (getLookAndFeel());

        juce::Colour fill = lcd.accent;
        if (isDown) fill = lcd.accent2;                              // active colour
        if (isOver) fill = fill.overlaidWith (lcd.accent2.withAlpha (0.45f));
        const auto ir = roundedInt (area);
        fillLcdCells (g, ir, fill);

        // 1px seam in the screen-base colour: adjacent naturals would otherwise
        // merge into a single lit block. Drawn on the integer grid (rounded
        // left edge, full key height) so it is a clean whole-pixel line.
        g.setColour (lcd.screenBase);
        g.fillRect (juce::Rectangle<int> (ir.getX(), ir.getY(), 1, ir.getHeight ()));

        // Octave label on each C — crisp text over the pixel field, like the
        // envelope display's title overlay.
        const auto text = getWhiteNoteText (midiNoteNumber);
        if (text.isNotEmpty() && area.getHeight() > 16.0f)
        {
            auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel());
            g.setColour (textColour.isTransparent() ? lcd.accent.contrasting() : textColour);
            g.setFont (lnf ? lnf->appFont (juce::jmin (11.0f, area.getWidth() * 0.6f), juce::Font::plain)
                           : juce::Font (juce::FontOptions (11.0f)));
            g.drawText (text, area.withTrimmedLeft (1.0f).withTrimmedBottom (2.0f),
                        juce::Justification::centredBottom, false);
        }
    }

    void drawBlackNote (int /*midiNoteNumber*/, juce::Graphics& g, juce::Rectangle<float> area,
                        bool isDown, bool isOver, juce::Colour noteFillColour) override
    {
        juce::ignoreUnused (noteFillColour);   // LCD colours come from the theme
        const auto lcd = resolveLcd (getLookAndFeel());

        const auto ir = roundedInt (area);

        if (isDown)
        {
            // Pressed sharp = lit accent2 cells, matching a pressed natural.
            fillLcdCells (g, ir, lcd.accent2);
        }
        else
        {
            // Unlit sharp = dark recessed region with the faint LCD backdrop
            // grid (the same inactive-area treatment as EnvelopeDisplay).
            g.setColour (lcd.blackBase);
            g.fillRect (ir);
            fillLcdCells (g, ir, lcd.track.withAlpha (0.12f));
            if (isOver)
            {
                g.setColour (lcd.accent2.withAlpha (0.45f));
                g.fillRect (ir);
            }
        }

        // Crisp outline so the black key reads against the lit naturals beneath.
        if (! lcd.outline.isTransparent())
        {
            g.setColour (lcd.outline);
            g.drawRect (ir);
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

void KeyboardView::lookAndFeelChanged()
{
    // The keyboard reads its colours from the inherited ParvatiLookAndFeel; re-
    // apply them the instant a new L&F is set on this component (e.g. when it is
    // added to the editor tree) so the keys are shown themed right away, with no
    // reliance on an external refresh() call.
    applyThemeColours();
}

void KeyboardView::paint (juce::Graphics& g)
{
    const ParvatiTheme* t = nullptr;
    if (auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel()))
        t = lnf->getTheme();

    g.fillAll (t != nullptr ? t->panelBackground : juce::Colour (0xff24242e));
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
        // The keyboard is drawn as a pixelated LCD screen (see KeyComp's
        // drawWhiteNote/drawBlackNote): the component background is the screen
        // base, naturals are fields of lit accent cells, and sharps are dark
        // recessed cells. The override reads the theme directly for the cell
        // colours; these IDs drive the base fill plus the state/label overlays
        // the override still consults.
        white  = t->panelBackground;            // LCD screen base (pixel gaps, between keys)
        black  = t->windowBackground;           // sharp base (override reads theme directly)
        down   = t->accent2;                    // pressed-key overlay
        over   = t->accent2.withAlpha (0.45f);  // hover overlay
        line   = t->outline;                    // keyboard bottom edge (drawKeyboardBackground)
        text   = t->windowBackground;           // C-label text (contrasts with accent naturals)
        shadow = juce::Colour (0x00000000);     // flat LCD surface — no top shadow gradient
    }
    else
    {
        // Carbon-derived fallback (shown only before the editor's
        // ParvatiLookAndFeel is inherited).
        white  = juce::Colour (0xff24242e);   // panelBackground (LCD screen base)
        black  = juce::Colour (0xff141419);   // windowBackground
        down   = juce::Colour (0xff5b8db8);   // accent2 (steel)
        over   = juce::Colour (0xff5b8db8).withAlpha (0.45f);
        line   = juce::Colour (0xff3c3c4a);   // outline (bottom edge)
        text   = juce::Colour (0xff141419);
        shadow = juce::Colour (0x00000000);
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

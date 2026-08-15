// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See KeyboardView.h.

#include "KeyboardView.h"

#include "ParvatiTheme.h"

#include <cmath>

//==============================================================================
// Flat, panel-integrated rendering for the on-screen keyboard. The keys are
// painted FLAT — a solid ivory natural fill and a dark recessed sharp fill, a
// 1px hairline seam between naturals, and a solid brand-accent fill for any
// pressed/latched key — so the keyboard reads as a calm, integrated control
// surface that matches the plugin's flat card aesthetic (no skeuomorphic
// gradients / sheens / shadows). The whole strip sits on a rounded-top panel
// (see KeyboardView::paint / KeyComp::paint) so it reads as one integrated
// bottom bar rather than a keyboard abutting the content. Keys stay pixel-
// aligned: every key is snapped to a whole-pixel Rectangle<int> (via
// roundedInt) so there are no fractional edges / cut-off pixels even on scaled
// (HiDPI / zoomed) surfaces.
// THEME-SAFE by contract: NO colour literals live here. Every colour is read
// from ParvatiTheme tokens via resolveLcd, so the whole keyboard re-tints on a
// theme switch with no extra wiring.
namespace
{
    // Rounded-panel corner radius — matches the GroupComponent cards, rounded on
    // the TOP corners only (the keyboard's bottom is flush with the editor edge).
    constexpr float kPanelCorner = 7.0f;

    struct KeyPalette
    {
        juce::Colour outline;    // 1px hairline seam between naturals
        juce::Colour accent;     // pressed / latched fill (brand accent)
        juce::Colour accent2;    // hover wash
        juce::Colour blackBase;  // unlit sharp keys (darkest recessed fill)
        juce::Colour keyWhite;   // natural (white) key resting fill (piano white)
    };

    KeyPalette resolveLcd (const juce::LookAndFeel& lnf)
    {
        const ParvatiTheme* t = nullptr;
        if (const auto* p = dynamic_cast<const ParvatiLookAndFeel*> (&lnf))
            t = p->getTheme();

        if (t != nullptr)
            return { t->outline, t->accentPrimary, t->accentSecondary,
                     t->backgroundBase, t->keyWhite };

        // Carbon-derived fallback (only before the editor's L&F is inherited).
        // keyWhite reuses Carbon's factory value so no new colour literal lives
        // outside the theme factories.
        return { juce::Colour (0xff3c3c4a), juce::Colour (0xffe8b84b), juce::Colour (0xff5b8db8),
                 juce::Colour (0xff141419), carbonTheme().keyWhite };
    }

    // Snap a float rect to the nearest whole-pixel rect (no fractional edges).
    juce::Rectangle<int> roundedInt (juce::Rectangle<float> a)
    {
        return { static_cast<int> (std::round (a.getX())),
                 static_cast<int> (std::round (a.getY())),
                 static_cast<int> (std::round (a.getWidth())),
                 static_cast<int> (std::round (a.getHeight())) };
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

        // Multitouch: each MouseInputSource (desktop mouse = 0, each finger =
        // 1+) tracks its OWN note, so several can sound at once (chords).
        // Release this source's previously-held note (if any) before arming.
        const int src = e.source.getIndex();
        const auto it = owner.mouseDownNotesBySource_.find (src);
        if (it != owner.mouseDownNotesBySource_.end() && it->second != midiNoteNumber)
            owner.fireNoteCallback (it->second, false, 0.0f);

        owner.mouseDownNotesBySource_[src] = midiNoteNumber;
        owner.fireNoteCallback (midiNoteNumber, true, owner.velocityFromEvent (e));
        return true;   // base lights the key via the shared state_
    }

    bool mouseDraggedToKey (int midiNoteNumber, const juce::MouseEvent&) override
    {
        // Glissando is visual-only (the base re-lights each swept key); the
        // engine holds each source's originally-pressed note until THAT source
        // releases. Keeps the engine free of stuck/duplicate notes per finger.
        juce::ignoreUnused (midiNoteNumber);
        return true;
    }

    void mouseUpOnKey (int midiNoteNumber, const juce::MouseEvent& e) override
    {
        juce::ignoreUnused (midiNoteNumber);
        releaseSourceNote (e.source.getIndex());
    }

    // Stuck-note guard: the base MidiKeyboardComponent::mouseUp only calls
    // mouseUpOnKey when the release lands on a key. A release off the keys / off
    // the component would otherwise leave THIS source's note held. Route through
    // the base first (on-key releases still clear via mouseUpOnKey), then clear
    // the source's note as a fallback. Per-source so multitouch releases don't
    // clobber other held fingers.
    void mouseUp (const juce::MouseEvent& e) override
    {
        juce::MidiKeyboardComponent::mouseUp (e);
        releaseSourceNote (e.source.getIndex());
    }

    // Release + erase one source's held note (no-op if that source holds none).
    void releaseSourceNote (int src)
    {
        const auto it = owner.mouseDownNotesBySource_.find (src);
        if (it != owner.mouseDownNotesBySource_.end())
        {
            owner.fireNoteCallback (it->second, false, 0.0f);
            owner.mouseDownNotesBySource_.erase (it);
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
    // Smooth vector key rendering: naturals are clean WHITE fills (piano
    // convention) — resting keyWhite, hover a faint accent2 wash, pressed an
    // accent tint + accent top-gleam; sharps are dark recessed fills (pressed
    // -> brightened accent2), each with a multi-stop depth gradient, a crisp
    // top-edge highlight and a faint bottom-edge shadow so the keys read sleek
    // and physical; black keys get softly rounded tops + a crisp accent edge.
    // Overrides the stock juce keys (drawWhiteNote/drawBlackNote are the
    // virtual hooks the final drawWhiteKey/drawBlackKey delegate to).
    void drawWhiteNote (int midiNoteNumber, juce::Graphics& g, juce::Rectangle<float> area,
                        bool isDown, bool isOver, juce::Colour lineColour, juce::Colour textColour) override
    {
        juce::ignoreUnused (lineColour, textColour);   // flat fill + hairline seam replace them
        const auto pal = resolveLcd (getLookAndFeel());

        // FLAT fills: naturals are clean WHITE (piano convention) — a solid
        // keyWhite resting fill, a faint accent2 hover wash, and a SOLID brand
        // accent fill when pressed/latched so an active key clearly pops above
        // its resting-white neighbours. Every colour derives from the palette
        // tokens only — no literals, no gradient.
        juce::Colour fill = pal.keyWhite;
        if (isOver && ! isDown) fill = fill.overlaidWith (pal.accent2.withAlpha (0.14f));
        if (isDown)             fill = pal.accent;

        const auto ir = roundedInt (area);
        g.setColour (fill);
        g.fillRect (ir);

        // Pressed accent top-gleam: a brighter-accent strip across the key top
        // so an active key reads clearly even against an accent-coloured fill.
        if (isDown && ir.getWidth() > 0 && ir.getHeight() > 4)
        {
            g.setColour (pal.accent.brighter (0.30f));
            g.fillRect (ir.withHeight (2));
        }

        // 1px HAIRLINE seam between contiguous naturals (low-alpha outline):
        // keeps adjacent white keys cleanly separated without a heavy key border
        // or opening a gap. Flat — no inset shadow.
        g.setColour (pal.outline.withAlpha (0.25f));
        g.fillRect (juce::Rectangle<int> (ir.getX(), ir.getY(), 1, ir.getHeight ()));

        // Octave label on each C — crisp text that contrasts with the live fill
        // (dark on ivory, dark on the bright accent).
        const auto text = getWhiteNoteText (midiNoteNumber);
        if (text.isNotEmpty() && area.getHeight() > 16.0f)
        {
            auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel());
            g.setColour (fill.contrasting ());
            g.setFont (lnf ? lnf->appFont (juce::jmin (11.0f, area.getWidth() * 0.6f), juce::Font::plain)
                           : juce::Font (juce::FontOptions (11.0f)));
            g.drawText (text, area.withTrimmedLeft (1.0f).withTrimmedBottom (2.0f),
                        juce::Justification::centredBottom, false);
        }
    }

    void drawBlackNote (int /*midiNoteNumber*/, juce::Graphics& g, juce::Rectangle<float> area,
                        bool isDown, bool isOver, juce::Colour noteFillColour) override
    {
        juce::ignoreUnused (noteFillColour);   // key colours come from the theme
        const auto pal = resolveLcd (getLookAndFeel());

        // FLAT fills: dark recessed blackBase, a faint accent2 hover wash, and a
        // SOLID brand accent when pressed (matching the naturals' active state).
        juce::Colour fill = pal.blackBase;
        if (isOver && ! isDown) fill = fill.overlaidWith (pal.accent2.withAlpha (0.30f));
        if (isDown)             fill = pal.accent;

        const auto ir = roundedInt (area);

        // Subtle rounded-top cap (cosmetic only — hit-testing still uses the
        // integer getKeyPosition geometry). No outline: the dark key reads
        // against the ivory naturals by tonal contrast alone.
        const float corner = juce::jlimit (1.0f, 2.0f, area.getWidth() * 0.20f);
        const auto fr = ir.toFloat();
        juce::Path key;
        key.addRoundedRectangle (fr.getX(), fr.getY(), fr.getWidth(), fr.getHeight(),
                                 corner, corner, true, true, false, false);

        g.setColour (fill);
        g.fillPath (key);
    }

    // The owning KeyboardView paints a rounded-top panel behind this component;
    // clip the stock keys to that same rounded shape so the square key corners
    // don't paint over the panel's rounded top corners. The strip then reads as
    // an integrated rounded card (matching the GroupComponent cards) instead of
    // a keyboard abutting the content. Cosmetic only — hit-testing uses the
    // integer getKeyPosition geometry.
    void paint (juce::Graphics& g) override
    {
        juce::Graphics::ScopedSaveState ss (g);
        const auto b = getLocalBounds().toFloat();
        juce::Path clip;
        clip.addRoundedRectangle (b.getX(), b.getY(), b.getWidth(), b.getHeight(),
                                  kPanelCorner, kPanelCorner, true, true, false, false);
        g.reduceClipRegion (clip);
        juce::MidiKeyboardComponent::paint (g);
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
    // Rounded-TOP panel (flush bottom): the strip reads as an integrated bottom
    // bar matching the GroupComponent cards (containerFill + 7px top corner).
    // The bottom is square — flush with the editor edge. The inner KeyComp's
    // own background is transparent (whiteNoteColourId set transparent in
    // applyThemeColours, which also flips it non-opaque), so this panel shows
    // through between/around the keys; the KeyComp's paint() clips the keys to
    // this same rounded shape so square key corners never cover it.
    const ParvatiTheme* t = nullptr;
    if (auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel()))
        t = lnf->getTheme();

    const auto bounds = getLocalBounds().toFloat();
    juce::Path panel;
    panel.addRoundedRectangle (bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight(),
                               kPanelCorner, kPanelCorner, true, true, false, false);
    g.setColour (t != nullptr ? t->containerFill : juce::Colour (0xff24242e));
    g.fillPath (panel);
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
        // The keys are drawn by the custom drawWhiteNote/drawBlackNote, which
        // read the theme directly for every key colour. So the only
        // MidiKeyboardComponent colour ID that MATTERS here is whiteNoteColourId:
        // the final drawKeyboardBackground fillAll()s it, so it MUST be
        // transparent for the owning KeyboardView's rounded panel (see
        // KeyboardView::paint) to show through between/around the keys. A side
        // effect of setting it transparent is colourChanged() calls
        // setOpaque(false) on the KeyComp — exactly what lets that rounded panel
        // show through. The remaining IDs are vestigial (the override ignores
        // them) but set to coherent theme values for correctness.
        white  = juce::Colour (0x00000000);              // transparent bg (panel shows through)
        black  = t->backgroundBase;                      // sharp base (vestigial)
        down   = t->accentPrimary;                       // vestigial (override handles press)
        over   = t->accentSecondary.withAlpha (0.30f);   // vestigial (override handles hover)
        line   = t->outline;                             // vestigial (override draws the seam)
        text   = t->textSecondary;                       // vestigial (override uses fill.contrasting())
        shadow = juce::Colour (0x00000000);              // no top shadow gradient
    }
    else
    {
        // Carbon-derived fallback (shown only before the editor's
        // ParvatiLookAndFeel is inherited).
        white  = juce::Colour (0x00000000);              // transparent bg
        black  = juce::Colour (0xff141419);              // windowBackground
        down   = juce::Colour (0xff38BDF8);              // accent (cyan)
        over   = juce::Colour (0xff5b8db8).withAlpha (0.30f);
        line   = juce::Colour (0xff3c3c4a);              // outline
        text   = juce::Colour (0xff9a9aa8);
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

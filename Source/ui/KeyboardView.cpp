// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See KeyboardView.h.

#include "KeyboardView.h"

#include "ParvatiTheme.h"

#include <algorithm>
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
        // keyWhite and the accent reuse Carbon's factory values so no new colour
        // literal lives outside the theme factories.
        return { juce::Colour (0xff3c3c4a), carbonTheme().accentPrimary, juce::Colour (0xff5b8db8),
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

    // The musical-typing row's TOP semitone offset (';' = +16 above
    // octaveBase_; keep in sync with KeyboardView::qwertyMap()). Referenced
    // by comments only now: the row always fits the two-octave window
    // (16 < 24 semitones) because Z/X move the base and the window together.
    [[maybe_unused]] constexpr int kQwertyTopOffset = 16;
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
        // Release this source's previously-held note (if any) before arming —
        // but only when no OTHER source still holds it (a glissando can land
        // two fingers on one key; each engine note must sound exactly once).
        const int src = e.source.getIndex();
        const auto it = owner.mouseDownNotesBySource_.find (src);
        if (it != owner.mouseDownNotesBySource_.end() && it->second != midiNoteNumber
            && ! owner.noteHeldByOtherSource (it->second, src))
            owner.fireNoteCallback (it->second, false, 0.0f);

        owner.mouseDownNotesBySource_[src] = midiNoteNumber;
        owner.fireNoteCallback (midiNoteNumber, true, owner.velocityFromEvent (e));
        // Continuous pressure starts with the strike: the press Y IS the first
        // pressure sample (lower on the key = harder press).
        owner.firePressureFromEvent (e);
        return true;   // base lights the key via the shared state_
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        // Continuous pressure on EVERY drag move (even within the same key):
        // sliding a held finger up/down the key tracks "pressure" live. The
        // base class handles the key-sweep lighting; mouseDraggedToKey below
        // fires the note retargets.
        owner.firePressureFromEvent (e);
        juce::MidiKeyboardComponent::mouseDrag (e);
    }

    bool mouseDraggedToKey (int midiNoteNumber, const juce::MouseEvent& e) override
    {
        // Real glissando: when a drag sweeps onto a DIFFERENT key, retarget this
        // source's note — release the old, sound the new. The base class
        // re-lights each swept key as it goes; keeping the engine on the FIRST
        // note (the old visual-only behaviour) read as a stuck note during every
        // slide. Velocity is recomputed from the drag position via the SAME
        // y-position=pressure rule as a press (lower on the key = louder; a
        // drag carries no fresh strike velocity, and JUCE's own drag handling
        // derives velocity from position the same way). Cross-source dedup
        // mirrors the base class's finger dedup in updateNoteUnderMouse: only
        // release the old note when no OTHER source still holds it, and only
        // sound the new note when no other source is already sounding it (two
        // fingers on one key = one note).
        const int src = e.source.getIndex();
        const auto it = owner.mouseDownNotesBySource_.find (src);
        if (it != owner.mouseDownNotesBySource_.end() && it->second != midiNoteNumber)
        {
            const int oldNote = it->second;
            if (! owner.noteHeldByOtherSource (oldNote, src))
                owner.fireNoteCallback (oldNote, false, 0.0f);
            owner.mouseDownNotesBySource_[src] = midiNoteNumber;
            if (! owner.noteHeldByOtherSource (midiNoteNumber, src))
                owner.fireNoteCallback (midiNoteNumber, true, owner.velocityFromEvent (e));
        }
        return true;   // base lights the swept key via the shared state_
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
    // Only fires the note-off when no OTHER source still holds the same note —
    // a glissando retarget can leave two sources on one key, and the first
    // release must not cut the sound out from under the finger still on it.
    void releaseSourceNote (int src)
    {
        const auto it = owner.mouseDownNotesBySource_.find (src);
        if (it != owner.mouseDownNotesBySource_.end())
        {
            if (! owner.noteHeldByOtherSource (it->second, src))
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

    // TWO OCTAVES only (default C3..C5 — kRangeLow..kRangeHigh): large keys
    // beat range on a touch strip. resized() stretches the window's 15 white
    // keys across the full component width, so there is never anything to
    // pan — no scroll arrows (the old 5-octave range needed them only
    // because the keys were small). The window FOLLOWS the Ableton-style Z/X
    // octave base (applyQwertyWindow); it is always C..C, so the white-key
    // count (15) never changes.
    keyboard_->setAvailableRange (octaveBase_, octaveBase_ + (kRangeHigh - kRangeLow));
    keyboard_->setScrollButtonsVisible (false);

    // Computer-keyboard play: this component owns the focus so it receives the
    // musical-typing keys (the inner KeyComp never takes focus itself).
    setWantsKeyboardFocus (true);
    keyboard_->setWantsKeyboardFocus (false);

    // Accessibility name/description/help (read by the default handler).
    setTitle ("Virtual Keyboard");
    setDescription ("Virtual keyboard");
    setHelpText ("Click a key, or type on the computer keyboard (A W S D F G H J K L ; with "
                 "W E T Y U O P for the sharps) to play notes inside the two visible "
                 "octaves. Z and X shift the octave down and up (the visible window "
                 "follows); C and V decrease and increase the velocity.");

    applyThemeColours();   // fallback colours until added to the themed tree
}

KeyboardView::~KeyboardView() {}

//==============================================================================
void KeyboardView::latchNoteOn (int midiNote, float velocity)
{
    // Notes OUTSIDE the visible window (kRangeLow..kRangeHigh) are accepted by
    // the state but never drawn — MidiKeyboardComponent ignores notes beyond
    // its available range — so engine activity outside the two visible octaves
    // lights no key (and the matching noteOff below clears it cleanly). That is
    // the accepted contract: the mirror covers exactly the visible window.
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

void KeyboardView::setPressureCallback (PressureCallback cb)
{
    pressureCallback_ = std::move (cb);
}

void KeyboardView::setSettingsChangedCallback (SettingsChangedCallback cb)
{
    onSettingsChanged_ = std::move (cb);
    // Report the initial state so the host can prime its display.
    if (onSettingsChanged_)
        onSettingsChanged_ (octaveBase_, qwertyVelocity127());
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
    // Span the full width: size one white key so the two-octave window
    // [octaveBase_, octaveBase_ + span] fills the component. A C..C window
    // holds 15 white keys (C D E F G A B x2 + the closing C), so each white
    // key is ~80pt wide at a 1200pt strip — the LARGE-key brief. Without this
    // the MidiKeyboardComponent left-aligns the keys at its default width and
    // leaves an empty block on the right. Vertically the KeyComp is bounds-fed
    // below and fills the FULL component height natively (no fixed-height
    // override anywhere; a horizontal MidiKeyboardComponent draws its keys
    // edge-to-edge top-to-bottom), so the tall strip stays fully covered.
    int whiteKeys = 0;
    for (int n = octaveBase_; n <= octaveBase_ + (kRangeHigh - kRangeLow); ++n)
    {
        const int d = n % 12;
        if (d == 0 || d == 2 || d == 4 || d == 5 || d == 7 || d == 9 || d == 11)
            ++whiteKeys;
    }
    if (whiteKeys > 0 && getWidth() > 0)
        keyboard_->setKeyWidth (static_cast<float> (getWidth()) / static_cast<float> (whiteKeys));
    keyboard_->setScrollButtonsVisible (false);   // the window fills the width: nothing to scroll
    keyboard_->setBounds (getLocalBounds());
}

void KeyboardView::applyQwertyWindow()
{
    // Called after a Z/X octave shift: move the visible window to the new base
    // (always C..C, so the white-key count and key width are unchanged) and
    // re-layout. setAvailableRange does not repaint the key geography until
    // the component re-lays out, hence the resized() call.
    keyboard_->setAvailableRange (octaveBase_, octaveBase_ + (kRangeHigh - kRangeLow));
    resized();
}

void KeyboardView::shiftOctave (int semitones)
{
    // GUI octave switch (the wheels panel's [<][>] buttons): the same move as
    // the Z/X keys, expressed as a semitone delta (callers pass whole
    // octaves). Clamped to the MIDI range, window follows, settings reported.
    octaveBase_ = juce::jlimit (0, 127 - (kRangeHigh - kRangeLow), octaveBase_ + semitones);
    applyQwertyWindow();
    if (onSettingsChanged_)
        onSettingsChanged_ (octaveBase_, qwertyVelocity127());
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

void KeyboardView::firePressureFromEvent (const juce::MouseEvent& e)
{
    // Same y-position=pressure mapping as velocityFromEvent (lower on the key
    // = harder). Called on note-on and on every drag move so a held finger's
    // up/down motion tracks aftertouch live.
    if (pressureCallback_)
        pressureCallback_ (velocityFromEvent (e));
}

float KeyboardView::velocityFromEvent (const juce::MouseEvent& e) const
{
    // Y-position "pressure": the LOWER the touch on the key, the LOUDER the
    // note (like pressing deeper into a key / MPE pressure). Full 0..1 range
    // across the strip height — the top of a key plays pianissimo, the bottom
    // fortissimo. Used on press AND on drag-retarget, so a glissando that
    // slides down the keys crescendos.
    const int h = juce::jmax (1, getHeight());
    const float norm = juce::jlimit (0.0f, 1.0f, e.position.y / static_cast<float> (h));
    return 1.0f - norm;
}

bool KeyboardView::noteHeldByOtherSource (int midiNote, int exceptSource) const
{
    return std::any_of (mouseDownNotesBySource_.begin(), mouseDownNotesBySource_.end(),
                        [midiNote, exceptSource] (const auto& kv)
                        { return kv.first != exceptSource && kv.second == midiNote; });
}

//==========================================================================
// Computer-keyboard (musical-typing) playback.
const std::map<char, int>& KeyboardView::qwertyMap()
{
    // Standard musical-typing layout, value = semitone offset from octaveBase_.
    // White keys: A S D F G H J K L  (C D E F G A B C D)
    // Black keys: W E   T Y U   O P  (C# D#  F# G# A#  D#... in next octave)
    // Spans 17 semitones (C..E of the next octave) — every offset lands inside
    // the visible two-octave window because Z/X move the base and the window
    // together (see kQwertyTopOffset in the anonymous namespace above).
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

void KeyboardView::releaseAllNotes()
{
    // Touch/mouse sources: every held finger/click gets its note-off (the
    // editor's destructor is the only caller today — teardown, so the latch
    // mirror clearing is harmless even if the editor's timer is gone).
    for (const auto& kv : mouseDownNotesBySource_)
    {
        fireNoteCallback (kv.second, false, 0.0f);
        latchNoteOff (kv.second);
    }
    mouseDownNotesBySource_.clear();
    releaseHeldComputerNotes();
}

bool KeyboardView::keyPressed (const juce::KeyPress& key)
{
    if (! computerKeyboardEnabled_)
        return false;

    // Never hijack modifier combos (Cmd/Ctrl/Alt) so the editor's Cmd +/-/0
    // zoom, Undo/Cut/Copy/Paste and host shortcuts still work while the
    // keyboard has focus (the octave/velocity controls are BARE keys below —
    // Cmd+Z/X/C/V stay the app's Undo/Cut/Copy/Paste).
    const auto& mods = key.getModifiers();
    if (mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown())
        return false;

    const auto wc = key.getTextCharacter();
    const char c = static_cast<char> (juce::CharacterFunctions::toLowerCase (wc));

    // Ableton-style controls. Z/X shift the octave: the base moves by whole
    // octaves across the FULL MIDI range and the visible two-octave window
    // FOLLOWS (applyQwertyWindow) — the QWERTY row stays anchored at the
    // window's bottom C. C/V adjust the musical-typing velocity in steps of
    // 20/127 (Ableton Live's computer-keyboard scheme; default 100/127).
    // Every change reports through the settings channel (the editor surfaces
    // it in the status/tooltip bar).
    if (c == 'z')
    {
        octaveBase_ = juce::jmax (0, octaveBase_ - 12);
        applyQwertyWindow();
        if (onSettingsChanged_) onSettingsChanged_ (octaveBase_, qwertyVelocity127());
        return true;
    }
    if (c == 'x')
    {
        octaveBase_ = juce::jmin (127 - (kRangeHigh - kRangeLow), octaveBase_ + 12);
        applyQwertyWindow();
        if (onSettingsChanged_) onSettingsChanged_ (octaveBase_, qwertyVelocity127());
        return true;
    }
    if (c == 'c')
    {
        qwertyVelocity_ = juce::jmax (1.0f / 127.0f, qwertyVelocity_ - 20.0f / 127.0f);
        if (onSettingsChanged_) onSettingsChanged_ (octaveBase_, qwertyVelocity127());
        return true;
    }
    if (c == 'v')
    {
        qwertyVelocity_ = juce::jmin (1.0f, qwertyVelocity_ + 20.0f / 127.0f);
        if (onSettingsChanged_) onSettingsChanged_ (octaveBase_, qwertyVelocity127());
        return true;
    }

    const auto& m = qwertyMap();
    const auto it = m.find (c);
    if (it == m.end())
        return false;   // not a musical key -> let the parent handle it

    // Ignore auto-repeat of an already-held key (no re-trigger).
    if (heldNotes_.find (c) != heldNotes_.end())
        return true;

    // In-window by construction (the row spans 16 semitones above the base,
    // the window 24 — and Z/X move both together); the jlimit documents +
    // enforces the contract as a safety net.
    const int note = juce::jlimit (octaveBase_, octaveBase_ + (kRangeHigh - kRangeLow),
                                   octaveBase_ + it->second);
    heldNotes_[c] = note;
    fireNoteCallback (note, true, qwertyVelocity_);
    latchNoteOn (note, qwertyVelocity_);
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

// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See WheelsComponent.h.

#include "WheelsComponent.h"

#include "ParvatiLookAndFeel.h"
#include "ParvatiTheme.h"
#include "dsp/patch.h"            // ambika::dsp::MOD_SRC_PITCH_BEND / MOD_SRC_WHEEL
#include "PluginEditor.h"   // ParamControl (tap-to-assign state)

//==============================================================================
// A vertical slider that snaps back to its midpoint (0.0) on mouse release.
struct WheelsComponent::SpringSlider : public juce::Slider
{
    SpringSlider()
        : juce::Slider (juce::Slider::LinearVertical, juce::Slider::NoTextBox)
    {
        setRange (-1.0, 1.0, 0.001);
        setValue (0.0, juce::dontSendNotification);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        // Spring back to centre: setValue fires onValueChange -> onPitch(0).
        setValue (0.0, juce::sendNotificationSync);
        juce::Slider::mouseUp (e);
    }
};

//==============================================================================
// A caption label ("PITCH" / "MOD") that is ALSO a modulation-source DRAG
// SOURCE. A drag past ~5px starts an internal DragAndDropContainer drag
// carrying "parvatiModSrc:<enum>", which any destination knob accepts (same
// payload/paths as the generator tab buttons and the matrix-row grip). The
// wheels above stay fully playable; only this caption strip initiates the
// assignment drag.
struct WheelDragLabel : public juce::Component, public juce::SettableTooltipClient
{
    WheelDragLabel (juce::String label, int sourceEnum)
        : label_ (std::move (label)), src_ (sourceEnum)
    {
        setInterceptsMouseClicks (true, false);
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        // Same suffix-key pattern as the matrix-row grip's tooltip: the label
        // ("PITCH"/"MOD", a proper noun) stays untranslated, the sentence
        // fragments around it are TRANS'd (FR/DE keys in Translations.cpp).
        setTooltip (TRANS ("Drag onto a knob to assign ") + label_
                    + TRANS (" as a modulation source"));
    }

    void paint (juce::Graphics& g) override
    {
        auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel());
        const ParvatiTheme* t = (lnf != nullptr) ? lnf->getTheme() : nullptr;
        // Match the surrounding label text (e.g. the keyboard's key labels use
        // the bright `text` token) rather than the dim caption token.
        g.setColour (t != nullptr ? t->textPrimary : juce::Colour (0xffe0e0e0));
        // Same app typeface as all other UI text (appFont uses the system
        // default sans-serif). The explicit fallback guarantees the FACE even
        // if this child's LookAndFeel is not yet resolved, so the caption never
        // silently renders in a different font than the surrounding labels.
        g.setFont (lnf != nullptr ? lnf->appFont (9.0f, juce::Font::plain)
                                  : juce::Font (juce::FontOptions (juce::Font::getDefaultSansSerifFontName(),
                                                                   9.0f, juce::Font::plain)));
        g.drawText (label_, getLocalBounds(), juce::Justification::centredTop);
    }

    void mouseDown (const juce::MouseEvent&) override { dragStarted_ = false; }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragStarted_ || src_ < 0 || e.getDistanceFromDragStart() <= 5)
            return;
        auto* ddc = findParentComponentOfClass<juce::DragAndDropContainer>();
        if (ddc == nullptr)
            return;   // no DragAndDropContainer ancestor (e.g. a headless test)
        dragStarted_ = true;
        ddc->startDragging ("parvatiModSrc:" + juce::String (src_), this, juce::ScaledImage (buildDragImage()), true);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        dragStarted_ = false;
        // Tap-to-assign: a clean tap (no drag) selects this wheel's mod source
        // (Pitch Bend / Mod Wheel) for the next dest tap. Inert unless [MOD] on.
        if (ParamControl::tapAssignActive() && src_ >= 0 && e.getDistanceFromDragStart() <= 5)
            ParamControl::setTapSelectedSource (src_);
    }

    // A small themed chip (mirrors the CentralModBar mod-pill drag image).
    juce::Image buildDragImage() const
    {
        auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel());
        const ParvatiTheme* t = (lnf != nullptr) ? lnf->getTheme() : nullptr;
        const juce::Colour fill = (t != nullptr) ? t->containerFill : juce::Colour (0xff202028);
        const juce::Colour txt  = (t != nullptr) ? t->textPrimary          : juce::Colour (0xffe0e0e0);
        const juce::Colour acc  = (t != nullptr) ? t->accentPrimary        : parvati::parvatiFallbackAccent;
        const juce::Font f = (lnf != nullptr) ? lnf->appFont (13.0f, juce::Font::plain)
                                              : juce::Font (juce::FontOptions (13.0f));
        const int textW = juce::GlyphArrangement::getStringWidthInt (f, label_);
        const int w = juce::jmax (48, 12 + 8 + textW + 10);
        const int h = 22;
        juce::Image img (juce::Image::ARGB, w, h, true);
        juce::Graphics g (img);
        g.setColour (fill);
        g.fillRoundedRectangle (img.getBounds().toFloat(), 5.0f);
        // Performance sources (Pitch Bend / Wheel) carry no Env/LFO/Seq/Arp
        // category, so the chip's accent bar uses the theme accent.
        g.setColour (acc);
        g.fillRoundedRectangle (juce::Rectangle<float> (5.0f, 5.0f, 7.0f, static_cast<float> (h) - 10.0f), 2.0f);
        g.setColour (txt);
        g.setFont (f);
        g.drawText (label_, juce::Rectangle<int> (17, 0, w - 17, h), juce::Justification::centredLeft, true);
        g.setColour (acc.withAlpha (0.6f));
        g.drawRoundedRectangle (img.getBounds().toFloat().reduced (0.5f), 5.0f, 1.0f);
        return img;
    }

    juce::String label_;
    int src_;
    bool dragStarted_ = false;
};

//==============================================================================
WheelsComponent::WheelsComponent()
{
    pitch_ = std::make_unique<SpringSlider>();
    // Wheel-scroll must NEVER tweak the wheels (the ParamControl idiom): a
    // scroll over the strip is page scrolling, not pitch/mod editing. An
    // unhandled wheel bubbles to the enclosing Viewport.
    pitch_->setScrollWheelEnabled (false);
    // Wheels never take keyboard focus (2026-08-21, same musical-typing rule
    // as ParamControl's knobs): dragging the pitch/mod wheel mid-performance
    // must not stop the QWERTY keys playing the KeyboardView.
    pitch_->setWantsKeyboardFocus (false);
    // Accessibility-only: both wheels are juce::Sliders, so JUCE's built-in
    // Slider accessibility handler (ranged numeric value, -1..1 pitch /
    // 0..1 mod, adjustable) is already attached — only the NAME was missing
    // (a NoTextBox slider exposes no visible text). setTitle() is read by the
    // default handler; TRANS keys added to Translations.cpp (FR/DE).
    pitch_->setTitle (TRANS ("Pitch Wheel"));
    pitch_->setDescription (TRANS ("Pitch Wheel"));
    pitch_->onValueChange = [this] { if (onPitch) onPitch (static_cast<float> (pitch_->getValue())); };
    addAndMakeVisible (*pitch_);

    mod_ = std::make_unique<juce::Slider> (juce::Slider::LinearVertical, juce::Slider::NoTextBox);
    mod_->setRange (0.0, 1.0, 0.001);
    mod_->setScrollWheelEnabled (false);   // wheel-scroll is page scroll, not mod editing
    mod_->setWantsKeyboardFocus (false);   // musical-typing rule (see pitch_ above)
    mod_->setTitle (TRANS ("Mod Wheel"));
    mod_->setDescription (TRANS ("Mod Wheel"));
    mod_->setValue (0.0, juce::dontSendNotification);
    mod_->onValueChange = [this] { if (onMod) onMod (static_cast<float> (mod_->getValue())); };
    addAndMakeVisible (*mod_);

    // Caption labels double as modulation-source drag sources (PITCH -> Pitch
    // Bend, MOD -> Mod Wheel). Dropped on a destination knob they consume the
    // next free slot, exactly like dragging a generator tab.
    pitchDrag_ = std::make_unique<WheelDragLabel> ("PITCH", ambika::dsp::MOD_SRC_PITCH_BEND);
    modDrag_   = std::make_unique<WheelDragLabel> ("MOD",   ambika::dsp::MOD_SRC_WHEEL);
    addAndMakeVisible (*pitchDrag_);
    addAndMakeVisible (*modDrag_);

    // [<][>] octave switch under the pitch wheel: mirrors the Z/X
    // musical-typing keys. Emits onOctaveShift(+/-1) (unit octave steps); the
    // editor multiplies by 12 and routes into KeyboardView::shiftOctave.
    // Plain themed TextButtons (ParvatiLookAndFeel styles them like the
    // header icon buttons — no custom painting) at the kOctBtnSize 44pt HIG
    // touch target.
    octaveDown_ = std::make_unique<juce::TextButton> ("<");
    octaveUp_   = std::make_unique<juce::TextButton> (">");
    octaveDown_->setWantsKeyboardFocus (false);   // musical-typing rule (see pitch_)
    octaveUp_->setWantsKeyboardFocus (false);
    octaveDown_->setTooltip (TRANS ("Octave down (Z)"));
    octaveUp_->setTooltip (TRANS ("Octave up (X)"));
    octaveDown_->onClick = [this] { if (onOctaveShift) onOctaveShift (-1); };
    octaveUp_->onClick   = [this] { if (onOctaveShift) onOctaveShift (+1); };
    addAndMakeVisible (*octaveDown_);
    addAndMakeVisible (*octaveUp_);
}

WheelsComponent::~WheelsComponent() = default;

void WheelsComponent::paint (juce::Graphics& g)
{
    // Read the active theme through the inherited ParvatiLookAndFeel (same
    // pattern as KeyboardView). Re-applied each paint so a live theme switch
    // recolours the wheels, and the label font follows the font mode.
    auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel());
    const ParvatiTheme* t = (lnf != nullptr) ? lnf->getTheme() : nullptr;

    const juce::Colour bg    = (t != nullptr) ? t->backgroundBase : juce::Colour (0xff141419);
    const juce::Colour track = (t != nullptr) ? t->outline          : juce::Colour (0xff3a3a44);
    const juce::Colour thumb = (t != nullptr) ? t->accentPrimary           : parvati::parvatiFallbackAccent;

    g.fillAll (bg);

    for (auto* s : { static_cast<juce::Slider*> (pitch_.get()), mod_.get() })
    {
        s->setColour (juce::Slider::backgroundColourId, bg);
        s->setColour (juce::Slider::trackColourId, track);
        s->setColour (juce::Slider::thumbColourId, thumb);
    }

    // The "PITCH" / "MOD" captions are drawn by the pitchDrag_ / modDrag_ label
    // components (which double as modulation-source drag sources), not here.
}

void WheelsComponent::resized()
{
    auto b = getLocalBounds();
    auto labelStrip = b.removeFromBottom (14);   // the PITCH/MOD caption drag strip

    // BOTH columns are split vertically: each wheel keeps the upper region,
    // and a kOctBtnSize (44pt HIG) button row sits at its BOTTOM (above the
    // caption strip) — [<] under the PITCH label, [>] under the MOD label
    // (one per column).
    const int half = b.getWidth() / 2;
    auto leftCol    = b.removeFromLeft (half);
    auto leftOctRow = leftCol.removeFromBottom (kOctBtnSize);
    auto rightOctRow = b.removeFromBottom (kOctBtnSize);
    pitch_->setBounds (leftCol.reduced (5, 2));
    mod_->setBounds (b.reduced (5, 2));

    // [<] in the left row, [>] in the right row: a 44x44 HIG touch target,
    // centred in its column (the 50pt half of the 100pt wheels strip leaves
    // a 3pt margin; a narrower column shrinks the button instead of
    // overflowing — the jmin guard is defensive only).
    const int btnW  = juce::jmin (kOctBtnSize, juce::jmax (6, leftOctRow.getWidth()));
    const int btnWR = juce::jmin (kOctBtnSize, juce::jmax (6, rightOctRow.getWidth()));
    octaveDown_->setBounds (leftOctRow.withSizeKeepingCentre (btnW, kOctBtnSize));
    octaveUp_->setBounds (rightOctRow.withSizeKeepingCentre (btnWR, kOctBtnSize));

    const int halfL = labelStrip.getWidth() / 2;
    pitchDrag_->setBounds (labelStrip.removeFromLeft (halfL));
    modDrag_->setBounds (labelStrip);
}

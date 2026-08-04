// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See WheelsComponent.h.

#include "WheelsComponent.h"

#include "ParvatiLookAndFeel.h"
#include "ParvatiTheme.h"

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
WheelsComponent::WheelsComponent()
{
    pitch_ = std::make_unique<SpringSlider>();
    pitch_->onValueChange = [this] { if (onPitch) onPitch (static_cast<float> (pitch_->getValue())); };
    addAndMakeVisible (*pitch_);

    mod_ = std::make_unique<juce::Slider> (juce::Slider::LinearVertical, juce::Slider::NoTextBox);
    mod_->setRange (0.0, 1.0, 0.001);
    mod_->setValue (0.0, juce::dontSendNotification);
    mod_->onValueChange = [this] { if (onMod) onMod (static_cast<float> (mod_->getValue())); };
    addAndMakeVisible (*mod_);
}

WheelsComponent::~WheelsComponent() = default;

void WheelsComponent::paint (juce::Graphics& g)
{
    // Read the active theme through the inherited ParvatiLookAndFeel (same
    // pattern as KeyboardView). Re-applied each paint so a live theme switch
    // recolours the wheels.
    const ParvatiTheme* t = nullptr;
    if (auto* lnf = dynamic_cast<ParvatiLookAndFeel*> (&getLookAndFeel()))
        t = lnf->getTheme();

    const juce::Colour bg    = (t != nullptr) ? t->windowBackground : juce::Colour (0xff141419);
    const juce::Colour track = (t != nullptr) ? t->outline          : juce::Colour (0xff3a3a44);
    const juce::Colour thumb = (t != nullptr) ? t->accent           : juce::Colour (0xffc8a44a);
    const juce::Colour dim   = (t != nullptr) ? t->textDim          : juce::Colour (0xff8a8a96);

    g.fillAll (bg);

    for (auto* s : { static_cast<juce::Slider*> (pitch_.get()), mod_.get() })
    {
        s->setColour (juce::Slider::backgroundColourId, bg);
        s->setColour (juce::Slider::trackColourId, track);
        s->setColour (juce::Slider::thumbColourId, thumb);
    }

    g.setColour (dim);
    g.setFont (juce::FontOptions (9.0f));
    const int halfW = getWidth() / 2;
    g.drawText ("PITCH", juce::Rectangle<int> (0, getHeight() - 13, halfW, 12),
                juce::Justification::centredTop);
    g.drawText ("MOD", juce::Rectangle<int> (halfW, getHeight() - 13, getWidth() - halfW, 12),
                juce::Justification::centredTop);
}

void WheelsComponent::resized()
{
    auto b = getLocalBounds();
    b.removeFromBottom (14);   // reserve the label strip drawn in paint()

    const int half = b.getWidth() / 2;
    pitch_->setBounds (b.removeFromLeft (half).reduced (5, 2));
    mod_->setBounds (b.reduced (5, 2));
}

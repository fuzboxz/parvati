// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// WheelsComponent — Pitch + Mod wheels drawn to the LEFT of the virtual
// keyboard. Two vertical sliders: the pitch wheel is bipolar and springs back
// to centre on release (a real pitch-bend wheel), the mod wheel is unipolar and
// stays where dropped. Themed via the inherited ParvatiLookAndFeel (read in
// paint()). The editor wires onPitch / onMod to the engine as MIDI pitch-bend
// / CC1 (mod wheel) on the current Part's channel.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

//==============================================================================
class WheelsComponent : public juce::Component
{
public:
    /** Pitch-bend value, -1.0 .. +1.0 (0 == centre). Fired on drag and on the
        spring-back-to-centre release. */
    std::function<void (float)> onPitch;
    /** Mod-wheel value, 0.0 .. 1.0. Fired on drag. */
    std::function<void (float)> onMod;

    WheelsComponent();
    ~WheelsComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    // Vertical slider that snaps to its midpoint (0) when the mouse is released,
    // so the pitch wheel always returns to no-bend.
    struct SpringSlider;
    friend struct SpringSlider;

    std::unique_ptr<SpringSlider>   pitch_;
    std::unique_ptr<juce::Slider>   mod_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WheelsComponent)
};

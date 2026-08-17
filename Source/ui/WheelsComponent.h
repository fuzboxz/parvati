// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// WheelsComponent — Pitch + Mod wheels drawn to the LEFT of the virtual
// keyboard. Two vertical sliders: the pitch wheel is bipolar and springs back
// to centre on release (a real pitch-bend wheel), the mod wheel is unipolar and
// stays where dropped. Themed via the inherited ParvatiLookAndFeel (read in
// paint()). The editor wires onPitch / onMod to the engine as MIDI pitch-bend
// / CC1 (mod wheel) on the current Part's channel.
//
// Under the PITCH wheel (left column, above the caption strip) sits a compact
// [<][>] octave-switch row mirroring the Z/X musical-typing keys: it fires
// onOctaveShift(+/-1) (octave steps), which the editor routes into
// KeyboardView::shiftOctave (clamped at the MIDI edges; the visible piano
// window follows and the settings-changed tooltip readout fires).

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
    /** Octave-switch request from the [<][>] buttons: @p steps is +/-1 octave
        (the buttons are unit steps; the editor multiplies by 12 semitones for
        KeyboardView::shiftOctave). Fired on click. */
    std::function<void (int)> onOctaveShift;

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

    // The "PITCH" / "MOD" caption labels double as DRAG SOURCES for modulation
    // assignment: dragging the caption starts a `parvatiModSrc:<enum>` drag
    // (PITCH -> MOD_SRC_PITCH_BEND, MOD -> MOD_SRC_WHEEL) so the source can be
    // dropped onto any destination knob (same payload as the generator tabs /
    // matrix-row grip). The wheels themselves stay playable; only the caption
    // strip initiates the assignment drag. Concrete type is file-local in the .cpp.
    std::unique_ptr<juce::Component> pitchDrag_;
    std::unique_ptr<juce::Component> modDrag_;

    // [<][>] octave switch under the pitch wheel (see onOctaveShift). Plain
    // themed TextButtons — no custom painting (ParvatiLookAndFeel styles them
    // like every other button in the chrome).
    std::unique_ptr<juce::TextButton> octaveDown_;
    std::unique_ptr<juce::TextButton> octaveUp_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WheelsComponent)
};

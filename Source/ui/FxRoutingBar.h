// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.
//
// FxRoutingBar — the slim ROUTING column (column 0 of the FX page's 4-column
// top row: [ ROUTING | FX1 | FX2 | FX3 ]). It holds the chain-level routing
// controls only (the master-EQ section was removed):
//
//   +-------------------------------+
//   | ROUTING                       |   bold 14px header (sibling-card style)
//   | FLOW: [ Series ...        |v] |   juce::ComboBox bound to `fx_topo`
//   | MIX:        ( dial )          |   rotary knob bound to `fx_mix`
//   | [ HPF ][ Mid ][ High ]       |   3-band master EQ bound to fx_eq_*
//   +-------------------------------+
//
// Rendered as a borderless sibling card (containerFill, 7px corners, no outline)
// matching the FX-slot cards + the synth GroupComponent cards. The editor owns
// the bar and hosts it non-owned as the LEFTMOST column of FxWorkspace's top row
// (it gets the full top-row height, like the cards). `fx_order` stays in the
// engine/serialization but is not user-exposed (default process order). The
// DragAndDropContainer base is retained (harmless). Self-contained: reads theme
// via ThemeManager + binds APVTS state directly (no editor coupling).

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>   // APVTS attachment types

#include <memory>
#include <array>

#include "ThemeManager.h"     // also brings HellcatTheme.h

class HellcatAudioProcessor;
class FxFlowDiagram;

//==============================================================================
class FxRoutingBar : public juce::Component,
                     public juce::DragAndDropContainer
{
public:
    // HIG touch-target constants (pinned by tests/ipad_hig_sizing_test.cpp):
    // the ◀ ▶ topology steppers are full 44x44 targets inside the 50pt flow
    // row (FlexBox centres them vertically, 3pt breathing room top/bottom),
    // and the EQ rotary dials draw at the 44pt minimum.
    static constexpr int kStepBtnW   = 44;   // ◀ ▶ topology stepper width
    static constexpr int kStepBtnH   = 44;   // ◀ ▶ topology stepper height
    static constexpr int kEqKnobSize = 44;   // EQ rotary dial (was 42; HIG minimum)

    FxRoutingBar (HellcatAudioProcessor& processor, ThemeManager& themeManager);
    ~FxRoutingBar() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Re-resolve theme colours onto the diagram / Dry/Wet knob + repaint (theme
        switch). */
    void applyThemeColors();

private:
    HellcatAudioProcessor& processor_;
    ThemeManager&          themeManager_;

    // ---- Routing: flow diagram + ◀ ▶ steppers + Dry/Wet knob ----
    juce::TextButton prevButton_, nextButton_;                 // ◀ ▶ topology steppers (cycle fx_topo)
    std::unique_ptr<FxFlowDiagram> flowDiagram_;               // in->out signal-flow block chart (fx_topo)

    juce::Label  mixLabel_;         // "Dry/Wet" caption above the Dry/Wet knob
    juce::Slider mixKnob_;          // bound to fx_mix (synth-style rotary, value drawn in-ring)
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> mixAttach_;

    // ---- 3-band master EQ (HPF / Mid / High) — synth-style rotary knobs ----
    std::array<juce::Slider, 3>           eqKnobs_;
    std::array<juce::Label, 3>            eqLabels_;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>, 3> eqAttach_;

    void stepTopology (int direction);     // ◀ ▶ : cycle fx_topo directly

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxRoutingBar)
};

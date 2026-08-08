// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// FxRoutingBar — the full-width header strip at the top of the FX page's upper
// region. It holds the chain-level ROUTING control AND the MASTER section:
//
//     | ROUTING & MASTER EQ                                                         |
//     | FLOW: [ FX1 -> FX2 -> FX3 |v]            |        /--\            ------\  |
//     | MIX:  ( 50 ) Global Wet/Dry             |       /    \          /       \ |
//     |                                          |  ---/      \----------/         \ |
//     | [x] Keep FX Tails on Bypass             |     Low Cut      Mid      High Shelf |
//
//   LEFT column (~45%):
//     * A bold "ROUTING & MASTER EQ" title.
//     * "FLOW:" + a juce::ComboBox bound to `fx_topo` (ComboBoxAttachment); the
//       combo's choice list is the parameter's OWN list (flow strings), so this
//       single dropdown IS the routing control.
//     * "MIX:" + a rotary knob bound to `fx_mix` (SliderAttachment) + a "Global
//       Wet/Dry" caption. (127 = fully wet = the pre-master default.)
//     * A "Keep FX Tails on Bypass" juce::ToggleButton bound to `fx_keep_tails`
//       (an Int 0/1, two-way via a juce::Value + Value::Listener — NOT a
//       ButtonAttachment, mirroring FxSlotCard's fx{N}_enabled binding).
//   RIGHT column (~55%):
//     * An FxMasterEqCurve — the composite master-EQ response (low-cut / mid /
//       high-shelf), drawn to MATCH FxChain's RBJ biquads exactly.
//
// `fx_order` stays in the engine/serialization but is not user-exposed here
// (default process order). The bar remains a DragAndDropContainer (harmless).
//
// Self-contained: reads theme via ThemeManager and binds APVTS state directly
// (no editor coupling). The editor owns the bar and hosts it non-owned inside
// FxWorkspace (mirrors FxMatrixView).

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>   // APVTS attachment types

#include <memory>

#include "ThemeManager.h"     // also brings ParvatiTheme.h

class ParvatiAudioProcessor;
class FxMasterEqCurve;

//==============================================================================
class FxRoutingBar : public juce::Component,
                     public juce::DragAndDropContainer,
                     private juce::Value::Listener
{
public:
    FxRoutingBar (ParvatiAudioProcessor& processor, ThemeManager& themeManager);
    ~FxRoutingBar() override;

    /** Fixed bar height the host reserves for this strip (fits the left routing
        + mix + tails stack AND the right EQ curve). */
    static constexpr int kBarHeight = 108;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Re-resolve theme colours onto the title/labels/combo/knob/toggle + the
        EQ curve + repaint (theme switch). */
    void applyThemeColors();

private:
    ParvatiAudioProcessor& processor_;
    ThemeManager&          themeManager_;

    // ---- LEFT: routing + mix + tails ----
    juce::Label titleLabel_;        // "ROUTING & MASTER EQ"
    juce::Label flowLabel_;         // "FLOW:"
    juce::ComboBox topoCombo_;      // bound to fx_topo
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> topoAttach_;

    juce::Label  mixLabel_;         // "MIX:"
    juce::Slider mixKnob_;          // bound to fx_mix
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> mixAttach_;
    juce::Label  mixCaption_;       // "Global Wet/Dry"

    juce::ToggleButton keepTailsToggle_;   // bound to fx_keep_tails via keepTailsValue_
    juce::Value keepTailsValue_;
    void syncKeepTails();                  // push param -> toggle state

    // ---- RIGHT: master EQ curve ----
    std::unique_ptr<FxMasterEqCurve> eqCurve_;

    // juce::Value::Listener: keep the toggle state in sync with the param value
    // (preset load / host automation / programmatic set).
    void valueChanged (juce::Value&) override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxRoutingBar)
};

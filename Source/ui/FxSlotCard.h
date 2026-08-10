// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// FxSlotCard — one Serum/Pigments-style modular FX-slot card for the FX page's
// upper region (FX1 / FX2 / FX3). It is a SELF-CONTAINED juce::Component that
// owns the per-slot effect's entire control surface, replacing the prior
// generic ParamPage knob grid with a structured modular layout:
//
//   HEADER  (~16px):  FX N (upper-left, bold uppercase 14px — synth GroupComponent
//       header parity) + a compact power/bypass toggle in the TOP-RIGHT corner.
//       The Enable/Bypass control is a POWER toggle button (the IEC glyph), NOT a
//       rotary knob: it reads accentSecondary (orange) when the slot is enabled
//       and dimmed when bypassed.
//   ROW 1  (~28px):  the fx{N}_type juce::ComboBox (the algorithm selector,
//       auto-populated with the effect list) sized as a STYLED combo — 28px
//       tall, fit-to-text width, centred — matching the Osc "Shape" / Filter
//       "Mode" selectors (it inherits the same editor-wide ComboBox theme
//       colours via the LookAndFeel). The card panel is BORDERLESS
//       (containerFill, 7px corners — a sibling of the synth GroupComponent cards).
//   VISUALIZER (compact band): an FxSlotVisualizer canvas (the same visual
//       family as the OSC waveform box and the Filter curve box). Dimmed grid +
//       outline when the type is None; a live per-algorithm graphic otherwise.
//       Capped at kVisMax (synth kDecorationH parity — the OSC/Filter preview
//       height); floored at kVisMin. Sits above the knob grid.
//   Bypass: a disabled slot recesses its knobs/visualizer/type-combo to a
//       reduced alpha so it reads as inactive; the panel + title + power glyph
//       stay full-alpha (legible state + identity).
//   PARAM GRID (bottom): a Mixer-style knob GRID (kCellH = the synth cell
//       height) — 3 columns for Delay/Reverb, 2 for Chorus/Gain-Pan so every
//       multi-knob type forms ~2 rows. The ACTIVE fx{N}_param1..4 knobs fill row-major; the
//       fx{N}_drywet knob is ALWAYS the LAST cell (labelled "Dry/Wet") — bottom-right
//       for Reverb / Delay. The count varies by type (None=1 / GainPan,Chorus=3
//       / Delay=4 / Reverb=5), so the grid is 1 or 2 rows. Inactive params are
//       hidden. The grid block centres vertically in its region; cells render the
//       full 52px dial (synth parity).
//
// Dynamic parameter labels: on a type change the active param knobs are
// relabelled to the active DSP algorithm's semantic names (Time / Feedback /
// Spread / Size / Damp / Rate / ...) via ParamControl::setDisplayLabel(). That
// method is added to ParamControl by the editor during integration; this card
// only CALLS it (the file does not compile standalone until that lands).
//
// The five knobs are full ParamControl instances (created here from the
// descriptor table + owned here) so they keep EVERY modulation behaviour the
// synth knobs have: FX-mod-matrix drag-and-drop assignment, per-source concentric
// mod rings, tooltips, and the category arc. The toggle + combo are bound to the
// APVTS (a Value for the 0..1 Int enable param, a ComboBoxAttachment for the
// type choice). The visualizer is fed by normalized APVTS getters built here.
//
// Colours are read from the active ParvatiTheme via the component's LookAndFeel
// every repaint; applyThemeColors() re-tints the visualizer (accentSecondary)
// and repaints. The owned ParamControl knobs are re-themed by the editor's
// global ParamControl::reapplyCategoryColours() pass on a theme switch.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>   // APVTS attachment
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

class ParvatiAudioProcessor;
class ParamControl;
class FxSlotVisualizer;
struct PatchParamDescriptor;

//==============================================================================
class FxSlotCard : public juce::Component,
                   private juce::AudioProcessorValueTreeState::Listener,
                   private juce::AsyncUpdater
{
public:
    /** Construct one FX-slot card.
        @param processor  the audio processor (APVTS access).
        @param themeManager  the editor theme manager.
        @param slot  0..2 (-> fx1_ / fx2_ / fx3_).
        @param p1Desc..p4Desc  descriptors for fx{N}_param1..4 (the card creates +
               owns the ParamControls; they are full modulation-destination knobs).
        @param drywetDesc  descriptor for fx{N}_drywet (the rightmost "Dry/Wet" knob). */
    FxSlotCard (ParvatiAudioProcessor& processor, int slot,
                const PatchParamDescriptor* p1Desc, const PatchParamDescriptor* p2Desc,
                const PatchParamDescriptor* p3Desc, const PatchParamDescriptor* p4Desc,
                const PatchParamDescriptor* drywetDesc);

    ~FxSlotCard() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Re-resolve theme colours onto the visualizer + repaint (theme switch).
        The owned ParamControl knobs are re-themed by the editor's global
        ParamControl::reapplyCategoryColours() pass. */
    void applyThemeColors();

private:
    // APVTS::Listener — fires on ANY fx{N}_type / fx{N}_enabled change (combo
    // edit, host automation, preset load). A Value::Listener on a separate
    // getParameterAsValue instance does NOT receive these, so the knob visible
    // set + power state would otherwise freeze at construction. On the message
    // thread the refresh is applied immediately (so a synchronous render / preset
    // load reflects it at once); off-thread it is deferred via AsyncUpdater.
    void parameterChanged (const juce::String& parameterID, float newValue) override;
    void handleAsyncUpdate() override;

    // Read the current type choice index (0..4) from the bound type Value,
    // clamped to the FxType range.
    int currentTypeIndex() const;

    // Re-apply the param-knob visible set + semantic labels for the current
    // type, then reflow the grid. Idempotent.
    void refreshFromType();

    // Sync the power-toggle button's on/off state to the bound enable Value.
    void refreshEnabled();

    // Lay the active param knobs + the dry/wet (Mix) into a Mixer-style
    // 3-column GRID (row-major): active params first, Mix as the last cell.
    // Inactive params are hidden. The knob block centres vertically in @p gridArea.
    void layoutParamGrid (const juce::Rectangle<int>& gridArea);

    ParvatiAudioProcessor& processor_;
    int slot_;
    const juce::String prefix_;   // "fx{N}_"

    // ---- Owned controls --------------------------------------------------
    // Five full ParamControl knobs (keep FX-mod-matrix drag-drop + mod rings).
    std::unique_ptr<ParamControl> p1_, p2_, p3_, p4_, drywet_;

    std::unique_ptr<juce::ComboBox> typeCombo_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> typeAttach_;

    std::unique_ptr<juce::Button> powerToggle_;   // PowerToggle (defined in the .cpp)

    std::unique_ptr<FxSlotVisualizer> visualizer_;

    // ---- APVTS bindings (type + enable) ---------------------------------
    // The fx{N}_type value is read LIVE via getParameter() in currentTypeIndex()
    // (NO cached Value: a cached Value would miss external changes such as a
    // preset load / host automation). enabledValue_ is the WRITE path for the
    // 0..1 enable Int (the power toggle's onClick assigns it).
    juce::Value enabledValue_;   // fx{N}_enable (write path for the power toggle)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxSlotCard)
};

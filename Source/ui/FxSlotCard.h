// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// FxSlotCard — one Serum/Pigments-style modular FX-slot card for the FX page's
// upper region (FX1 / FX2 / FX3). It is a SELF-CONTAINED juce::Component that
// owns the per-slot effect's entire control surface, replacing the prior
// generic ParamPage knob grid with a structured modular layout:
//
//   HEADER  (~16px):  FX N (upper-left, bold uppercase 14px — synth GroupComponent
//       header parity) + a compact power/bypass toggle in the TOP-RIGHT corner.
//       The toggle's HIT area is the full 44x44 card corner (kPowerHitSize, the
//       HIG touch minimum) while its glyph stays pinned to the small header spot,
//       so the look is unchanged and only the tappable region grows. The
//       Enable/Bypass control is a POWER toggle button (the IEC glyph), NOT a
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
//       height) — a FIXED 3-column x 2-row layout (6 cells = up to 5 params +
//       Dry/Wet). The ACTIVE fx{N}_param1..5 knobs fill row-major from the top-
//       left; the fx{N}_drywet knob is FIXED in the BOTTOM-RIGHT cell (cell 5)
//       so it never moves when the effect type changes. Inactive param cells are
//       simply empty (the knob is hidden). When the slot type is None the Dry/Wet
//       knob is HIDDEN too (the whole grid collapses — a None slot has no mix).
//       The grid block centres vertically in its region; cells render the
//       full 52px dial (synth parity).
//
// Dynamic parameter labels: on a type change the active param knobs are
// relabelled to the active DSP algorithm's semantic names (Time / Feedback /
// Spread / Size / Damp / Rate / ...) via ParamControl::setDisplayLabel(). That
// method is added to ParamControl by the editor during integration; this card
// only CALLS it (the file does not compile standalone until that lands).
//
// The six knobs are full ParamControl instances (created here from the
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
    // The power/bypass toggle's HIT area: a 44x44 card-corner band (the HIG
    // touch minimum — the old 16px header strip left a ~10x12pt rect, reliably
    // missed by a fingertip). The VISUAL glyph stays small + pinned to the
    // header corner (PowerToggle::paintButton), so only the tappable region
    // grows. Pinned by tests/ipad_hig_sizing_test.cpp.
    static constexpr int kPowerHitSize = 44;

    /** Construct one FX-slot card.
        @param processor  the audio processor (APVTS access).
        @param themeManager  the editor theme manager.
        @param slot  0..2 (-> fx1_ / fx2_ / fx3_).
        @param p1Desc..p5Desc  descriptors for fx{N}_param1..5 (the card creates +
               owns the ParamControls; they are full modulation-destination knobs).
        @param drywetDesc  descriptor for fx{N}_drywet (the fixed "Dry/Wet" knob,
               anchored bottom-right; hidden when the slot type is None). */
    FxSlotCard (ParvatiAudioProcessor& processor, int slot,
                const PatchParamDescriptor* p1Desc, const PatchParamDescriptor* p2Desc,
                const PatchParamDescriptor* p3Desc, const PatchParamDescriptor* p4Desc,
                const PatchParamDescriptor* p5Desc,
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

    // Step the effect TYPE by @p delta (-1 prev / +1 next), clamped to the
    // choice range, and write it through the APVTS (the ComboBoxAttachment +
    // the APVTS::Listener sync the combo + refresh the knob set).
    void stepType (int delta);

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
    // Six full ParamControl knobs (keep FX-mod-matrix drag-drop + mod rings):
    // fx{N}_param1..5 + the fixed fx{N}_drywet (bottom-right; hidden on None).
    std::unique_ptr<ParamControl> p1_, p2_, p3_, p4_, p5_, drywet_;

    std::unique_ptr<juce::ComboBox> typeCombo_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> typeAttach_;

    // Prev/next ("<" ">") step buttons flanking the type combo: a shortcut to
    // cycle the effect TYPE without opening the dropdown.
    std::unique_ptr<juce::Button> typePrev_, typeNext_;

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

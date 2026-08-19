// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// SeqLengthStepper — a ParamControl subclass for the sequencer LENGTH params
// (seq_length_{1,2,3}, a normal 1..16 Int). A 1..16 knob is an awkward control
// for "how many steps"; the cell is too small for two 44pt buttons (the prior
// audit's STOPPED item T9a), so since the 2026-08-19 iOS hunt the NUMBER is
// the control: the whole cell is one >=44pt tap target that opens a picker
// popup of 44pt rows (1..16, the current value ticked — the T7 idiom). The
// slider (and its SliderAttachment) stay as the param<->value backing but are
// HIDDEN. The bold "Length" caption (set by the base ParamControl ctor for
// seq_length_*) is preserved. Desktop keyboard parity: up/down and +/- nudge.
//
// Picks/nudges clamp to 1..16 and write via the hidden slider
// (sendNotificationAsync) so the attachment propagates each change with proper
// undo granularity. The number label follows the slider value via
// onValueChange (covers both picks and external param writes like preset load
// / host automation).

#pragma once

#include "PluginEditor.h"   // ParamControl, ParvatiAudioProcessor

class SeqLengthStepper : public ParamControl,
                         private juce::Button::Listener
{
public:
    SeqLengthStepper (ParvatiAudioProcessor& processor, const PatchParamDescriptor& descriptor);
    ~SeqLengthStepper() override = default;

    void resized() override;

    // F-ios-touch-2: the 44pt-row picker popup height (the T7 idiom's HIG
    // floor for popup items).
    static constexpr int kPopupRowHeight = 44;

    // Test hook (headless): open the 1..16 picker exactly as a tap does.
    void showLengthPopup();

    // Test-only seam (F-ios-touch-2): drive a picker item action + the
    // keyboard nudge headlessly (the lifecycle test [4]).
    void setValueForTest (int v) { setValue (v); }
    bool keyPressedForTest (const juce::KeyPress& k) { return keyPressed (k); }

private:
    // Button::Listener: the full-cell tap button.
    void buttonClicked (juce::Button*) override;

    bool keyPressed (const juce::KeyPress& key) override;

    // +/- clamp 1..16 and nudge the hidden slider (the attachment propagates).
    void nudge (int delta);
    // Set an absolute 1..16 value (a picker pick; its own undo transaction).
    void setValue (int v);

    void refreshNumberLabel();

    // The invisible full-cell button that turns the NUMBER into the control.
    std::unique_ptr<juce::TextButton> tapBtn_;
    std::unique_ptr<juce::Label>      numberLabel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SeqLengthStepper)
};

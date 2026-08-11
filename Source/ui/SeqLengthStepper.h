// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// SeqLengthStepper — a ParamControl subclass for the sequencer LENGTH params
// (seq_length_{1,2,3}, a normal 1..16 Int). A 1..16 knob is an awkward control
// for "how many steps", so this replaces it with a − [ n ] + stepper in the
// same cell: the slider (and its SliderAttachment) stay as the param↔value
// backing but are HIDDEN; a − button, a centred number label and a + button sit
// over the dial area. The bold "Length" caption (set by the base ParamControl
// ctor for seq_length_*) is preserved.
//
// −/+ clamp to 1..16 and write via the hidden slider (sendNotificationAsync) so
// the existing SliderAttachment propagates each click to the param with proper
// undo granularity (one step per click). The number label follows the slider
// value via onValueChange (covers both button clicks and external param writes
// like preset load / host automation).

#pragma once

#include "PluginEditor.h"   // ParamControl, ParvatiAudioProcessor

class SeqLengthStepper : public ParamControl
{
public:
    SeqLengthStepper (ParvatiAudioProcessor& processor, const PatchParamDescriptor& descriptor);
    ~SeqLengthStepper() override = default;

    void resized() override;

private:
    // −/+ clamp 1..16 and nudge the hidden slider (the attachment propagates).
    void nudge (int delta);

    void refreshNumberLabel();

    std::unique_ptr<juce::TextButton> minusBtn_, plusBtn_;
    std::unique_ptr<juce::Label>      numberLabel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SeqLengthStepper)
};

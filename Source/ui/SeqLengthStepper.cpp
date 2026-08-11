// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See SeqLengthStepper.h.

#include "ui/SeqLengthStepper.h"

SeqLengthStepper::SeqLengthStepper (ParvatiAudioProcessor& processor,
                                    const PatchParamDescriptor& descriptor)
    : ParamControl (processor, descriptor)
{
    // The base ctor created the 1..16 slider + SliderAttachment (the value
    // backing) and the bold "Length" label. Hide the slider and overlay a
    // − / number / + stepper. The attachment stays alive so undo / host
    // automation / preset load all flow through the normal slider->param path.
    if (slider_ != nullptr)
    {
        slider_->setVisible (false);
        // The hidden slider still carries the value; mirror it to the number
        // label whenever it changes (button nudge OR external param write).
        slider_->onValueChange = [this] { refreshNumberLabel(); };
    }

    minusBtn_ = std::make_unique<juce::TextButton> ("seqLenMinus", "-");
    plusBtn_  = std::make_unique<juce::TextButton> ("seqLenPlus",  "+");
    minusBtn_->onClick = [this] { nudge (-1); };
    plusBtn_->onClick  = [this] { nudge (+1); };
    addAndMakeVisible (*minusBtn_);
    addAndMakeVisible (*plusBtn_);

    numberLabel_ = std::make_unique<juce::Label> ("seqLenNum", juce::String());
    numberLabel_->setJustificationType (juce::Justification::centred);
    numberLabel_->setFont (juce::FontOptions (15.0f, juce::Font::bold));
    addAndMakeVisible (*numberLabel_);

    refreshNumberLabel();
}

void SeqLengthStepper::nudge (int delta)
{
    if (slider_ == nullptr)
        return;
    // Bracket each click as its own undo step (programmatic setValue otherwise
    // coalesces a burst of clicks into one undo entry). Mirrors resetToDefault /
    // randomize (PluginEditor.cpp) and NoteStepControl's drag transactions.
    processor_.getUndoManager().beginNewTransaction();
    // Clamp to the param's 1..16 range (defensive: the slider range is already
    // 1..16, but jlimit guards against any descriptor mismatch).
    const int cur = juce::roundToInt (slider_->getValue());
    const int next = juce::jlimit (1, 16, cur + delta);
    slider_->setValue (static_cast<double> (next), juce::sendNotificationAsync);
}

void SeqLengthStepper::refreshNumberLabel()
{
    if (numberLabel_ == nullptr || slider_ == nullptr)
        return;
    numberLabel_->setText (juce::String (juce::roundToInt (slider_->getValue())),
                           juce::dontSendNotification);
}

void SeqLengthStepper::resized()
{
    // Base lays out the bold "Length" label + the (hidden) slider bounds. Then
    // overlay − / number / + across the dial band, centred.
    ParamControl::resized();

    auto b = getLocalBounds().reduced (2);
    // SeqLengthStepper is only ever a seq_length_* control, so the base ctor
    // always reserves the bold "Length" label band (15px + 3px gap).
    b.removeFromTop (15);
    b.removeFromTop (3);

    constexpr int btnW = 22, numW = 30, gap = 4;
    const int totalW = btnW + gap + numW + gap + btnW;
    auto row = b.withSizeKeepingCentre (juce::jmin (totalW, b.getWidth()),
                                        juce::jmin (28, b.getHeight()));
    if (minusBtn_ != nullptr) minusBtn_->setBounds (row.removeFromLeft (btnW));
    row.removeFromLeft (gap);
    if (numberLabel_ != nullptr) numberLabel_->setBounds (row.removeFromLeft (numW));
    row.removeFromLeft (gap);
    if (plusBtn_ != nullptr) plusBtn_->setBounds (row.removeFromLeft (btnW));
}

// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See SeqLengthStepper.h.

#include "ui/SeqLengthStepper.h"

SeqLengthStepper::SeqLengthStepper (ParvatiAudioProcessor& processor,
                                    const PatchParamDescriptor& descriptor)
    : ParamControl (processor, descriptor)
{
    // The base ctor created the 1..16 slider + SliderAttachment (the value
    // backing) and the bold "Length" label. Hide the slider and overlay the
    // big number; TAPPING THE NUMBER opens a 44pt-row 1..16 picker. The
    // attachment stays alive so undo / host automation / preset load all flow
    // through the normal slider->param path.
    if (slider_ != nullptr)
    {
        slider_->setVisible (false);
        // The hidden slider still carries the value; mirror it to the number
        // label whenever it changes (picker pick OR external param write).
        slider_->onValueChange = [this] { refreshNumberLabel(); };
    }

    // F-ios-touch-2 (bug hunt 2026-08-19): the old − / + button pair rendered
    // at ~32x20 inside the 72x64 sequencer grid cell — far below the 44pt HIG
    // floor, and two 44x44 buttons provably cannot fit that cell (the prior
    // audit's STOPPED item T9a). Replacement interaction (the audit's own
    // option list): the NUMBER is the control — a full-cell tappable target —
    // opening a picker popup of 44pt rows (the T7 idiom). Keyboard up/down
    // remains for desktop (keyPressed below).
    numberLabel_ = std::make_unique<juce::Label> ("seqLenNum", juce::String());
    numberLabel_->setJustificationType (juce::Justification::centred);
    numberLabel_->setFont (juce::FontOptions (17.0f, juce::Font::bold));
    numberLabel_->setInterceptsMouseClicks (false, false);   // the CELL is the button
    addAndMakeVisible (*numberLabel_);

    tapBtn_ = std::make_unique<juce::TextButton> (juce::String(), TRANS ("Set sequence length"));
    tapBtn_->setBounds ({});   // logic-only: the whole cell IS its hit band
    tapBtn_->addListener (this);
    addAndMakeVisible (*tapBtn_);

    refreshNumberLabel();
}

void SeqLengthStepper::buttonClicked (juce::Button*)
{
    showLengthPopup();
}

bool SeqLengthStepper::keyPressed (const juce::KeyPress& key)
{
    // Desktop keyboard parity with the old − / + buttons: up/down (and the
    // legacy +/- keys) nudge the length; every other key falls through.
    int delta = 0;
    if (key == juce::KeyPress::upKey || key == juce::KeyPress::rightKey)  delta = +1;
    if (key == juce::KeyPress::downKey || key == juce::KeyPress::leftKey) delta = -1;
    if (key.getTextCharacter() == '+') delta = +1;
    if (key.getTextCharacter() == '-') delta = -1;
    if (delta == 0)
        return ParamControl::keyPressed (key);
    nudge (delta);
    return true;
}

void SeqLengthStepper::nudge (int delta)
{
    if (slider_ == nullptr)
        return;
    // Bracket each step as its own undo step (programmatic setValue otherwise
    // coalesces a burst into one undo entry). Mirrors resetToDefault /
    // randomize (PluginEditor.cpp) and NoteStepControl's drag transactions.
    processor_.getUndoManager().beginNewTransaction();
    const int cur = juce::roundToInt (slider_->getValue());
    const int next = juce::jlimit (1, 16, cur + delta);
    // SYNC notification: this seam is message-thread-only by construction
    // (a picker item action / a key event), and the value must be visible to
    // the immediate caller (tests read the param synchronously).
    slider_->setValue (static_cast<double> (next), juce::sendNotificationSync);
}

void SeqLengthStepper::setValue (int v)
{
    if (slider_ == nullptr)
        return;
    processor_.getUndoManager().beginNewTransaction();
    slider_->setValue (static_cast<double> (juce::jlimit (1, 16, v)),
                       juce::sendNotificationSync);   // message-thread seam (see nudge)
}

void SeqLengthStepper::showLengthPopup()
{
    // F-ios-touch-2: 44pt-row picker (T7 idiom). SafePointer-guarded actions;
    // explicit setLookAndFeel so the popup is themed (PopupMenu L&F comes only
    // from setLookAndFeel — withTargetComponent does NOT theme it).
    juce::PopupMenu m;
    m.setLookAndFeel (&getLookAndFeel());
    const int current = slider_ != nullptr ? juce::roundToInt (slider_->getValue()) : 1;
    for (int v = 1; v <= 16; ++v)
    {
        juce::PopupMenu::Item item;
        item.text      = juce::String (v);
        item.itemID    = v;
        item.isTicked  = (v == current);
        juce::Component::SafePointer<SeqLengthStepper> safe { this };
        item.action    = [safe, v] { if (safe != nullptr) safe->setValue (v); };
        m.addItem (std::move (item));
    }
    m.showMenuAsync (juce::PopupMenu::Options()
                         .withTargetComponent (this)
                         .withStandardItemHeight (kPopupRowHeight));
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
    // Base lays out the bold "Length" label + the (hidden) slider bounds; the
    // whole remaining cell is the tappable number target (F-ios-touch-2: one
    // >=44pt target instead of two sub-44 buttons).
    ParamControl::resized();

    auto b = getLocalBounds().reduced (2);
    b.removeFromTop (15);
    b.removeFromTop (3);

    if (tapBtn_ != nullptr)
        tapBtn_->setBounds (b);
    if (numberLabel_ != nullptr)
        numberLabel_->setBounds (b);
}

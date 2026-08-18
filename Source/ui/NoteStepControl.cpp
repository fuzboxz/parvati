// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See NoteStepControl.h.

#include "ui/NoteStepControl.h"

#include "ui/NoteName.h"   // midiNoteName

NoteStepControl::NoteStepControl (ParvatiAudioProcessor& processor,
                                  const PatchParamDescriptor& descriptor)
    : ParamControl (processor, descriptor)
{
    // The base ParamControl ctor already created a 0..255 SliderAttachment bound
    // to the byte param and set the slider to the current byte value. Tear it
    // down and re-range the slider to the remapped note range (0=Rest, 1..128 =
    // notes 0..127). Keeping the same slider_ instance preserves the inherited
    // rotary L&F, mod-ring, mouse/right-click wiring and category arc colour.
    sliderAttachment_.reset();

    // Discrete note range: 129 stops. 0 = Rest (the hard-left min stop), 1..128
    // = MIDI notes 0..127. The whole former dead 0..127 zone collapses into the
    // single Rest stop, so 100% of the remaining travel is audible notes.
    slider_->setRange (0.0, 128.0, 1.0);
    slider_->textFromValueFunction = [] (double v)
    {
        return v <= 0.0 ? juce::String ("Rest") : midiNoteName (juce::roundToInt (v) - 1);
    };

    // Subscribe to the byte param so external writes (preset load / undo / host
    // automation / Reset / Randomize) decode back to the slider. The base ctor
    // already listens to the sibling length param for step-dimming.
    noteValue_ = processor.getApvts().getParameterAsValue (descriptor.paramID);
    paramID_ = juce::String (descriptor.paramID);
    processor.getApvts().addParameterListener (descriptor.paramID, this);

    // Seed the slider from the current byte (the torn-down attachment had set it
    // to the raw 0..255 value, which is outside the new 0..128 range).
    if (auto* raw = processor.getApvts().getRawParameterValue (descriptor.paramID))
        slider_->setValue (byteToSlider (juce::roundToInt (raw->load())),
                           juce::dontSendNotification);

    // slider drag -> write the recomposed byte. onDragStart/onDragEnd bracket
    // each gesture with a fresh undo transaction (the torn-down attachment did
    // this internally; we replicate it so a drag is one undo step, like every
    // other knob). Writing via noteValue_ (getParameterAsValue) routes through
    // the APVTS undo path (the same path ParamControl::resetToDefault uses).
    slider_->onValueChange = [this] { sliderValueChanged(); };
    slider_->onDragStart   = [this] { processor_.getUndoManager().beginNewTransaction(); };
    slider_->onDragEnd     = [this] { processor_.getUndoManager().beginNewTransaction(); };

    slider_->repaint();
}

NoteStepControl::~NoteStepControl()
{
    processor_.getApvts().removeParameterListener (paramID_, this);
}

int NoteStepControl::sliderToByte (double sliderValue) noexcept
{
    // 0 => Rest (gate off, any note). 1..128 => note 0..127 with gate on.
    if (sliderValue <= 0.0)
        return 0;
    const int note = (juce::roundToInt (sliderValue) - 1) & 0x7f;
    return 0x80 | note;
}

int NoteStepControl::byteToSlider (int byte) noexcept
{
    // gate off (byte < 128) => Rest; gate on => note+1 (1..128).
    return (byte < 128) ? 0 : ((byte & 0x7f) + 1);
}

void NoteStepControl::sliderValueChanged()
{
    if (slider_ == nullptr)
        return;
    noteValue_.setValue (static_cast<float> (sliderToByte (slider_->getValue())));
}

void NoteStepControl::parameterChanged (const juce::String& parameterID, float newValue)
{
    // F-ui-1 (bug hunt 2026-08-18): same audio-thread delivery hazard as the
    // base (NRPN map inside processBlock / host automation) — the slider
    // mutation below is message-thread-only. Defer and decode from CURRENT
    // state in handleAsyncUpdate.
    if (! juce::MessageManager::existsAndIsCurrentThread())
    {
        triggerAsyncUpdate();
        return;
    }

    // Base first: handles the sibling seq_length_* step-dimming.
    ParamControl::parameterChanged (parameterID, newValue);

    if (parameterID != paramID_ || slider_ == nullptr)
        return;

    // Decode the byte -> slider. dontSendNotification so this does NOT re-fire
    // onValueChange (which would write the byte again -> a benign but wasteful
    // re-entrant round-trip).
    slider_->setValue (byteToSlider (juce::roundToInt (newValue)),
                       juce::dontSendNotification);
    slider_->repaint();
}

void NoteStepControl::handleAsyncUpdate()
{
    // F-ui-1: the deferred refresh — base refreshes (step dimming, mod tint,
    // rings) plus this control's byte->slider decode, from CURRENT state.
    ParamControl::handleAsyncUpdate();
    if (slider_ == nullptr)
        return;
    if (auto* raw = processor_.getApvts().getRawParameterValue (paramID_))
        slider_->setValue (byteToSlider (juce::roundToInt (raw->load())),
                           juce::dontSendNotification);
    slider_->repaint();
}

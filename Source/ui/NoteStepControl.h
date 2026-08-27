// Copyright (c) 2026 805Labs Kft. / Hellcat.
//
// NoteStepControl — a ParamControl subclass for the note-sequencer step byte
// (seqnote_step{0..15}). The underlying APVTS param is a single 0..255 byte
// whose bit 7 is the GATE and bits 0..6 the NOTE (Sequencer.h noteStep():
//   n.note = byte & 0x7f;  n.gate = (byte & 0x80) != 0).
// Exposed as one 0..255 knob that is HALF DEAD: dragging 0..127 sets a pitch
// with the gate CLEAR (a silent rest), so only the upper half (128..255) is
// audible — "only works from 50% of the range".
//
// This control REMAPS the knob so the dead 0..127 zone collapses into ONE
// "Rest" stop at the minimum: the slider's discrete range is 0..128 where
//   0     => Rest (gate off)
//   1..128 => MIDI notes 0..127 (gate on)
// so the FULL travel is meaningful (one rest stop + a complete note range). It
// tears down the byte-range SliderAttachment and drives the byte param itself
// via getParameterAsValue + addParameterListener (the same composite-over-one-
// param pattern the FX power-toggle uses), so:
//   slider -> param:  byte = (v<=0) ? 0 : (0x80 | ((v-1) & 0x7f))
//   param  -> slider: v = (byte < 128) ? 0 : ((byte & 0x7f) + 1)
// Preset serialization, the engine byte-bridge, MIDI-learn and the right-click
// Reset/Randomize menu are all untouched (they read/write the raw byte; this
// control's parameterChanged decodes it to the slider).

#pragma once

#include "ParamControl.h"   // ParamControl, HellcatAudioProcessor

// A single note-sequencer step cell: one remapped rotary (Rest-at-min + full
// note range). Lives in a ParamPage's controls_ vector (polymorphic ParamControl)
// so it inherits the modulation ring, right-click menu, step-dimming past the
// sequence length, label handling and tooltip machinery.
class NoteStepControl : public ParamControl
{
public:
    NoteStepControl (HellcatAudioProcessor& processor, const PatchParamDescriptor& descriptor);
    ~NoteStepControl() override;

    // Slider value (0..128: 0=Rest, 1..128=note 0..127) -> seqnote_step byte.
    // Public for the unit test (tests/note_step_control_test.cpp): the
    // Rest-collapse + gate-bit recomposition is the control's whole contract.
    static int sliderToByte (double sliderValue) noexcept;
    // seqnote_step byte (0..255) -> slider value (0=Rest, 1..128=note).
    static int byteToSlider (int byte) noexcept;

private:
    // APVTS::Listener: a byte change (preset load / host automation / undo /
    // Reset / Randomize) -> decode to the slider. Calls the base first so the
    // sibling-length step-dimming still runs.
    void parameterChanged (const juce::String& parameterID, float newValue) override;
    // F-ui-1: deferred half of parameterChanged when it fired on the audio
    // thread (see ParamControl::handleAsyncUpdate) — decodes the CURRENT
    // byte to the slider on the message thread.
    void handleAsyncUpdate() override;

    // slider drag -> recompose + write the byte param.
    void sliderValueChanged();

    juce::Value noteValue_;   // getParameterAsValue(seqnote_step*) — byte writes
    juce::String paramID_;    // cached juce::String (desc_.paramID is std::string)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoteStepControl)
};

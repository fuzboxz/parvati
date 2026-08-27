// Copyright (c) 2026 805Labs Kft. / Hellcat.
//
// NoteName — the canonical MIDI-note-number -> name formatter ("C4" convention:
// MIDI 60 == C4, i.e. octave = note/12 - 1). Header-only; depends only on
// juce_core. Used by the synth readout formatter (SynthParamLabels) and the
// Patch page key-zone knobs.
//
// (Consolidation note: the local kNoteNames in FxSlotLabels.cpp implements
// the IDENTICAL math. This is the canonical home; deduping that caller onto
// it is a mechanical follow-up with identical math — deliberately deferred
// here so the just-shipped FX code is not perturbed.)

#pragma once

#include <juce_core/juce_core.h>   // juce::String

// MIDI note number (0..127) -> "C4"-style name. Returns {} for out-of-range.
inline juce::String midiNoteName (int note)
{
    static const char* const kNoteNames[12] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    if (note < 0 || note > 127)
        return {};
    const int octave = note / 12 - 1;   // 60 -> 5 - 1 == 4 => "C4"
    return juce::String (kNoteNames[note % 12]) + juce::String (octave);
}

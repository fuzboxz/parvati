// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See FxSlotLabels.h.
//
// Definitions of the per-FX-type active-parameter count + semantic short labels.
// These are at GLOBAL scope (not an anonymous namespace) so they have external
// linkage and link across translation units (FxSlotCard.cpp for the live knob
// labels, FxMatrixView.cpp for the dynamic FX-mod-matrix destination labels).
// FxSlotCard.cpp previously held these in its anonymous namespace; they were
// hoisted out here to be shareable.

#include "FxSlotLabels.h"

#include <cmath>

//==============================================================================
// idx is 0..3 (param1..4). Inactive params (idx >= activeParamCount) are
// hidden on the slot card and omitted from the FX-mod dest combo.
int activeParamCount (FxType t) noexcept
{
    switch (t)
    {
        case FxType::Diffuser:     return 0;
        case FxType::PitchShifter: return 2;
        case FxType::Reverb: return 3;
        case FxType::LoopingDelay:  return 4;
        case FxType::WSOLAStretch:  return 3;
        case FxType::Spectral:      return 4;
        case FxType::Wavefolder:    return 2;
        case FxType::FrequencyShifter: return 3;
        case FxType::RingModulator:  return 3;
        case FxType::Resonator:      return 4;
        case FxType::None:
        case FxType::Count:   return 0;
    }
    return 0;   // unreachable; keeps -Wreturn-type calm
}

const char* paramLabel (FxType t, int idx) noexcept
{
    switch (t)
    {
        case FxType::Diffuser:
            break;
        case FxType::PitchShifter:
            if (idx == 0) return "Pitch";
            if (idx == 1) return "Size";
            break;
        case FxType::Reverb:
            if (idx == 0) return "Time";
            if (idx == 1) return "Tone";
            if (idx == 2) return "Diffusion";
            break;
        case FxType::LoopingDelay:
            if (idx == 0) return "Position";
            if (idx == 1) return "Size";
            if (idx == 2) return "Pitch";
            if (idx == 3) return "Freeze";
            break;
        case FxType::WSOLAStretch:
            if (idx == 0) return "Pitch";
            if (idx == 1) return "Position";
            if (idx == 2) return "Size";
            break;
        case FxType::Spectral:
            if (idx == 0) return "Pitch";
            if (idx == 1) return "Warp";
            if (idx == 2) return "Position";
            if (idx == 3) return "Blur";
            break;
        case FxType::Wavefolder:
            if (idx == 0) return "Fold";
            if (idx == 1) return "Bias";
            break;
        case FxType::FrequencyShifter:
            if (idx == 0) return "Shift";
            if (idx == 1) return "Feedback";
            if (idx == 2) return "Spread";
            break;
        case FxType::RingModulator:
            if (idx == 0) return "Carrier";
            if (idx == 1) return "Shape";
            if (idx == 2) return "Drive";
            break;
        case FxType::Resonator:
            if (idx == 0) return "Pitch";
            if (idx == 1) return "Decay";
            if (idx == 2) return "Bright";
            if (idx == 3) return "Position";
            break;
        case FxType::None:
        case FxType::Count:
            break;
    }
    return "-";
}

// Format a signed semitone readout, snapping to the nearest INTEGER semitone
// when within half a knob quantization step (range/127 steps => half-step =
// range/254). Without this, a +/-12 range over 127 steps (~0.189/step) shows
// "6.9" then "7.1" with no "7.0" in between; the snap makes every integer land.
static juce::String formatSemis (double semis, double range) noexcept
{
    const double halfStep = range / 254.0;
    const double nearest  = std::round (semis);
    if (std::fabs (semis - nearest) <= halfStep + 1e-9)
        semis = nearest;
    return juce::String (semis >= 0.0 ? "+" : "") + juce::String (semis, 1) + " st";
}

//==============================================================================
// Per-param meaningful-unit value readout (DISPLAY-ONLY; stored value unchanged).
// p = value0to127/127.0 mirrors the DSP normalization (SynthEngine / ParamControl).
juce::String paramValueText (FxType t, int idx, double value0to127)
{
    const double p = value0to127 / 127.0;

    switch (t)
    {
        case FxType::Resonator:
            if (idx == 0)   // Pitch -> MIDI note 24..96 (C1..C7)
            {
                static const char* const kNoteNames[] = {
                    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
                const int note   = juce::jlimit (0, 127, juce::roundToInt (24.0 + p * 72.0));
                const int octave = note / 12 - 1;
                return juce::String (kNoteNames[note % 12]) + juce::String (octave);
            }
            break;

        case FxType::PitchShifter:
            if (idx == 0)   // Pitch -> +/-12 semitones
                return formatSemis ((p - 0.5) * 24.0, 24.0);
            break;

        case FxType::WSOLAStretch:
        case FxType::Spectral:
            if (idx == 0)   // Pitch -> +/-24 semitones
                return formatSemis ((p - 0.5) * 48.0, 48.0);
            break;

        case FxType::LoopingDelay:
            if (idx == 2)   // Pitch -> +/-24 semitones
                return formatSemis ((p - 0.5) * 48.0, 48.0);
            if (idx == 3)   // Freeze -> On/Off (threshold at p > 0.5)
                return p > 0.5 ? "On" : "Off";
            break;

        case FxType::FrequencyShifter:
            if (idx == 0)   // Shift -> Hz (non-linear; mirrors FxProcessors.cpp verbatim)
            {
                const double dir = p >= 0.5 ? 1.0 : -1.0;
                double f = 2.0 * std::fabs (p - 0.5);
                f = f <= 0.4 ? f * f * f * 62.5
                             : 4.0 * std::pow (2.0, 180.0 * (f - 0.4) / 12.0);
                const double hz = f * dir;
                return juce::String (hz > 0.0 ? "+" : "") + juce::String (juce::roundToInt (hz)) + " Hz";
            }
            break;

        case FxType::RingModulator:
            if (idx == 0)   // Carrier -> Hz (20..4000, log)
            {
                const double hz = 20.0 * std::pow (200.0, p);
                if (hz < 1000.0)
                    return juce::String (juce::roundToInt (hz)) + " Hz";
                return juce::String (hz / 1000.0, 2) + " kHz";
            }
            break;

        default:
            break;
    }

    // Default: dimensionless param -> 0..100%
    return juce::String (juce::roundToInt (p * 100.0)) + "%";
}

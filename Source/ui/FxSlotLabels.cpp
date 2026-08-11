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
// idx is 0..4 (param1..5). Inactive params (idx >= activeParamCount) are
// hidden on the slot card and omitted from the FX-mod dest combo.
int activeParamCount (FxType t) noexcept
{
    switch (t)
    {
        case FxType::Diffuser:     return 0;
        case FxType::PitchShifter: return 3;
        case FxType::Reverb: return 5;
        case FxType::LoopingDelay:  return 4;
        case FxType::WSOLAStretch:  return 5;
        case FxType::Spectral:      return 5;
        case FxType::Wavefolder:    return 4;
        case FxType::FrequencyShifter: return 4;
        case FxType::RingModulator:  return 3;
        case FxType::Resonator:      return 5;
        case FxType::ClockedDelay:    return 4;
        case FxType::Ensemble:        return 4;
        case FxType::PlateReverb:     return 4;
        case FxType::VinylCompressor: return 4;
        case FxType::Phaser:          return 4;
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
            if (idx == 2) return "Spread";
            break;
        case FxType::Reverb:
            if (idx == 0) return "Predelay";
            if (idx == 1) return "Diffusion";
            if (idx == 2) return "Time";
            if (idx == 3) return "Tone";
            if (idx == 4) return "Low-Cut";
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
            if (idx == 3) return "Freeze";
            if (idx == 4) return "Tone";
            break;
        case FxType::Spectral:
            if (idx == 0) return "Pitch";
            if (idx == 1) return "Warp";
            if (idx == 2) return "Position";
            if (idx == 3) return "Blur";
            if (idx == 4) return "Freeze";
            break;
        case FxType::Wavefolder:
            if (idx == 0) return "Drive";
            if (idx == 1) return "Fold";
            if (idx == 2) return "Bias";
            if (idx == 3) return "Tone";
            break;
        case FxType::FrequencyShifter:
            if (idx == 0) return "Shift";
            if (idx == 1) return "Shape";
            if (idx == 2) return "Feedback";
            if (idx == 3) return "Spread";
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
            if (idx == 4) return "Structure";
            break;
        case FxType::ClockedDelay:
            if (idx == 0) return "Sync";
            if (idx == 1) return "Feedback";
            if (idx == 2) return "Tape Age";
            if (idx == 3) return "Grit";
            break;
        case FxType::Ensemble:
            if (idx == 0) return "Rate";
            if (idx == 1) return "Depth";
            if (idx == 2) return "Center";
            if (idx == 3) return "Feedback";
            break;
        case FxType::PlateReverb:
            if (idx == 0) return "Predelay";
            if (idx == 1) return "Decay";
            if (idx == 2) return "Damping";
            if (idx == 3) return "Mod";
            break;
        case FxType::VinylCompressor:
            if (idx == 0) return "Compress";
            if (idx == 1) return "Pitch";
            if (idx == 2) return "Crackle";
            if (idx == 3) return "Age";
            break;
        case FxType::Phaser:
            if (idx == 0) return "Rate";
            if (idx == 1) return "Depth";
            if (idx == 2) return "Feedback";
            if (idx == 3) return "Center";
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

        case FxType::Reverb:
            if (idx == 0)   // Predelay -> 0..200 ms (mirrors FxReverb's 0.2 s cap)
                return juce::String (juce::roundToInt (p * 200.0)) + " ms";
            break;

        case FxType::WSOLAStretch:
            if (idx == 0)   // Pitch -> +/-24 semitones
                return formatSemis ((p - 0.5) * 48.0, 48.0);
            if (idx == 3)   // Freeze -> On/Off (threshold at p > 0.5)
                return p > 0.5 ? "On" : "Off";
            break;

        case FxType::LoopingDelay:
            if (idx == 2)   // Pitch -> +/-24 semitones
                return formatSemis ((p - 0.5) * 48.0, 48.0);
            if (idx == 3)   // Freeze -> On/Off (threshold at p > 0.5)
                return p > 0.5 ? "On" : "Off";
            break;

        case FxType::Spectral:
            if (idx == 0)   // Pitch -> +/-24 semitones
                return formatSemis ((p - 0.5) * 48.0, 48.0);
            if (idx == 4)   // Freeze -> On/Off (threshold at p > 0.5)
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

        case FxType::ClockedDelay:
            if (idx == 0)   // Sync -> division (8 steps: 1/1..1/16)
            {
                static const char* const kDiv[] = { "1/1", "1/2", "1/3", "1/4", "1/6", "1/8", "1/12", "1/16" };
                const int i = juce::jlimit (0, 7, juce::roundToInt (p * 7.0));
                return kDiv[i];
            }
            if (idx == 1)   // Feedback -> 0..95 %
                return juce::String (juce::roundToInt (p * 95.0)) + " %";
            if (idx == 3)   // Grit -> 24-bit..8-bit
                return juce::String (24 - juce::roundToInt (p * 16.0)) + "-bit";
            break;

        case FxType::Ensemble:
            if (idx == 0)   // Rate -> 0.1..8 Hz (log)
                return juce::String (0.1 * std::pow (80.0, p), 2) + " Hz";
            if (idx == 1)   // Depth -> 0..15 ms
                return juce::String (p * 15.0, 1) + " ms";
            if (idx == 2)   // Center -> 2..25 ms
                return juce::String (2.0 + p * 23.0, 1) + " ms";
            if (idx == 3)   // Feedback -> -90..+90 %
                return juce::String (juce::roundToInt ((-0.9 + p * 1.8) * 100.0)) + " %";
            break;

        case FxType::PlateReverb:
            if (idx == 0)   // Predelay -> 0..100 ms
                return juce::String (juce::roundToInt (p * 100.0)) + " ms";
            if (idx == 1)   // Decay -> 0.1..4.0 s
                return juce::String (0.1 + p * 3.9, 2) + " s";
            if (idx == 2)   // Damping -> 500..12000 Hz (log)
            {
                const double hz = 500.0 * std::pow (24.0, p);
                return hz < 1000.0 ? juce::String (juce::roundToInt (hz)) + " Hz"
                                   : juce::String (hz / 1000.0, 2) + " kHz";
            }
            break;

        case FxType::VinylCompressor:
            if (idx == 3)   // Age -> 1000..15000 Hz (log)
            {
                const double hz = 1000.0 * std::pow (15.0, p);
                return hz < 1000.0 ? juce::String (juce::roundToInt (hz)) + " Hz"
                                   : juce::String (hz / 1000.0, 2) + " kHz";
            }
            break;

        case FxType::Phaser:
            if (idx == 0)   // Rate -> 0.1..8 Hz (log)
                return juce::String (0.1 * std::pow (80.0, p), 2) + " Hz";
            if (idx == 2)   // Feedback -> -90..+90 %
                return juce::String (juce::roundToInt ((-0.9 + p * 1.8) * 100.0)) + " %";
            if (idx == 3)   // Center -> 200..2000 Hz (log)
            {
                const double hz = 200.0 * std::pow (10.0, p);
                return juce::String (juce::roundToInt (hz)) + " Hz";
            }
            break;

        default:
            break;
    }

    // Default: dimensionless param -> 0..100%
    return juce::String (juce::roundToInt (p * 100.0)) + "%";
}

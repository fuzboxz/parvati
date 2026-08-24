// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See FxSlotLabels.h.
//
// Definitions of the per-FX-type active-parameter count + semantic short labels.
// These are at GLOBAL scope (not an anonymous namespace) so they have external
// linkage and link across translation units (FxSlotCard.cpp for the live knob
// labels, FxMatrixView.cpp for the dynamic FX-mod-matrix destination labels).
// FxSlotCard.cpp previously held these in its anonymous namespace; they were
// hoisted out here to be shareable.

#include "FxSlotLabels.h"

#include "FormatHelpers.h"   // hzReadoutFx (single source with the synth Hz readout)

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
        case FxType::Overdrive:       return 4;
        case FxType::LutDistortion:   return 4;
        case FxType::Compressor:      return 4;
        case FxType::Gate:            return 4;
        case FxType::Chorus:          return 4;
        case FxType::Flanger:         return 4;
        case FxType::Echo:            return 4;
        case FxType::Room:            return 4;
        case FxType::Spring:          return 4;
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
            if (idx == 1) return "Wow/Flut";
            if (idx == 2) return "Crackle";
            if (idx == 3) return "Age";
            break;
        case FxType::Phaser:
            if (idx == 0) return "Rate";
            if (idx == 1) return "Depth";
            if (idx == 2) return "Feedback";
            if (idx == 3) return "Center";
            break;
        case FxType::Overdrive:
            if (idx == 0) return "Drive";
            if (idx == 1) return "Bias";
            if (idx == 2) return "Tone";
            if (idx == 3) return "Level";
            break;
        case FxType::LutDistortion:
            if (idx == 0) return "Drive";
            if (idx == 1) return "Wavetable";   // the 16-shape LUT (was "Shape")
            if (idx == 2) return "Jitter";
            if (idx == 3) return "Tone";
            break;
        case FxType::Compressor:
            if (idx == 0) return "Amount";
            if (idx == 1) return "Attack";
            if (idx == 2) return "Release";
            if (idx == 3) return "Level";
            break;
        case FxType::Gate:
            if (idx == 0) return "Thresh";
            if (idx == 1) return "Attack";
            if (idx == 2) return "Hold";
            if (idx == 3) return "Release";
            break;
        case FxType::Chorus:
            if (idx == 0) return "Rate";
            if (idx == 1) return "Depth";
            if (idx == 2) return "Center";
            if (idx == 3) return "Feedback";
            break;
        case FxType::Flanger:
            if (idx == 0) return "Rate";
            if (idx == 1) return "Depth";
            if (idx == 2) return "Manual";
            if (idx == 3) return "Feedback";
            break;
        case FxType::Echo:
            if (idx == 0) return "Time";
            if (idx == 1) return "Feedback";
            if (idx == 2) return "Tone";
            // "Stereo" (not "Stereo Spread"): the slot-card cell caption is
            // narrow; the short form still carries the meaning (R-time spread).
            if (idx == 3) return "Stereo";
            break;
        case FxType::Room:
            if (idx == 0) return "Decay";
            if (idx == 1) return "Damp";
            if (idx == 2) return "Width";
            if (idx == 3) return "Tone";
            break;
        case FxType::Spring:
            if (idx == 0) return "Decay";
            if (idx == 1) return "Damp";
            if (idx == 2) return "Chirp";
            if (idx == 3) return "Width";
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
// NO " st" suffix: the widest signed value with it ("-24.0 st") busts the
// hard 6-char FX-cell budget, and the knob caption ("Pitch") carries the unit.
static juce::String formatSemis (double semis, double range) noexcept
{
    const double halfStep = range / 254.0;
    const double nearest  = std::round (semis);
    if (std::fabs (semis - nearest) <= halfStep + 1e-9)
        semis = nearest;
    return juce::String (semis >= 0.0 ? "+" : "") + juce::String (semis, 1);
}

// Compact FX-page Hz readout: ui/FormatHelpers.h hzReadoutFx (single source
// with the synth filter's electronic-component style — only the documented
// edge policies differ): below 1 kHz -> integer + "Hz" ("820Hz"); 1..10 kHz
// -> one-decimal k-notation with 'k' replacing the decimal point ("1k2",
// "2k6", "6k7"); at or above 10 kHz -> integer kHz ("12k", "15k"). Rounding
// that carries into the next integer kHz (9950 Hz -> "10k") lands in the
// hundreds >= 100 branch of the shared core.

// Compact FX-page milliseconds readout: integer-rounded with NO decimals once
// the value reaches 10 ms ("10ms", "250ms", "470ms"); sub-10 ms values keep
// the requested decimals (1 => "0.5ms"/"2.5ms"; 2 => "0.05ms"/"9.97ms") —
// every form stays <= 6 chars.
static juce::String formatMs (double ms, int decimals) noexcept
{
    if (ms < 10.0)
        return juce::String (ms, decimals) + "ms";
    return juce::String (juce::roundToInt (ms)) + "ms";
}

// Compact seconds readout (decay tails): one decimal, no space — "0.1s".."4.0s"
// (two decimals + space, "1.60 s", busts the 6-char cell budget).
static juce::String formatSecs (double s) noexcept
{
    return juce::String (s, 1) + "s";
}

//==============================================================================
// Per-param meaningful-unit value readout (DISPLAY-ONLY; stored value unchanged).
// p = value0to127/127.0 mirrors the DSP normalization (SynthEngine / ParamControl).
// HARD RULE: every string this returns is <= 6 characters — the FX-slot knob
// cell is narrow. Hz use the compact "820Hz"/"2k6" style (hzReadoutFx), times are
// space-free ms (formatMs), rates are space-free "0.87Hz", decays one-decimal
// "1.6s". tests/fx_param_coverage_test.cpp sweeps every (type, idx, value)
// triple and asserts the budget.
juce::String paramValueText (FxType t, int idx, double value0to127)
{
    const double p = value0to127 / 127.0;

    // Only the types with parameter text have cases. The default covers
    // the parameterless types, so the switch stays exhaustive.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wswitch-enum"
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
                return formatMs (p * 200.0, 1);
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
                // Compact + signed, no spaces: "+230Hz" small, "+1k2" large.
                return juce::String (hz > 0.0 ? "+" : (hz < 0.0 ? "-" : ""))
                       + hzReadoutFx (std::fabs (hz));
            }
            break;

        case FxType::RingModulator:
            if (idx == 0)   // Carrier -> Hz (20..4000, log)
            {
                const double hz = 20.0 * std::pow (200.0, p);
                return hzReadoutFx (hz);   // "20Hz".."999Hz" / "1k0".."4k0"
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
                return juce::String (juce::roundToInt (p * 95.0)) + "%";
            if (idx == 3)   // Grit -> 24-bit..8-bit
                return juce::String (24 - juce::roundToInt (p * 16.0)) + "-bit";
            break;

        case FxType::Ensemble:
            if (idx == 0)   // Rate -> 0.1..8 Hz (log)
                return juce::String (0.1 * std::pow (80.0, p), 2) + "Hz";   // "0.10Hz".."8.00Hz"
            if (idx == 1)   // Depth -> 0..15 ms
                return formatMs (p * 15.0, 1);
            if (idx == 2)   // Center -> 2..25 ms
                return formatMs (2.0 + p * 23.0, 1);
            if (idx == 3)   // Feedback -> -90..+90 %
                return juce::String (juce::roundToInt ((-0.9 + p * 1.8) * 100.0)) + "%";
            break;

        case FxType::PlateReverb:
            if (idx == 0)   // Predelay -> 0..100 ms
                return formatMs (p * 100.0, 1);
            if (idx == 1)   // Decay -> 0.1..4.0 s
                return formatSecs (0.1 + p * 3.9);
            if (idx == 2)   // Damping -> 500..12000 Hz (log)
                return hzReadoutFx (500.0 * std::pow (24.0, p));
            break;

        case FxType::VinylCompressor:
            if (idx == 3)   // Age -> 700..15000 Hz (log)
                return hzReadoutFx (700.0 * std::pow (15000.0 / 700.0, p));
            break;

        case FxType::Phaser:
            if (idx == 0)   // Rate -> 0.1..8 Hz (log)
                return juce::String (0.1 * std::pow (80.0, p), 2) + "Hz";   // "0.10Hz".."8.00Hz"
            if (idx == 2)   // Feedback -> -90..+90 %
                return juce::String (juce::roundToInt ((-0.9 + p * 1.8) * 100.0)) + "%";
            if (idx == 3)   // Center -> 200..2000 Hz (log)
                return hzReadoutFx (200.0 * std::pow (10.0, p));   // "200Hz".."2k0"
            break;

        // ---- FV-1 second wave readouts ----
        case FxType::Overdrive:
            if (idx == 0)   // Drive -> 1..16x (log)
                return juce::String (std::pow (16.0, p), 1) + "x";
            if (idx == 1)   // Bias -> -0.3..+0.3
                return juce::String ((p - 0.5) * 0.6, 2);
            if (idx == 2)   // Tone -> 700..15000 Hz (log)
                return hzReadoutFx (700.0 * std::pow (15000.0 / 700.0, p));
            if (idx == 3)   // Level -> 0..2
                return juce::String (p * 2.0, 2);
            break;

        case FxType::LutDistortion:
            if (idx == 0)   // Drive -> 1..8x
                return juce::String (1.0 + p * 7.0, 1) + "x";
            if (idx == 1)   // Shape -> 16 stepped names
            {
                static const char* const kShapes[] = {
                    "Clip", "Soft", "Tube", "Wrap", "OctUp", "Fuzz", "Square",
                    "Steps", "SFold", "Cheby2", "Cheby3", "Asym", "Mirror",
                    "HGate", "Crush4", "Sparse" };   // 16 entries, all <= 6 chars (cell budget)
                return kShapes[juce::jlimit (0, 15, juce::roundToInt (p * 16.0))];
            }
            if (idx == 3)   // Tone -> 700..15000 Hz (log)
                return hzReadoutFx (700.0 * std::pow (15000.0 / 700.0, p));
            break;

        case FxType::Compressor:
            if (idx == 1)   // Attack -> 0.5..50 ms (log)
                return formatMs (0.5 * std::pow (100.0, p), 1);
            if (idx == 2)   // Release -> 20..500 ms (log)
                return formatMs (20.0 * std::pow (25.0, p), 1);
            if (idx == 3)   // Level -> 0..2
                return juce::String (p * 2.0, 2);
            break;

        case FxType::Gate:
            if (idx == 0)   // Threshold -> 0..-3 dB (0 = off)
                return p < 0.005 ? "Off" : juce::String (juce::roundToInt (p * 100.0)) + "%";
            if (idx == 1)   // Attack -> 0.05..10 ms (log)
                return formatMs (0.05 * std::pow (200.0, p), 2);
            if (idx == 2)   // Hold -> 0..150 ms
                return formatMs (p * 150.0, 1);
            if (idx == 3)   // Release -> 5..500 ms (log)
                return formatMs (5.0 * std::pow (100.0, p), 1);
            break;

        case FxType::Chorus:
            if (idx == 0)   // Rate -> 0.1..8 Hz (log)
                return juce::String (0.1 * std::pow (80.0, p), 2) + "Hz";   // "0.10Hz".."8.00Hz"
            if (idx == 1)   // Depth -> 0..6 ms
                return formatMs (p * 6.0, 1);
            if (idx == 2)   // Center -> 5..25 ms
                return formatMs (5.0 + p * 20.0, 1);
            break;

        case FxType::Flanger:
            if (idx == 0)   // Rate -> 0.05..3 Hz (log)
                return juce::String (0.05 * std::pow (60.0, p), 2) + "Hz";   // "0.05Hz".."3.00Hz"
            if (idx == 1)   // Depth -> 0..4.5 ms
                return formatMs (p * 4.5, 1);
            if (idx == 2)   // Manual -> 0.15..6 ms
                return formatMs (0.15 + p * 5.85, 2);
            break;

        case FxType::Echo:
            if (idx == 0)   // Time -> 10..470 ms (log)
                return formatMs (10.0 * std::pow (47.0, p), 1);
            if (idx == 2)   // Tone -> 700..12000 Hz (log)
                return hzReadoutFx (700.0 * std::pow (12000.0 / 700.0, p));
            if (idx == 3)   // Spread -> R time 1..2x
                return juce::String (1.0 + p, 2) + "x";
            break;

        case FxType::Room:
        case FxType::Spring:
            if (idx == 0)   // Decay
                return formatSecs (0.1 + p * (t == FxType::Spring ? 3.9 : 2.9));
            if (idx == 1)   // Damp
                return hzReadoutFx (500.0 * std::pow (t == FxType::Spring ? 16.0 : 24.0, p));
            break;

        default:
            break;
    }
#pragma clang diagnostic pop

    // Default: dimensionless param -> 0..100%
    return juce::String (juce::roundToInt (p * 100.0)) + "%";
}

//==============================================================================
// Master-section readouts (hoisted from FxRoutingBar.cpp — see the header
// declaration comment). Compact, <=5 chars, no space, so they fit the 44px EQ
// dial above the painter's 9px floor; the Hz readout uses the synth's
// electronic-component k-notation ("1k5" for 1500 Hz). Bounds mirror
// FxChain.cpp:308-312 (low) and :319/:330 (mid/high).
juce::String fxEqLowToString (double v)
{
    const int iv = juce::roundToInt (v);
    if (iv <= 0) return "Off";
    const double t = static_cast<double> (iv - 1) / 126.0;
    const double hz = 20.0 * std::pow (1500.0 / 20.0, t);
    if (hz < 1000.0) return juce::String (juce::roundToInt (hz)) + "Hz";   // "20Hz".."999Hz"
    const int hundreds = juce::roundToInt (hz / 100.0);                   // 1500 -> 15
    return juce::String (hundreds / 10) + "k" + juce::String (hundreds % 10);   // "1k0".."1k5"
}

juce::String fxEqDbToString (double v)
{
    const int db = juce::roundToInt ((v - 64.0) / 64.0 * 12.0);
    return (db > 0 ? "+" : juce::String()) + juce::String (db) + "dB";     // "+6dB","0dB","-12dB"
}

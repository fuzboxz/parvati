// Copyright (c) 2026 805Labs Kft. / Hellcat.  See FxSlotHelp.h.
//
// Definitions of the per-FX-type module help. One sentence per ACTIVE
// generic param. The ranges mirror paramValueText (FxSlotLabels.cpp) and the
// knob laws in dsp/fx/FxTypes.h (namespace fxlaw), so the sentence, the knob
// readout and the DSP agree.
//
// These are at GLOBAL scope (not an anonymous namespace) so they have
// external linkage and link across translation units (the FxSlotLabels
// pattern). ASD-STE100: one sentence, short words, active voice, present
// tense.

#include "FxSlotHelp.h"

#include "FxSlotLabels.h"   // activeParamCount / paramLabel (label source)

//==============================================================================
// The one-sentence body for generic param @p idx (0-based) of effect type
// @p t. The label and the slot number are added by fxParamHelp(). Returns
// nullptr for an inactive param.
static const char* paramHelpSentence (FxType t, int idx) noexcept
{
    switch (t)
    {
        case FxType::PitchShifter:
            if (idx == 0) return "sets the pitch shift from -12 to +12 semitones";
            if (idx == 1) return "sets the grain size of the shifter";
            if (idx == 2) return "spreads the shifted voices across the stereo field";
            break;

        case FxType::Reverb:
            if (idx == 0) return "sets the delay before the reverb starts, 0 to 200 ms";
            if (idx == 1) return "sets the echo density of the reverb";
            if (idx == 2) return "sets the decay time of the reverb tank";
            if (idx == 3) return "sets the brightness of the reverb tail";
            if (idx == 4) return "removes low frequencies from the reverb tail";
            break;

        case FxType::LoopingDelay:
            if (idx == 0) return "sets the play position inside the captured loop";
            if (idx == 1) return "sets the length of the captured loop";
            if (idx == 2) return "sets the pitch shift from -24 to +24 semitones";
            if (idx == 3) return "holds the loop when the value passes half";
            break;

        case FxType::WSOLAStretch:
            if (idx == 0) return "sets the pitch shift from -24 to +24 semitones";
            if (idx == 1) return "sets the play position inside the buffer";
            if (idx == 2) return "sets the size of the stretch window";
            if (idx == 3) return "holds the buffer when the value passes half";
            if (idx == 4) return "sets the brightness of the stretched signal";
            break;

        case FxType::Spectral:
            if (idx == 0) return "sets the pitch shift from -24 to +24 semitones";
            if (idx == 1) return "sets the amount of spectral warp";
            if (idx == 2) return "sets the play position inside the spectrum";
            if (idx == 3) return "smears the spectrum over time";
            if (idx == 4) return "holds the spectrum when the value passes half";
            break;

        case FxType::Wavefolder:
            if (idx == 0) return "sets the input level into the wavefolder";
            if (idx == 1) return "sets the number of waveform folds";
            if (idx == 2) return "sets a direct voltage offset before the fold";
            if (idx == 3) return "sets the brightness after the fold";
            break;

        case FxType::FrequencyShifter:
            if (idx == 0) return "shifts all frequencies up or down";
            if (idx == 1) return "selects the shape of the shifter core";
            if (idx == 2) return "feeds the shifted signal back into the input";
            if (idx == 3) return "offsets the right channel against the left";
            break;

        case FxType::RingModulator:
            if (idx == 0) return "sets the carrier frequency from 20 Hz to 4 kHz";
            if (idx == 1) return "selects the carrier waveform";
            if (idx == 2) return "sets the input level into the ring modulator";
            break;

        case FxType::Resonator:
            if (idx == 0) return "sets the resonated pitch from C1 to C7";
            if (idx == 1) return "sets the ring time of the modes";
            if (idx == 2) return "sets the balance toward the higher modes";
            if (idx == 3) return "sets the strike position on the structure";
            if (idx == 4) return "selects the resonator structure";
            break;

        case FxType::ClockedDelay:
            if (idx == 0) return "selects the tempo division from 1/1 to 1/16";
            if (idx == 1) return "sets the repeat level, up to 95 percent";
            if (idx == 2) return "sets the tape wear of the repeats";
            if (idx == 3) return "reduces the bit depth of the repeats, 24 to 8 bit";
            break;

        case FxType::Ensemble:
            if (idx == 0) return "sets the modulation speed from 0.1 Hz to 8 Hz";
            if (idx == 1) return "sets the modulation depth up to 15 ms";
            if (idx == 2) return "sets the base delay from 2 ms to 25 ms";
            if (idx == 3) return "sets the loop gain from -90 to +90 percent";
            break;

        case FxType::PlateReverb:
            if (idx == 0) return "sets the delay before the plate starts, 0 to 100 ms";
            if (idx == 1) return "sets the plate decay from 0.1 s to 4 s";
            if (idx == 2) return "sets the damping from 500 Hz to 12 kHz";
            if (idx == 3) return "sets the modulation of the plate delays";
            break;

        case FxType::VinylCompressor:
            if (idx == 0) return "sets the amount of level compression";
            if (idx == 1) return "sets the wow and flutter of the disc";
            if (idx == 2) return "sets the crackle noise level";
            if (idx == 3) return "limits the bandwidth from 700 Hz to 15 kHz";
            break;

        case FxType::Phaser:
            if (idx == 0) return "sets the sweep speed from 0.1 Hz to 8 Hz";
            if (idx == 1) return "sets the depth of the notch sweep";
            if (idx == 2) return "sets the loop gain from -90 to +90 percent";
            if (idx == 3) return "sets the sweep centre from 200 Hz to 2 kHz";
            break;

        case FxType::Overdrive:
            if (idx == 0) return "sets the gain from 1x to 16x";
            if (idx == 1) return "sets a direct voltage offset from -0.3 to +0.3";
            if (idx == 2) return "sets the tone filter from 700 Hz to 15 kHz";
            if (idx == 3) return "sets the output level from 0 to 2";
            break;

        case FxType::LutDistortion:
            if (idx == 0) return "sets the gain from 1x to 8x";
            if (idx == 1) return "selects one of sixteen waveshapes";
            if (idx == 2) return "adds random movement to the selected shape";
            if (idx == 3) return "sets the tone filter from 700 Hz to 15 kHz";
            break;

        case FxType::Compressor:
            if (idx == 0) return "sets the amount of compression";
            if (idx == 1) return "sets the attack time from 0.5 ms to 50 ms";
            if (idx == 2) return "sets the release time from 20 ms to 500 ms";
            if (idx == 3) return "sets the output level from 0 to 2";
            break;

        case FxType::Gate:
            if (idx == 0) return "sets the level the gate needs to open";
            if (idx == 1) return "sets the attack time from 0.05 ms to 10 ms";
            if (idx == 2) return "sets the hold time up to 150 ms";
            if (idx == 3) return "sets the release time from 5 ms to 500 ms";
            break;

        case FxType::Chorus:
            if (idx == 0) return "sets the modulation speed from 0.1 Hz to 8 Hz";
            if (idx == 1) return "sets the modulation depth up to 6 ms";
            if (idx == 2) return "sets the base delay from 5 ms to 25 ms";
            if (idx == 3) return "sets the loop gain up to 50 percent";
            break;

        case FxType::JunoChorus:
            if (idx == 0) return "selects chorus I or chorus II";
            if (idx == 1) return "trims the mode rate from 0.5x to 2x";
            if (idx == 2) return "sets the sweep depth up to 200 percent of stock";
            if (idx == 3) return "sets the wet level of the chorus";
            break;

        case FxType::Flanger:
            if (idx == 0) return "sets the sweep speed from 0.05 Hz to 3 Hz";
            if (idx == 1) return "sets the sweep depth up to 4.5 ms";
            if (idx == 2) return "sets the base delay from 0.15 ms to 6 ms";
            if (idx == 3) return "sets the loop gain up to 92 percent";
            break;

        case FxType::Echo:
            if (idx == 0) return "sets the echo time from 10 ms to 470 ms";
            if (idx == 1) return "sets the repeat level; the maximum holds the loop";
            if (idx == 2) return "sets the damp filter from 700 Hz to 12 kHz";
            if (idx == 3) return "sets the right time from 1x to 2x of the left";
            break;

        case FxType::Room:
            if (idx == 0) return "sets the decay up to 3 s";
            if (idx == 1) return "sets the damping from 500 Hz to 12 kHz";
            if (idx == 2) return "sets the stereo width of the room";
            if (idx == 3) return "sets the brightness of the room";
            break;

        case FxType::Spring:
            if (idx == 0) return "sets the decay up to 4 s";
            if (idx == 1) return "sets the damping from 500 Hz to 8 kHz";
            if (idx == 2) return "sets the chirp amount of the spring";
            if (idx == 3) return "sets the stereo width of the spring";
            break;

        // Types without generic params keep the generic ParamHelp text.
        case FxType::Diffuser:
        case FxType::None:
        case FxType::Count:
            break;
    }
    return nullptr;
}

juce::String fxParamHelp (FxType t, int slot, int idx)
{
    // An inactive param or a param-less type returns empty text. The caller
    // then keeps the generic ParamHelp text for that knob.
    if (idx < 0 || idx >= activeParamCount (t))
        return {};

    const char* sentence = paramHelpSentence (t, idx);
    if (sentence == nullptr)
        return {};

    // "FX slot N <label>: <sentence>." — one sentence, the label included,
    // so the tooltip names the slot and the module parameter.
    return "FX slot " + juce::String (slot) + " " + juce::String (paramLabel (t, idx))
           + ": " + juce::String (sentence) + ".";
}

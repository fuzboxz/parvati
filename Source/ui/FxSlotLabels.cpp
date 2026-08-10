// Copyright (c) 2026 Jozsef Ottucsak / Parvati.  See FxSlotLabels.h.
//
// Definitions of the per-FX-type active-parameter count + semantic short labels.
// These are at GLOBAL scope (not an anonymous namespace) so they have external
// linkage and link across translation units (FxSlotCard.cpp for the live knob
// labels, FxMatrixView.cpp for the dynamic FX-mod-matrix destination labels).
// FxSlotCard.cpp previously held these in its anonymous namespace; they were
// hoisted out here to be shareable.

#include "FxSlotLabels.h"

//==============================================================================
// idx is 0..3 (param1..4). Inactive params (idx >= activeParamCount) are
// hidden on the slot card and omitted from the FX-mod dest combo.
int activeParamCount (FxType t) noexcept
{
    switch (t)
    {
        case FxType::Diffuser:     return 1;
        case FxType::PitchShifter: return 2;
        case FxType::CloudsReverb: return 4;
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
            if (idx == 0) return "Amount";
            break;
        case FxType::PitchShifter:
            if (idx == 0) return "Ratio";
            if (idx == 1) return "Size";
            break;
        case FxType::CloudsReverb:
            if (idx == 0) return "Amount";
            if (idx == 1) return "Time";
            if (idx == 2) return "Tone";
            if (idx == 3) return "Diffusion";
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
            if (idx == 2) return "Amount";
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

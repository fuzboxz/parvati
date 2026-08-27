// Copyright (c) 2026 805Labs Kft. / Hellcat.
//
// FormatHelpers — single source for the small readout formatters that several
// pages/formatting layers must agree on CHARACTER-FOR-CHARACTER (pinned by
// host_param_text_test / paramhelp_parity_test / hellcat_synth_paramtext_test
// / fx_param_coverage_test). Previously each site carried its own copy and
// they were one rounding tweak away from drifting (host text vs knob text).
//
// Current callers: ParameterLayout.cpp (host value strings),
// ui/SynthParamLabels.cpp (synth knob readouts), ui/ModMatrixView.cpp +
// ui/FxMatrixView.cpp (matrix amount labels), ui/FxSlotLabels.cpp (FX Hz
// readouts).

#pragma once

#include <juce_core/juce_core.h>   // juce::String, juce::roundToInt

// Bipolar ±63 scale -> "+100%" / "0%" / "-50%". The mod-matrix / FX-mod
// amount sliders and the engine use ±63 as full scale, so 63 maps to 100%.
// Negative values rounding to zero print as "0%" (no "-0%").
inline juce::String signedAmountPercent (double v)
{
    const int pct = juce::roundToInt (v * 100.0 / 63.0);
    return (pct > 0 ? "+" : juce::String()) + juce::String (pct) + "%";
}

// ---- Hz readouts ("electronic-component" style) -----------------------------
// Shared core: <1 kHz -> integer + "Hz" ("820Hz"); >=1 kHz -> k-notation
// rounded to the nearest 100 Hz with 'k' replacing the decimal point
// ("1k2".."9k9"). The two public variants differ only in documented edge
// policies, so a change to the shared rounding lands everywhere at once.

// Core. @p subHzTwoDecimals gives sub-1 Hz values two decimals ("0.42Hz");
// @p dropTenthAtTenK elides the k-tenth for hundreds >= 100 ("10k", "15k").
inline juce::String hzReadoutCore (double hz, bool subHzTwoDecimals, bool dropTenthAtTenK)
{
    if (subHzTwoDecimals && hz < 1.0)
        return juce::String (hz, 2) + "Hz";
    if (hz < 1000.0)
        return juce::String (juce::roundToInt (hz)) + "Hz";
    const int hundreds = juce::roundToInt (hz / 100.0);   // nearest 100 Hz
    if (dropTenthAtTenK && hundreds >= 100)
        return juce::String (hundreds / 10) + "k";
    return juce::String (hundreds / 10) + "k" + juce::String (hundreds % 10);
}

// Synth filter/LFO readout: sub-1 Hz keeps two decimals and the k-tenth is
// ALWAYS shown ("15k6", "16k0") — the filter cutoff is the only synth knob
// that reaches kHz; LFO rates top out ~980 Hz -> "NHz".
inline juce::String hzReadoutSynth (double hz)
{
    return hzReadoutCore (hz, true, false);
}

// FX-page readout: no sub-1-Hz branch (FX params never reach it) and >=10 kHz
// drops the tenth ("10k", "12k", "15k") to keep every form <= 4 chars.
inline juce::String hzReadoutFx (double hz)
{
    return hzReadoutCore (hz, false, true);
}

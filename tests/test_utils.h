// Shared parameter-set helpers for the unified test suite.
//
// The canonical "host changed this parameter" path: setValueNotifyingHost
// fires APVTS parameterChanged synchronously, which writes the patch byte
// into every voice (no message-thread pumping needed). These were lifted
// verbatim from the byte-identical file-local copies that ~20 test files
// carried; signatures and semantics are unchanged.
//
//   setInt    - juce::AudioParameterInt    (typed; skips non-int params)
//   setChoice - juce::AudioParameterChoice (typed; skips non-choice params)
//   setParam  - generic RangedAudioParameter path (works for any param type;
//               value is interpreted in the param's native range)
//
// Precedent: tests/mt_harness.h (shared test headers compile into every TU
// of parvati_unified_tests, hence `inline`).

#ifndef PARVATI_TEST_UTILS_H_
#define PARVATI_TEST_UTILS_H_

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"

// Typed: host-style write to an AudioParameterInt; silently no-ops if the id
// is not an int parameter (mirrors the original per-file copies).
inline void setInt (ParvatiAudioProcessor& proc, const char* id, int value)
{
    if (auto* param = proc.getApvts().getParameter (id))
        if (auto* ip = dynamic_cast<juce::AudioParameterInt*> (param))
            ip->setValueNotifyingHost (ip->convertTo0to1 (static_cast<float> (value)));
}

// Typed: host-style write to an AudioParameterChoice; silently no-ops if the
// id is not a choice parameter (mirrors the original per-file copies).
inline void setChoice (ParvatiAudioProcessor& proc, const char* id, int value)
{
    if (auto* param = proc.getApvts().getParameter (id))
        if (auto* cp = dynamic_cast<juce::AudioParameterChoice*> (param))
            cp->setValueNotifyingHost (cp->convertTo0to1 (static_cast<float> (value)));
}

// Generic: host-style write through the base RangedAudioParameter interface,
// so it drives ints, choices, floats alike via convertTo0to1.
inline void setParam (ParvatiAudioProcessor& proc, const char* id, int value)
{
    if (auto* p = proc.getApvts().getParameter (id))
        p->setValueNotifyingHost (p->convertTo0to1 (static_cast<float> (value)));
}

#endif  // PARVATI_TEST_UTILS_H_

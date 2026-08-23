// Shared parameter-set helpers + render/transport fixtures for the unified
// test suite.
//
// The canonical "host changed this parameter" path: setValueNotifyingHost
// fires APVTS parameterChanged synchronously, which writes the patch byte
// into every voice (no message-thread pumping needed). These were lifted
// verbatim from the byte-identical file-local copies that ~20 test files
// carried; signatures and semantics are unchanged.
//
//   setInt      - juce::AudioParameterInt    (typed; skips non-int params)
//   setChoice   - juce::AudioParameterChoice (typed; skips non-choice params)
//   setParam    - generic RangedAudioParameter path (works for any param type;
//                 value is interpreted in the param's native range)
//   FakePlayHead  - minimal host transport (fixed bpm + playing)
//   renderBlocks  - cleared-audio block pump with optional MIDI injection
//
// Precedent: tests/mt_harness.h (shared test headers compile into every TU
// of parvati_unified_tests, hence `inline`).
//
// FakePlayHead and renderBlocks replace the per-file copies that eleven
// tests carried. Semantics stay those of the originals: bpm and playing are
// public so a test can retime the transport between renders; renderBlocks
// clears the buffer before every block and adds @p inject (if any) at sample
// 0 of block @p injectBlock. The default block size is 256; the 512-based
// timing tests pass their own kBlock.

#ifndef PARVATI_TEST_UTILS_H_
#define PARVATI_TEST_UTILS_H_

#include <cstdlib>   // ::setenv (POSIX branch of setEnvVar)

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"

// True when the host reports at least one display. Headless Linux CI has no
// display server, so JUCE cannot create a desktop peer. Tests that need a
// real window call this first and print a skip note. macOS always reports a
// display, so the guard stays dormant there. (JUCE 9 exposes the check as
// Displays::getPrimaryDisplay; it returns nullptr with no connected screen.)
inline bool displayAvailable()
{
    return juce::Desktop::getInstance().getDisplays().getPrimaryDisplay() != nullptr;
}

// Sets a process environment variable on every platform. JUCE 9 exposes
// only a reader (SystemStats::getEnvironmentVariable), so the setter wraps
// the native calls: setenv on POSIX, _putenv_s under MSVC. An empty value
// removes the variable on POSIX and blanks it on Windows; no reader in the
// tree distinguishes the two states.
inline void setEnvVar (const char* key, const char* value)
{
#if defined (_WIN32)
    _putenv_s (key, value);
#else
    ::setenv (key, value, 1);
#endif
}

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

// Minimal host transport: fixed bpm + playing flag, time pinned to zero.
// The processor polls getPosition() once per block and reads bpm/playing
// only, so no further fields are modelled. Both members are public so a
// test can retime the transport between renders.
class FakePlayHead : public juce::AudioPlayHead
{
public:
    FakePlayHead (double initialBpm = 120.0, bool initiallyPlaying = true)
        : bpm (initialBpm), playing (initiallyPlaying) {}

    double bpm;
    bool playing;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm (bpm);
        info.setIsPlaying (playing);
        info.setTimeInSamples (static_cast<int64_t> (0));
        return info;
    }
};

// Render @p blocks of cleared stereo audio through @p proc. When @p inject
// is non-null, the message enters block @p injectBlock at sample 0. The
// buffer is allocated once and cleared before every block (the processBlock
// result is discarded). @p blockSamples defaults to 256; timing tests that
// prepared at 512 pass their own block size.
inline void renderBlocks (ParvatiAudioProcessor& proc, int blocks,
                          const juce::MidiMessage* inject = nullptr,
                          int blockSamples = 256, int injectBlock = 0)
{
    juce::AudioBuffer<float> buf (2, blockSamples);
    for (int b = 0; b < blocks; ++b)
    {
        juce::MidiBuffer midi;
        if (inject != nullptr && b == injectBlock)
            midi.addEvent (*inject, 0);
        buf.clear();
        proc.processBlock (buf, midi);
    }
}

#endif  // PARVATI_TEST_UTILS_H_

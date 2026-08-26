// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.
//
// Trivial SynthesiserSound: every AmbikaVoice can play any note on any channel.
// (Multi-timbral part/channel filtering is deferred — out of scope for the
//  single-engine port.)

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

class AmbikaSound : public juce::SynthesiserSound
{
public:
    AmbikaSound() = default;

    bool appliesToNote (int) override   { return true; }
    bool appliesToChannel (int) override { return true; }
};

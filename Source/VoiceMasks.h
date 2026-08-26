// Copyright (c) 2026 Jozsef Ottucsak / Hellcat.
//
// VoiceMasks — shared helpers over the 8-bit voicecard bitmasks (pure,
// dependency-free). The slots model derives a Part's voice count as
// popcount(mask), and the .MUL export solver counts solved cards the same
// way. One helper here keeps the engine and the solver from drifting.

#ifndef HELLCAT_VOICE_MASKS_H_
#define HELLCAT_VOICE_MASKS_H_

#include <cstdint>

namespace hellcat
{

// Count the set bits of an 8-bit voicecard bitmask.
inline int popcount8 (uint8_t x) noexcept
{
    int n = 0;
    while (x != 0) { n += x & 1; x >>= 1; }
    return n;
}

}  // namespace hellcat

#endif  // HELLCAT_VOICE_MASKS_H_

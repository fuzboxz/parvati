// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// RbjBiquad — the shared biquad kernel. Both biquad users in the FX tree
// (FxChain::EqBiquad master EQ and fv1::BiquadLP in the FV-1 rate bridge)
// implement the same Direct Form II Transposed sample loop. Two hand copies
// of a filter kernel drift apart silently, so the loop lives here once.
//
// DEPENDENCY-FREE: only <cmath> is included. No JUCE header, no project
// header. Both call sites sit in the real-time audio path.
//
// The two sites keep their own DESIGN math: the master EQ computes a
// high-pass / peaking / shelf set with per-coefficient division by a0, while
// BiquadLP computes a Butterworth low-pass with the same division shape but a
// DIFFERENT Q literal (0.70710678 vs the full-precision 0.7071067811865476 —
// distinct doubles by history). Forcing one design would change bits at one
// site; the designs stay local. The kernel below is the bit-identical part.

#pragma once

#include <cmath>

namespace parvati::dsp
{

// 2*pi as a full-precision double. Both former literals
// (6.28318530717958647692 in FxChain and 6.283185307179586 in Fv1Engine)
// parse to this same double, so adopting it changes no coefficient.
inline constexpr double kRbjTwoPi = 6.28318530717958647692;

// Direct Form II Transposed sample step (numerically well-behaved). The four
// statements are shared verbatim by both biquad sites. Denormal policy stays
// at the caller: the FV-1 site flushes z1/z2 after the call, the master EQ
// does not (see the call sites for each rationale).
inline float df2tProcessSample (float x, float b0, float b1, float b2,
                                float a1, float a2,
                                float& z1, float& z2) noexcept
{
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
}

} // namespace parvati::dsp

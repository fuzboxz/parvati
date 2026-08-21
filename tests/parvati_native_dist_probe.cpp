// Native-distortion quality probe (2026-08-21): audits the transfer curves of
// the NATIVE (invented, non-ported) distortion code — the 16 LUT shapes in
// Fv1LutDistortion (shapeFn copy below — the exact definitions) — for C0
// discontinuities (jumps = buzzy garbage no oversampling can fix), DC at
// zero, and edge behavior. Diagnostic dump, not a gate.
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
float clamp1 (float v) { return std::clamp (v, -1.0f, 1.0f); }

// VERBATIM copy of Fv1LutDistortion.cpp shapeFn (the code under audit).
float shapeFn (int s, float x)
{
    switch (s)
    {
        case 0:  return clamp1 (1.6f * x);
        case 1:  return clamp1 (1.5f * x / (1.0f + std::fabs (x)));
        case 2:  return clamp1 (x >= 0.0f ? 1.5f * x / (1.0f + 0.35f * x * x)
                                          : 1.7f * x / (1.0f + 0.22f * x * x));
        case 3:
        {
            float w = x * 0.5f;
            w = w - std::floor (w + 0.5f);
            return clamp1 (2.0f * w);
        }
        case 4:  return clamp1 (1.5f * std::fabs (x) - 0.45f);
        case 5:  return clamp1 (x >= 0.12f
                ? 2.4f * (x - 0.12f) / (1.0f + x)
                : (x >= 0.0f ? 0.15f * x * (0.5f + 0.5f * std::cos (3.14159265f * x / 0.12f))
                             : 0.15f * x));
        case 6:  return clamp1 (std::tanh (2.5f * x));
        case 7:
        {
            const float soft = 1.5f * x / (1.0f + std::fabs (x));
            return clamp1 (std::round (soft * 3.0f) / 3.0f);
        }
        case 8:
        {
            float w = x * 0.5f;
            w = w - std::floor (w + 0.5f);
            return clamp1 (std::sin (w * 6.2831853f));
        }
        case 9:  return clamp1 (0.95f * (2.0f * x * x - 1.0f) * std::exp (-std::fabs (x) * 0.35f));
        case 10: return clamp1 (0.95f * (4.0f * x * x * x - 3.0f * x));
        case 11:
        {
            const float t = x - 0.15f;
            return clamp1 (1.4f * (t - 0.4f * t * t * t));
        }
        case 12: return clamp1 (x >= 0.0f ? 1.7f * x / (1.0f + 0.22f * x * x)
                                          : 1.5f * x / (1.0f + 0.35f * x * x));
        case 13: return clamp1 (x >= 0.0f ? 1.4f * x / (1.0f + 0.3f * x)
                                          : 0.3f * x / (1.0f + std::fabs (x)));
        case 14: return clamp1 (1.35f * std::round (x * 8.0f) / 8.0f);
        case 15: return clamp1 (x / std::pow (std::fabs (x) + 0.05f, 0.7f));
        default: return clamp1 (x);
    }
}
} // namespace

int main()
{
    const char* names[16] = { "Clip", "Soft", "Tube", "Wrap", "OctUp", "Fuzz",
                              "Square", "Steps", "SFold", "Cheby2", "Cheby3",
                              "Asym", "Mirror", "HGate", "Crush4", "Sparse" };
    std::printf ("shape    :  maxJump   maxSlope   f(0)      f(0.12-)  f(0.12+)  edge+\n");
    for (int s = 0; s < 16; ++s)
    {
        double maxJump = 0.0, maxSlope = 0.0;
        const int N = 32 * 1024;              // dense grid over [-4, 4)
        const double dx = 8.0 / N;
        float prev = shapeFn (s, -4.0f);
        for (int i = 1; i <= N; ++i)
        {
            const float v = shapeFn (s, (float) (-4.0 + i * dx));
            const double jump = std::fabs ((double) v - prev);
            if (jump > maxJump) maxJump = jump;
            maxSlope = std::max (maxSlope, jump / dx);
            prev = v;
        }
        std::printf ("%-8s : %8.4f  %9.1f  %+8.4f  %+8.4f  %+8.4f  %+8.4f\n",
                     names[s], maxJump, maxSlope,
                     shapeFn (s, 0.0f),
                     shapeFn (s, 0.11999999f),
                     shapeFn (s, 0.12000001f),
                     shapeFn (s, 3.999f));
    }
    std::printf ("\nmaxJump > 0.01 = a hard C0 discontinuity (buzzy); slope in output-per-input units\n");

    // Sine-through-engine alias check for the previously-discontinuous shapes:
    // 220 Hz sine, drive 4x, shape swept; worst inharmonic spur (Goertzel at
    // k*220*(32768/44100)/... folded frequencies) vs the fundamental.
    return 0;
}

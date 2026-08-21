// Sine-vs-saw aliasing discriminator for the FV-1 nonlinear FX: drives
// Fv1Overdrive directly with a sine or saw at several frequencies, measures
// (a) the impulse census and (b) the inharmonic spectral spurs (Goertzel at
// the first few folded harmonic frequencies) vs the input fundamental.
// Build: parvati_fv1_alias_probe (EXCLUDE_FROM_ALL). Run: ./p

#include <cmath>
#include "unified_test_runner.h"
#include <cstdio>
#include <vector>

#include "dsp/fx/fv1/Fv1Overdrive.h"

using parvati::fv1::Fv1Overdrive;

namespace
{
// Simple rectangular-FFT-free spectral estimate: Goertzel power at freq f.
double goertzel (const std::vector<float>& x, double f, double sr)
{
    const double w = 2.0 * 3.141592653589793 * f / sr;
    const double cw = std::cos (w), coeff = 2.0 * cw;
    double s1 = 0.0, s2 = 0.0;
    for (float v : x)
    {
        const double s0 = v + coeff * s1 - s2;
        s2 = s1; s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

struct Census { int count; double worst; };

Census census (const std::vector<float>& x)
{
    const int n = (int) x.size ();
    std::vector<float> d (n, 0.f);
    for (int i = 1; i < n; ++i) d[(size_t) i] = std::fabs (x[(size_t) i] - x[(size_t) i - 1]);
    Census c { 0, 0.0 };
    for (int i = 65; i < n; ++i)
    {
        float w[64];
        for (int k = 0; k < 64; ++k) w[k] = d[(size_t) (i - 64 + k)];
        std::sort (w, w + 64);
        if (d[(size_t) i] > 8.f * w[60] && d[(size_t) i] > 0.004f)
        {
            ++c.count;
            c.worst = std::fmax (c.worst, d[(size_t) i]);
        }
    }
    return c;
}

void run (bool saw, double freq, double driveParam, bool print)
{
    Fv1Overdrive fx;                       // type 16
    const double sr = 48000.0;
    fx.prepare (sr, 512);
    float p[5] = { (float) driveParam, 0.5f, 0.5f, 0.5f, 0.0f };
    fx.setParams (p);
    fx.reset();

    const int warm = (int) (0.5 * sr);
    const int len  = (int) (1.0 * sr);
    std::vector<float> L (512), R (512);
    std::vector<float> out;
    double t = 0.0;
    const double dt = 1.0 / sr;
    for (int done = 0; done < warm + len; done += 512)
    {
        for (int i = 0; i < 512; ++i, t += dt)
        {
            const double ph = std::fmod (t * freq, 1.0);
            double v;
            if (saw) v = 2.0 * ph - 1.0;             // naive saw (with wrap)
            else     v = std::sin (2.0 * 3.141592653589793 * t * freq);
            L[(size_t) i] = R[(size_t) i] = (float) (0.5 * v);
        }
        fx.process (L.data(), R.data(), 512);
        if (done >= warm)
            out.insert (out.end(), L.begin(), L.end ());
    }
    const Census c = census (out);
    const double fund = goertzel (out, freq, sr);
    // First few aliased-harmonic candidates for the 32768 internal rate: a
    // harmonic m folds to |m*f - k*32768| (host-rate equivalent of the fold).
    // Report the strongest of a set of inharmonic probes vs the fundamental.
    double worstSpurRatio = 0.0; double worstSpurHz = 0.0;
    for (int m = 2; m <= 96; ++m)
    {
        const double fh = m * freq;
        // fold into [0, 16384] (the internal-rate band), triangle-fold mod 32768
        double r = std::fmod (fh, 32768.0);
        const double folded = (r > 16384.0) ? (32768.0 - r) : r;
        if (folded <= 20.0 || folded > 16000.0) continue;
        // skip near-harmonic folded freqs (within 3 Hz of an integer multiple of freq)
        bool nearHarmonic = false;
        for (int k = 1; k <= 200; ++k)
            if (std::fabs (folded - k * freq) < 3.0) { nearHarmonic = true; break; }
        if (nearHarmonic) continue;
        const double g = goertzel (out, folded, sr);
        const double ratio = g / (fund > 0.0 ? fund : 1e-30);
        if (ratio > worstSpurRatio) { worstSpurRatio = ratio; worstSpurHz = folded; }
    }
    if (print)
        std::printf ("  %-4s %6.1f Hz drive=%.2f: impulses=%4d worst=%.4f | worst inharmonic spur %.0f Hz at %.1f dB below fundamental\n",
                     saw ? "saw" : "sine", freq, driveParam, c.count, c.worst,
                     worstSpurHz, -10.0 * std::log10 (worstSpurRatio + 1e-30));
}
} // namespace

TEST(parvati_fv1_alias_probe)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    std::printf ("=== Fv1Overdrive aliasing probe (direct, 48 kHz host, 0.5 amp) ===\n");
    run (false, 220.0, 0.5f, true);
    run (true,  220.0, 0.5f, true);
    run (false, 1000.0, 0.5f, true);
    run (true,  1000.0, 0.5f, true);
    run (false, 3000.0, 0.5f, true);
    run (false, 220.0, 1.0f, true);
    run (true,  220.0, 1.0f, true);
    run (false, 220.0, 0.0f, true);
    run (true,  220.0, 0.0f, true);
    return true;
}

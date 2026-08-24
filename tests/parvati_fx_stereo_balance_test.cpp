// Stereo-balance investigation: (1) parallel FX routing (true-parallel vs L/R
// split) and (2) whether the Spectral effect (Clouds phase vocoder) produces a
// SYSTEMATIC left/right energy imbalance.
//
// Method: feed a MONO-CORRELATED stereo signal (inL == inR) through an FxChain
// with one slot set fully wet (drywet=1.0), let the STFT pipeline settle
// (>=1 s warmup), then measure the L vs R RMS energy over a long window (~3 s).
// With identical input on both channels, ANY L/R imbalance must come from the
// effect itself. We sweep the 4 Spectral params (pitch/warp/position/blur) and
// compare against two control effects:
//   * Resonator: known odd/even L/R split (position~0.5 -> R/even vanishes).
//   * Diffuser: expected symmetric.
// We also feed UNCORRELATED stereo noise (inL != inR) to check whether Spectral
// collapses/narrows a genuine stereo image.
//
// Run: ./build_unified/parvati_unified_tests parvati_fx_stereo_balance_test

#include <cmath>
#include "unified_test_runner.h"
#include <cstdint>
#include <cstdio>
#include <vector>

#include "dsp/fx/FxChain.h"
#include "stmlib/utils/random.h"   // to reseed the shared RNG (multi-seed blur probe)

namespace
{
constexpr int    kBlock      = 256;
constexpr double kRate       = 48000.0;

// ---- Deterministic pink-ish noise (Paul Kellet filter) ----
// Broadband, low-heavy content ideal for RMS-energy balance measurement. Fully
// deterministic (fixed LCG seed) so every run / param setting sees the SAME
// input -> a fair, reproducible comparison.
struct PinkGen
{
    uint32_t s;
    float b0 = 0, b1 = 0, b2 = 0;
    explicit PinkGen (uint32_t seed) : s (seed) {}
    float next()
    {
        s = s * 1664525u + 1013904223u;
        const float white = (float) ((int32_t) s >> 16) / 32768.0f;   // -1..1
        b0 = 0.99765f * b0 + white * 0.0990460f;
        b1 = 0.96300f * b1 + white * 0.2965164f;
        b2 = 0.57000f * b2 + white * 1.0526913f;
        return (b0 + b1 + b2 + white * 0.1848f) * 0.18f;   // ~0.5 peak pink-ish
    }
};

// Pre-render a flat noise buffer of `n` samples from a seeded pink generator.
std::vector<float> makePink (int n, uint32_t seed)
{
    std::vector<float> v (static_cast<size_t> (n));
    PinkGen g (seed);
    for (int i = 0; i < n; ++i)
        v[static_cast<size_t> (i)] = g.next();
    return v;
}

struct BalanceResult
{
    bool   finite    = true;
    double rmsL      = 0.0;
    double rmsR      = 0.0;
    double ratio     = 0.0;   // rmsR / rmsL
    double ratioDb   = 0.0;   // 20*log10(ratio)  (R relative to L; - = R quieter)
    double maxAbsL   = 0.0;
    double maxAbsR   = 0.0;
};

// Run `chain` over `totalSamples` drawn (L==noiseL[idx], R==noiseR[idx]); discard
// the first `warmupSamples`; measure RMS + max of the remainder. noiseL/noiseR
// are the (possibly identical) per-channel stimulus.
BalanceResult measureBalance (FxChain& chain,
                              const std::vector<float>& noiseL,
                              const std::vector<float>& noiseR,
                              int totalSamples, int warmupSamples)
{
    BalanceResult r;
    std::vector<float> inL (kBlock), inR (kBlock), outL (kBlock), outR (kBlock);
    double sumL = 0.0, sumR = 0.0;
    long count = 0;

    for (int off = 0; off + kBlock <= totalSamples; off += kBlock)
    {
        for (int i = 0; i < kBlock; ++i)
        {
            inL[static_cast<size_t> (i)] = noiseL[static_cast<size_t> (off + i)];
            inR[static_cast<size_t> (i)] = noiseR[static_cast<size_t> (off + i)];
        }
        chain.process (inL.data(), inR.data(), outL.data(), outR.data(), kBlock);

        if (off < warmupSamples)
            continue;   // warmup: pipeline settle, not measured

        for (int i = 0; i < kBlock; ++i)
        {
            const float l = outL[static_cast<size_t> (i)];
            const float rr = outR[static_cast<size_t> (i)];
            if (! std::isfinite (l) || ! std::isfinite (rr))
                r.finite = false;
            sumL += (double) l * (double) l;
            sumR += (double) rr * (double) rr;
            r.maxAbsL = std::fmax (r.maxAbsL, std::fabs ((double) l));
            r.maxAbsR = std::fmax (r.maxAbsR, std::fabs ((double) rr));
            ++count;
        }
    }

    r.rmsL = std::sqrt (sumL / std::max (1L, count));
    r.rmsR = std::sqrt (sumR / std::max (1L, count));
    r.ratio = (r.rmsL > 1.0e-12) ? r.rmsR / r.rmsL : 0.0;
    r.ratioDb = (r.ratio > 0.0) ? 20.0 * std::log10 (r.ratio) : -999.0;
    return r;
}

// Configure a single-slot chain, fully wet, with the given type + params.
void configureChain (FxChain& chain, FxType type, const float param[4])
{
    chain.prepare (kRate, kBlock);
    chain.setTopology (FxTopology::Series);
    chain.setOrder ({ 0, 1, 2 });
    chain.setSlotType (0, type);
    chain.setSlotEnabled (0, true);
    chain.setSlotDryWet (0, 1.0f);          // fully wet
    chain.setSlotParam (0, 0, param[0]);
    chain.setSlotParam (0, 1, param[1]);
    chain.setSlotParam (0, 2, param[2]);
    chain.setSlotParam (0, 3, param[3]);
    chain.setMasterMix (1.0f);              // no-op
}

// Pretty-print one measurement row.
void printRow (const char* label, const float param[4], const BalanceResult& r)
{
    std::printf ("  %-26s p=[%.2f,%.2f,%.2f,%.2f]  rmsL=%.5f rmsR=%.5f  "
                 "R/L=%.4f (%+.2fdB)%s\n",
                 label, param[0], param[1], param[2], param[3],
                 r.rmsL, r.rmsR, r.ratio, r.ratioDb,
                 r.finite ? "" : "  [NON-FINITE]");
}

// One mono-correlated measurement for a given effect+params, using a SHARED
// stimulus buffer (so every config sees identical input).
BalanceResult measureMono (FxType type, const float param[4],
                           const std::vector<float>& stim,
                           const char* label)
{
    FxChain chain;
    configureChain (chain, type, param);
    BalanceResult r = measureBalance (chain, stim, stim,
                                      static_cast<int> (stim.size()),
                                      static_cast<int> (0.5 * kRate));   // 0.5s warmup
    printRow (label, param, r);
    return r;
}
}  // namespace

TEST(parvati_fx_stereo_balance_test)
{
    constexpr int kTotalSeconds   = 3;   // measured window
    constexpr int kWarmupSeconds  = 1;   // settle (warmup is part of the buffer prefix)
    const int totalSamples = (kWarmupSeconds + kTotalSeconds) * static_cast<int> (kRate);

    // Shared mono stimulus (pink-ish noise) — identical on L and R for the
    // mono-correlated case. Two INDEPENDENT pink streams for the uncorrelated
    // case (different seeds).
    const std::vector<float> mono = makePink (totalSamples, 0x12345678u);
    const std::vector<float> indepL = mono;
    const std::vector<float> indepR = makePink (totalSamples, 0x9abcdef0u);

    std::printf ("=== Parvati FX stereo-balance investigation ===\n");
    std::printf ("rate=%.0f block=%d  warmup=%.1fs  measure=%.1fs  "
                 "(mono-correlated unless noted)\n\n",
                 kRate, kBlock,
                 (double) kWarmupSeconds, (double) kTotalSeconds);

    int worstSpectralFailures = 0;

    // --------------------------------------------------------------------
    // PART A: Spectral (phase vocoder) mono-correlated param sweep.
    // --------------------------------------------------------------------
    std::printf ("--- SPECTRAL (mono-correlated, inL==inR) ---\n");
    {
        struct Cfg { float p[4]; const char* name; };
        const Cfg cfgs[] = {
            // vary pitch (others default 0.5)
            {{0.00f,0.50f,0.50f,0.50f}, "pitch sweep:0.00"},
            {{0.25f,0.50f,0.50f,0.50f}, "pitch sweep:0.25"},
            {{0.50f,0.50f,0.50f,0.50f}, "pitch sweep:0.50"},
            {{0.75f,0.50f,0.50f,0.50f}, "pitch sweep:0.75"},
            {{1.00f,0.50f,0.50f,0.50f}, "pitch sweep:1.00"},
            // vary warp
            {{0.50f,0.00f,0.50f,0.50f}, "warp sweep :0.00"},
            {{0.50f,0.25f,0.50f,0.50f}, "warp sweep :0.25"},
            {{0.50f,0.75f,0.50f,0.50f}, "warp sweep :0.75"},
            {{0.50f,1.00f,0.50f,0.50f}, "warp sweep :1.00"},
            // vary position
            {{0.50f,0.50f,0.00f,0.50f}, "pos sweep  :0.00"},
            {{0.50f,0.50f,0.25f,0.50f}, "pos sweep  :0.25"},
            {{0.50f,0.50f,0.75f,0.50f}, "pos sweep  :0.75"},
            {{0.50f,0.50f,1.00f,0.50f}, "pos sweep  :1.00"},
            // vary blur (refresh_rate -> probabilistic StoreMagnitudes when <0.5)
            {{0.50f,0.50f,0.50f,0.00f}, "blur sweep :0.00"},
            {{0.50f,0.50f,0.50f,0.25f}, "blur sweep :0.25"},
            {{0.50f,0.50f,0.50f,0.75f}, "blur sweep :0.75"},
            {{0.50f,0.50f,0.50f,1.00f}, "blur sweep :1.00"},
            // combos (pitch away from 0.5)
            {{0.30f,0.70f,0.40f,0.20f}, "combo A"},
            {{0.70f,0.30f,0.60f,0.80f}, "combo B"},
            {{0.10f,0.90f,0.20f,0.10f}, "combo C"},
            {{0.90f,0.10f,0.80f,0.90f}, "combo D"},
        };
        double maxAbsDb = 0.0;
        for (const auto& c : cfgs)
        {
            const BalanceResult r = measureMono (FxType::Spectral, c.p, mono, c.name);
            if (! r.finite) ++worstSpectralFailures;
            maxAbsDb = std::fmax (maxAbsDb, std::fabs (r.ratioDb));
        }
        std::printf ("  >> Spectral mono: max |R/L imbalance| over all configs = %.2f dB\n\n",
                     maxAbsDb);
    }

    // --------------------------------------------------------------------
    // PART A2: is the blur<0.5 imbalance SYSTEMATIC (always one channel) or
    // STOCHASTIC (RNG-draw dependent, symmetric in expectation)? Re-run the
    // blur=0.25 measurement under many different RNG seeds. If the imbalance is
    // a genuine channel bug, every seed lands on the SAME side; if it is the
    // probabilistic StoreMagnitudes texture freeze, the sign/magnitude scatter.
    // --------------------------------------------------------------------
    std::printf ("--- SPECTRAL blur=0.25: multi-seed RNG probe (mono) ---\n");
    {
        float pBlur[4] = { 0.50f, 0.50f, 0.50f, 0.25f };
        const uint32_t seeds[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        int posCount = 0, negCount = 0;
        double minDb = 1e9, maxDb = -1e9;
        for (uint32_t sd : seeds)
        {
            stmlib::Random::Seed (sd);   // the phase vocoder consumes this stream
            FxChain chain;
            configureChain (chain, FxType::Spectral, pBlur);
            const BalanceResult r = measureBalance (chain, mono, mono,
                                                     static_cast<int> (mono.size()),
                                                     static_cast<int> (0.5 * kRate));
            std::printf ("  seed=%2u  rmsL=%.5f rmsR=%.5f  R/L=%.4f (%+.2fdB)\n",
                         sd, r.rmsL, r.rmsR, r.ratio, r.ratioDb);
            if (r.ratioDb >= 0.0) ++posCount; else ++negCount;
            minDb = std::fmin (minDb, r.ratioDb);
            maxDb = std::fmax (maxDb, r.ratioDb);
        }
        std::printf ("  >> seeds on +side:%d  -side:%d   range [%.2f .. %.2f] dB\n",
                     posCount, negCount, minDb, maxDb);
        std::printf ("  >> If both signs appear, the blur<0.5 imbalance is STOCHASTIC\n"
                     "     (texture-freeze variance), NOT a systematic channel bias.\n\n");
    }

    // --------------------------------------------------------------------
    // PART B: control effects (Resonator known-split, Diffuser symmetric).
    // --------------------------------------------------------------------
    std::printf ("--- CONTROL effects (mono-correlated) ---\n");
    {
        float pDiff[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        const BalanceResult rDiff = measureMono (FxType::Diffuser, pDiff, mono, "Diffuser");

        // Resonator: position 0.25 (both odd+even active) vs 0.5 (R/even null).
        float pRes25[4] = { 0.5f, 0.5f, 0.5f, 0.25f };
        const BalanceResult rRes25 = measureMono (FxType::Resonator, pRes25, mono, "Resonator pos0.25");
        float pRes50[4] = { 0.5f, 0.5f, 0.5f, 0.50f };
        const BalanceResult rRes50 = measureMono (FxType::Resonator, pRes50, mono, "Resonator pos0.50");
        float pRes75[4] = { 0.5f, 0.5f, 0.5f, 0.75f };
        const BalanceResult rRes75 = measureMono (FxType::Resonator, pRes75, mono, "Resonator pos0.75");

        std::printf ("  >> Diffuser expected ~symmetric; Resonator pos0.50 expected R/even -> null.\n\n");
    }

    // --------------------------------------------------------------------
    // PART C: Spectral with UNCORRELATED stereo input (inL != inR) — does it
    // collapse/narrow a genuine stereo image?
    // --------------------------------------------------------------------
    std::printf ("--- SPECTRAL (UN-correlated stereo, inL!=inR) ---\n");
    {
        // Reference: the unprocessed uncorrelated stereo input ratio.
        double sumL = 0.0, sumR = 0.0; long cnt = 0;
        const int warmup = static_cast<int> (0.5 * kRate);
        for (int i = warmup; i < totalSamples; ++i)
        {
            sumL += (double) indepL[static_cast<size_t> (i)] * (double) indepL[static_cast<size_t> (i)];
            sumR += (double) indepR[static_cast<size_t> (i)] * (double) indepR[static_cast<size_t> (i)];
            ++cnt;
        }
        const double inRL = std::sqrt (sumR / std::max (1L, cnt)) / std::sqrt (sumL / std::max (1L, cnt));
        std::printf ("  input (uncorrelated) R/L = %.4f (%.2f dB)\n",
                     inRL, 20.0 * std::log10 (inRL));

        struct Cfg { float p[4]; const char* name; };
        const Cfg cfgs[] = {
            {{0.50f,0.50f,0.50f,0.50f}, "spectral default"},
            {{0.30f,0.50f,0.50f,0.50f}, "spectral pitch0.30"},
            {{0.50f,0.20f,0.50f,0.50f}, "spectral warp0.20"},
            {{0.50f,0.50f,0.50f,0.25f}, "spectral blur0.25"},
        };
        for (const auto& c : cfgs)
        {
            FxChain chain;
            configureChain (chain, FxType::Spectral, c.p);
            const BalanceResult r = measureBalance (chain, indepL, indepR,
                                                    totalSamples, warmup);
            std::printf ("  %-26s p=[%.2f,%.2f,%.2f,%.2f]  rmsL=%.5f rmsR=%.5f  "
                         "R/L=%.4f (%+.2fdB)%s\n",
                         c.name, c.p[0], c.p[1], c.p[2], c.p[3],
                         r.rmsL, r.rmsR, r.ratio, r.ratioDb,
                         r.finite ? "" : "  [NON-FINITE]");
            if (! r.finite) ++worstSpectralFailures;
        }
        std::printf ("  >> If Spectral preserves the input R/L, it does NOT collapse the image.\n\n");
    }

    // --------------------------------------------------------------------
    // PART D: code-evidence echo — renderParallel is TRUE parallel (each slot
    // gets full L AND R), not an L/R split. (Verified by reading FxChain.cpp;
    // echoed here as a self-contained assertion summary.)
    // --------------------------------------------------------------------
    std::printf ("--- PARALLEL ROUTING verdict (from FxChain::renderParallel) ---\n");
    std::printf ("  Each parallel slot copies BOTH inL and inR into its own wet buffer\n");
    std::printf ("  and processes the full stereo pair; the wets are summed. -> TRUE parallel.\n\n");

    std::printf ("=== END === (%d non-finite result%s)\n",
                 worstSpectralFailures, worstSpectralFailures == 1 ? "" : "s");
    return worstSpectralFailures == 0;
}

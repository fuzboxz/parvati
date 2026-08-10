// Diagnostic: low-level RHYTHMIC POPPING when the Diffuser FX is enabled (static,
// NO modulation). Measures spike rate / period / autocorrelation and isolates the
// cause via sub-chunked-vs-single-block and slot-vs-direct renderings.
//
// Diagnosis only — no production-DSP change. Reuses the FxChain/FxProcessor APIs.
//
// Build: linked as parvati_fx_diffuser_diag_test (see CMakeLists).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "dsp/fx/FxChain.h"
#include "dsp/fx/FxProcessors.h"
#include "dsp/fx/FxTypes.h"

namespace
{
constexpr double kSr = 48000.0;

enum class Input { Silence, Tone220, Noise };

void fillInput (std::vector<float>& d, Input in)
{
    switch (in)
    {
        case Input::Silence: std::fill (d.begin(), d.end(), 0.0f); break;
        case Input::Tone220:
            for (size_t i = 0; i < d.size(); ++i)
                d[i] = static_cast<float> (0.5 * std::sin (2.0 * 3.14159265 * 220.0 * static_cast<double> (i) / kSr));
            break;
        case Input::Noise:
        {
            unsigned int state = 12345u;
            for (auto& v : d)
            {
                state = state * 1664525u + 1013904223u;
                v = (static_cast<float> (state >> 8) / static_cast<float> (0x00FFFFFF) - 0.5f);
            }
            break;
        }
    }
}

const char* inName (Input in)
{
    return in == Input::Silence ? "silence" : in == Input::Tone220 ? "220Hz" : "noise";
}

// Isolated-spike detector: a sample where |Δout[i]| > k * max(|Δout[i-1]|,|Δout[i+1]|).
// Catches impulsive discontinuities regardless of steady signal level.
std::vector<int> detectSpikes (const std::vector<float>& out, int start, int end, double k = 4.0)
{
    std::vector<int> spikes;
    for (int i = start + 1; i < end - 1; ++i)
    {
        const float d  = std::fabs (out[static_cast<size_t> (i)] - out[static_cast<size_t> (i - 1)]);
        const float dn = std::fabs (out[static_cast<size_t> (i + 1)] - out[static_cast<size_t> (i)]);
        const float neigh = std::max (dn, std::fabs (out[static_cast<size_t> (i - 1)] - out[static_cast<size_t> (i - 2)]));
        if (d > k * neigh && d > 1e-4f)
            spikes.push_back (i);
    }
    return spikes;
}

// Dominant inter-spike interval (period in samples) via histogram.
int dominantPeriod (const std::vector<int>& spikes, int maxPeriod = 8192)
{
    if (spikes.size() < 4)
        return 0;
    std::vector<int> hist (static_cast<size_t> (maxPeriod), 0);
    for (size_t i = 1; i < spikes.size(); ++i)
    {
        const int iv = spikes[i] - spikes[i - 1];
        if (iv > 0 && iv < maxPeriod)
            hist[static_cast<size_t> (iv)]++;
    }
    const auto it = std::max_element (hist.begin(), hist.end());
    return *it >= 3 ? static_cast<int> (it - hist.begin()) : 0;
}

// Autocorrelation peak of |Δout| over a lag range (excludes lag 0).
int autocorrPeak (const std::vector<float>& out, int start, int end, int maxLag = 4096)
{
    std::vector<float> d (static_cast<size_t> (end - start));
    for (int i = 1; i < end - start; ++i)
        d[static_cast<size_t> (i)] = std::fabs (out[static_cast<size_t> (start + i)] - out[static_cast<size_t> (start + i - 1)]);
    double best = 0.0;
    int bestLag = 0;
    for (int lag = 8; lag < maxLag; ++lag)
    {
        double s = 0.0;
        for (int i = lag; i < static_cast<int> (d.size()); ++i)
            s += d[static_cast<size_t> (i)] * d[static_cast<size_t> (i - lag)];
        if (s > best) { best = s; bestLag = lag; }
    }
    return bestLag;
}

struct Result
{
    int spikeCount;
    int periodSpikes;   // samples
    int periodAuto;     // samples
    double rateHz;      // from spike period
};

Result measure (const std::vector<float>& out, int totalSamples)
{
    const int start = 2048;   // warmup (let diffuser ring up)
    const int end   = totalSamples - 64;
    const auto spikes = detectSpikes (out, start, end);
    Result r {};
    r.spikeCount = static_cast<int> (spikes.size());
    r.periodSpikes = dominantPeriod (spikes);
    r.periodAuto = autocorrPeak (out, start, end);
    r.rateHz = r.periodSpikes > 0 ? kSr / r.periodSpikes : 0.0;
    return r;
}

// Render via FxChain slot for ANY FX type. chunk = sub-chunk size.
std::vector<float> renderSlotType (FxType type, Input in, int chunk, int blocks)
{
    const int N = chunk * blocks;
    std::vector<float> inL (static_cast<size_t> (N)), inR (static_cast<size_t> (N));
    std::vector<float> outL (static_cast<size_t> (N)), outR (static_cast<size_t> (N));
    fillInput (inL, in);
    inR = inL;

    FxChain chain;
    chain.prepare (kSr, 256);
    chain.setTopology (FxTopology::Series);
    chain.setSlotType (0, type);
    chain.setSlotEnabled (0, true);
    chain.setSlotDryWet (0, 1.0f);
    for (int k = 0; k < kNumFxSlotParams; ++k)
        chain.setSlotParam (0, k, 0.5f);

    for (int s = 0; s < blocks; ++s)
    {
        const int off = s * chunk;
        chain.process (inL.data() + off, inR.data() + off,
                       outL.data() + off, outR.data() + off, chunk);
    }
    return outL;
}

// Render via FxChain slot (includes slot dry/wet + wetFade). chunk = sub-chunk size.
std::vector<float> renderSlot (Input in, int chunk, int blocks)
{
    return renderSlotType (FxType::Diffuser, in, chunk, blocks);
}

// Render via FxDiffuser DIRECTLY (no slot dry/wet / wetFade). chunk = sub-chunk size.
std::vector<float> renderDirect (Input in, int chunk, int blocks)
{
    const int N = chunk * blocks;
    std::vector<float> inL (static_cast<size_t> (N)), inR (static_cast<size_t> (N));
    fillInput (inL, in);
    inR = inL;

    auto fx = createFxProcessor (FxType::Diffuser);
    fx->prepare (kSr, 256);
    float p[4] = { 0.5f, 0.0f, 0.0f, 0.0f };
    fx->setParams (p);

    for (int s = 0; s < blocks; ++s)
    {
        const int off = s * chunk;
        fx->process (inL.data() + off, inR.data() + off, chunk);
    }
    return inL;   // in-place
}

void printResult (const char* label, const std::vector<float>& out, int totalSamples)
{
    const Result r = measure (out, totalSamples);
    std::printf ("  %-34s spikes=%-5d period(spikes)=%-5d (%6.1f Hz)  period(auto)=%-5d (%6.1f Hz)\n",
                 label, r.spikeCount, r.periodSpikes, r.rateHz, r.periodAuto,
                 r.periodAuto > 0 ? kSr / r.periodAuto : 0.0);
}

void printBoundaryTable()
{
    const double ratio = 32000.0 / kSr;
    const int wrapInternal = 126 + 180 + 269 + 444 + 151 + 205 + 245 + 405; // 2025
    const int wrapHost = static_cast<int> (wrapInternal / ratio);            // internal->host samples
    std::printf ("\n  Candidate boundary periods (host samples @48k):\n");
    std::printf ("    sub-chunk (49)            : %5d samples  %7.1f Hz\n", 49, kSr / 49.0);
    std::printf ("    host-block (256)          : %5d samples  %7.1f Hz\n", 256, kSr / 256.0);
    std::printf ("    bridge m-alt (32->33)     : variable, ~sub-chunk\n");
    std::printf ("    diffuser buf wrap (2025 internal): %5d host samples  %7.1f Hz\n",
                 wrapHost, kSr / static_cast<double> (wrapHost));
}

}  // namespace

int main()
{
    std::printf ("=== Diffuser rhythmic-pop diagnostic (static, NO modulation) ===\n");

    const int subChunk = 49;
    const int blocksSub = 600;     // ~0.6 s sub-chunked
    const int singleBlock = 256;
    const int blocksSingle = 115;  // ~0.6 s single-block

    printBoundaryTable();

    std::printf ("\n-- FxChain SLOT render (includes dry/wet + wetFade) --\n");
    printResult ("slot, sub-chunk(49), 220Hz", renderSlot (Input::Tone220, subChunk, blocksSub), subChunk * blocksSub);
    printResult ("slot, single(256),  220Hz", renderSlot (Input::Tone220, singleBlock, blocksSingle), singleBlock * blocksSingle);
    printResult ("slot, sub-chunk(49), silence", renderSlot (Input::Silence, subChunk, blocksSub), subChunk * blocksSub);
    printResult ("slot, sub-chunk(49), noise", renderSlot (Input::Noise, subChunk, blocksSub), subChunk * blocksSub);

    std::printf ("\n-- FxDiffuser DIRECT render (no slot dry/wet / wetFade) --\n");
    printResult ("direct, sub-chunk(49), 220Hz", renderDirect (Input::Tone220, subChunk, blocksSub), subChunk * blocksSub);
    printResult ("direct, single(256),  220Hz", renderDirect (Input::Tone220, singleBlock, blocksSingle), singleBlock * blocksSingle);
    printResult ("direct, sub-chunk(49), silence", renderDirect (Input::Silence, subChunk, blocksSub), subChunk * blocksSub);

    std::printf ("\n-- Baseline: slot DISABLED (dry pass-through), 220Hz --\n");
    {
        const int N = subChunk * blocksSub;
        std::vector<float> inL (static_cast<size_t> (N)), inR (static_cast<size_t> (N));
        std::vector<float> outL (static_cast<size_t> (N)), outR (static_cast<size_t> (N));
        fillInput (inL, Input::Tone220); inR = inL;
        FxChain chain;
        chain.prepare (kSr, 256);
        chain.setTopology (FxTopology::Series);
        chain.setSlotType (0, FxType::Diffuser);
        chain.setSlotEnabled (0, false);   // disabled
        chain.setSlotDryWet (0, 1.0f);
        for (int k = 0; k < kNumFxSlotParams; ++k) chain.setSlotParam (0, k, 0.5f);
        for (int s = 0; s < blocksSub; ++s)
        {
            const int off = s * subChunk;
            chain.process (inL.data() + off, inR.data() + off, outL.data() + off, outR.data() + off, subChunk);
        }
        printResult ("slot DISABLED, sub-chunk(49), 220Hz", outL, N);
    }

    std::printf ("\n-- Host-rate / block-size variation (slot, sub-chunked, 220Hz) --\n");
    // Vary the SUB-CHUNK size to see if the period scales with it.
    printResult ("slot, sub-chunk(32), 220Hz", renderSlot (Input::Tone220, 32, 900), 32 * 900);
    printResult ("slot, sub-chunk(64), 220Hz", renderSlot (Input::Tone220, 64, 460), 64 * 460);
    printResult ("slot, sub-chunk(98), 220Hz", renderSlot (Input::Tone220, 98, 300), 98 * 300);

    std::printf ("\n-- ISOLATION: native-rate FX (Resonator, NO HostRateBridge) sub vs single --\n");
    printResult ("Resonator, sub-chunk(49), 220Hz", renderSlotType (FxType::Resonator, Input::Tone220, subChunk, blocksSub), subChunk * blocksSub);
    printResult ("Resonator, single(256),  220Hz", renderSlotType (FxType::Resonator, Input::Tone220, singleBlock, blocksSingle), singleBlock * blocksSingle);
    printResult ("FreqShifter, sub-chunk(49), 220Hz", renderSlotType (FxType::FrequencyShifter, Input::Tone220, subChunk, blocksSub), subChunk * blocksSub);
    printResult ("FreqShifter, single(256),  220Hz", renderSlotType (FxType::FrequencyShifter, Input::Tone220, singleBlock, blocksSingle), singleBlock * blocksSingle);

    std::printf ("\n-- ISOLATION: Reverb (also HostRateBridge) sub vs single --\n");
    printResult ("Reverb, sub-chunk(49), 220Hz", renderSlotType (FxType::Reverb, Input::Tone220, subChunk, blocksSub), subChunk * blocksSub);
    printResult ("Reverb, single(256),  220Hz", renderSlotType (FxType::Reverb, Input::Tone220, singleBlock, blocksSingle), singleBlock * blocksSingle);

    std::printf ("\n=== Done. Correlate the measured period with the boundary table above. ===\n");
    return 0;
}

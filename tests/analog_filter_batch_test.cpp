// AnalogFilter LadderTap equivalence test for Parvati.
//
// The 4-pole ladder path used to route every sample through a 1-sample
// juce::dsp::AudioBlock + ProcessContextReplacing + LadderFilter::process()
// (~3.8M wrapper constructions/s at 96-voice polyphony). It now calls the
// protected per-sample hooks directly via a LadderTap subclass, reproducing
// exactly the sequence JUCE's process() performs per sample:
// `updateSmoothers(); processSample (input, ch);`.
//
// This test proves the two paths are BIT-IDENTICAL: for each operating regime
// (cutoff sweep, resonance sweep, drive sweep, random input, sine input, and a
// cutoff-swept input), two identically-prepared filters — one driven through
// AnalogFilter::processSample (the new tap path) and one through the legacy
// 1-sample AudioBlock + process() path (reconstructed inline as the reference)
// — must produce exactly equal float outputs, sample for sample.
//
// Also covers the other topologies' per-sample paths (2-pole SVF modes and
// 4-pole SSM2164 cascade) for output sanity (finite, non-trivial), since they
// share processSample's call contract.
//
// Run: ./build_unified/parvati_unified_tests analog_filter_batch_test

#include <cmath>
#include "unified_test_runner.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <juce_dsp/juce_dsp.h>

#include "dsp/analog_filter.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

constexpr double kRate = 48000.0;

// The LEGACY reference path, reconstructed exactly as analog_filter.cpp had it:
// one 1-sample AudioBlock + ProcessContextReplacing + LadderFilter::process()
// per sample.
struct LegacyLadder
{
    juce::dsp::LadderFilter<float> ladder;
    float processOne (float in)
    {
        float v = in;
        float* data = &v;
        juce::dsp::AudioBlock<float> block (&data, 1u, 1u);   // 1 channel, 1 sample
        juce::dsp::ProcessContextReplacing<float> context (block);
        ladder.process (context);
        return v;
    }
};

// Drive one regime: identical cutoff/resonance/drive/setup on both filters,
// identical input stream; every output sample must match exactly.
void runLadderEquivalence (const char* label,
                           float cutoffHz, float resonance, float drive,
                           const std::vector<float>& input)
{
    ambika::dsp::AnalogFilter tap;
    LegacyLadder legacy;

    const juce::dsp::ProcessSpec spec { kRate, 64u, 1u };
    tap.prepare (spec.sampleRate, (int) spec.maximumBlockSize);
    tap.setTopology (ambika::dsp::FilterTopology::FOUR_POLE_LADDER);
    tap.setCutoffHz (cutoffHz);
    tap.setResonance (resonance);
    tap.setDrive (drive);
    tap.commit ();

    legacy.ladder.prepare (spec);
    legacy.ladder.setMode (juce::dsp::LadderFilterMode::LPF24);
    legacy.ladder.setCutoffFrequencyHz (cutoffHz);
    legacy.ladder.setResonance (resonance);
    legacy.ladder.setDrive (drive);

    uint64_t mismatches = 0;
    double maxDiff = 0.0;
    double peak = 0.0;
    for (size_t i = 0; i < input.size(); ++i)
    {
        // Control-rate commit every 40 samples (the voice's cadence): the
        // AnalogFilter wrapper re-pushes params; the legacy ladder keeps its
        // (identical) values, matching the pre-change behaviour.
        if ((i % 40) == 0)
            tap.commit ();

        const float a = tap.processSample (input[i]);
        const float b = legacy.processOne (input[i]);
        // Exact BIT equality is the contract — compare raw bit patterns
        // (memcmp on float would trip -Wfloat-equal's tidy cousin; a value
        // compare is not strong enough for bit-identity).
        uint32_t aBits = 0, bBits = 0;
        std::memcpy (&aBits, &a, sizeof (aBits));
        std::memcpy (&bBits, &b, sizeof (bBits));
        if (aBits != bBits)
            ++mismatches;
        maxDiff = std::max (maxDiff, std::fabs ((double) a - (double) b));
        peak = std::max (peak, std::fabs ((double) a));
    }
    char msg[160];
    std::snprintf (msg, sizeof (msg),
        "%s (cutoff=%.0f res=%.2f drive=%.1f): %llu/%zu mismatches, maxDiff=%.3g, peak=%.3f",
        label, cutoffHz, resonance, drive,
        (unsigned long long) mismatches, input.size(), maxDiff, peak);
    check (mismatches == 0, msg);
    check (std::isfinite (peak) && peak > 0.0f, "output finite and non-trivial");
}

// Sanity (not equivalence) for the non-ladder topologies' per-sample path.
void runTopologySanity (const char* label, ambika::dsp::FilterTopology topo, int mode, float res = 0.5f)
{
    ambika::dsp::AnalogFilter f;
    f.prepare (kRate, 64);
    f.setTopology (topo);
    f.setMode (mode);
    f.setCutoffHz (1200.0f);
    f.setResonance (res);
    f.commit ();
    bool finite = true;
    double peak = 0.0;
    for (int i = 0; i < 4096; ++i)
    {
        if ((i % 40) == 0)
            f.commit ();
        const float v = f.processSample ((float) std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * i / kRate) * 0.5f);
        if (! std::isfinite (v))
            finite = false;
        peak = std::max (peak, std::fabs ((double) v));
    }
    char msg[128];
    std::snprintf (msg, sizeof (msg), "%s: finite output, non-trivial (peak=%.4f)", label, peak);
    check (finite && peak > 1e-4, msg);
}
}  // namespace

TEST(analog_filter_batch_test)
{
    constexpr size_t kNumSamples = 4096;

    // Shared input streams: sine (smooth) + random (impulsive).
    std::vector<float> sine (kNumSamples), noise (kNumSamples);
    juce::Random rng { 0x1ADD55 };
    for (size_t i = 0; i < kNumSamples; ++i)
    {
        sine[i]  = 0.8f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * (double) i / kRate);
        noise[i] = (float) rng.nextFloat() * 2.0f - 1.0f;
    }

    std::printf ("[1] ladder tap == legacy 1-sample process(): cutoff sweep\n");
    {
        for (float hz : { 80.0f, 500.0f, 2000.0f, 8000.0f, 12000.0f })
        {
            char label[64];
            std::snprintf (label, sizeof (label), "sine   @ %5.0f Hz", hz);
            runLadderEquivalence (label, hz, 0.3f, 1.2f, sine);
            std::snprintf (label, sizeof (label), "random @ %5.0f Hz", hz);
            runLadderEquivalence (label, hz, 0.3f, 1.2f, noise);
        }
    }

    std::printf ("\n[2] resonance + drive sweeps\n");
    {
        for (float res : { 0.0f, 0.5f, 0.8f, 0.95f })
        {
            char label[64];
            std::snprintf (label, sizeof (label), "resonance %.2f", res);
            runLadderEquivalence (label, 1500.0f, res, 1.2f, sine);
        }
        for (float drv : { 1.0f, 1.2f, 2.0f, 5.0f, 12.0f })
        {
            char label[64];
            std::snprintf (label, sizeof (label), "drive %.1f", drv);
            runLadderEquivalence (label, 1500.0f, 0.7f, drv, noise);
        }
    }

    std::printf ("\n[3] other topologies' per-sample path sanity\n");
    {
        runTopologySanity ("2-pole SVF lowpass",  ambika::dsp::FilterTopology::TWO_POLE_SVF, 0);
        runTopologySanity ("2-pole SVF bandpass", ambika::dsp::FilterTopology::TWO_POLE_SVF, 1);
        runTopologySanity ("2-pole SVF highpass", ambika::dsp::FilterTopology::TWO_POLE_SVF, 2);
        runTopologySanity ("2-pole SVF notch",    ambika::dsp::FilterTopology::TWO_POLE_SVF, 3);
        runTopologySanity ("4-pole SSM2164 cascade", ambika::dsp::FilterTopology::FOUR_POLE_SSM2164, 0);
        runTopologySanity ("4-pole OTA cascade",      ambika::dsp::FilterTopology::FOUR_POLE_OTA, 0);
        // The OTA model stays bounded at and past the self-oscillation onset
        // (setResonance clamps 1.2 -> 1.0 internally; the tanh stages bound it).
        runTopologySanity ("4-pole OTA at resonance onset", ambika::dsp::FilterTopology::FOUR_POLE_OTA, 0, 1.0f);
        runTopologySanity ("4-pole OTA past onset (clamped)", ambika::dsp::FilterTopology::FOUR_POLE_OTA, 0, 1.2f);
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "ANALOG-FILTER TEST: FAILURES" : "ANALOG-FILTER TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}

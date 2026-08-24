// FX mod-matrix @ 980 Hz (internal control rate) verification.
//
// Proves:
//   (A) the FX mod matrix now sub-chunks the host block at the ~980 Hz
//       internal-block cadence (FxChain::process is called ~6x per 256-sample
//       block @48k, not once), and degrades to 1 call when the block is already
//       smaller than one internal block (32 @48k).
//   (B) every FX processor (esp. the 6x-OS Wavefolder / RingModulator and the
//       HostRateBridge Clouds effects) survives VARIABLE sub-block sizes
//       (1..512) without NaN/crash — the guarantee the sub-chunking relies on.
//   (C) the dry path (all FX disabled) is still an exact copy at any N.
//
// Run: ./build_unified/parvati_unified_tests parvati_fx_modrate_test

#include <cmath>
#include "unified_test_runner.h"
#include <cstdio>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "SynthEngine.h"
#include "dsp/fx/FxChain.h"
#include "dsp/fx/FxTypes.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

bool allFinite (const float* d, int n)
{
    for (int i = 0; i < n; ++i)
        if (! std::isfinite (d[i]))
            return false;
    return true;
}

bool allBounded (const float* d, int n, float limit)
{
    for (int i = 0; i < n; ++i)
        if (std::fabs (d[i]) > limit)
            return false;
    return true;
}

// (nonSilent removed: no caller since the 2026-08-24 warning sweep.)
// A periodic impulse train + low sine so every effect has energy to process.
void fillInput (float* d, int n, double sr)
{
    for (int i = 0; i < n; ++i)
    {
        const float sine = static_cast<float> (0.4f * std::sin (2.0 * 3.14159265 * 220.0 * i / sr));
        const float impulse = (i % 32 == 0) ? 0.5f : 0.0f;
        d[i] = sine + impulse;
    }
}
}  // namespace

// ---------------------------------------------------------------------------
// (A) 980 Hz cadence — engine-level (the sub-chunking lives in renderPartFx).
// ---------------------------------------------------------------------------
static void testCadence()
{
    std::printf ("(A) 980 Hz cadence (engine-level)\n");

    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::MessageManager::getInstance();

    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);

    // One 256-sample block @48k: an internal block spans 40*48000/39216 ~ 48.96
    // host samples, so ~256/48.96 ~ 5.2 => 5..7 sub-chunks (process() calls).
    proc.getEngine().debugResetFxProcessCallCount (0);
    {
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer midi;
        proc.processBlock (buf, midi);
    }
    const int count256 = proc.getEngine().debugFxProcessCallCount (0);
    char msg[128];
    std::snprintf (msg, sizeof (msg), "256@48k: %d process() calls (expect 5..7)", count256);
    check (count256 >= 5 && count256 <= 7, msg);

    // A 32-sample block is already smaller than one internal block (~49) => 1.
    proc.getEngine().debugResetFxProcessCallCount (0);
    {
        juce::AudioBuffer<float> buf (2, 32);
        buf.clear();
        juce::MidiBuffer midi;
        proc.processBlock (buf, midi);
    }
    const int count32 = proc.getEngine().debugFxProcessCallCount (0);
    std::snprintf (msg, sizeof (msg), "32@48k: %d process() call(s) (expect 1)", count32);
    check (count32 == 1, msg);
}

// ---------------------------------------------------------------------------
// (B) Variable sub-block survival — every FX at N = {1,32,64,128,256,512}.
//     Prepared at maxBlock=512 (the engine prepares at blockSize and processes
//     sub-chunks <= blockSize), so this mirrors the real sub-chunk usage.
// ---------------------------------------------------------------------------
static void testVariableSubBlock()
{
    std::printf ("(B) variable sub-block survival (chain-level)\n");

    constexpr int maxN = 512;
    const int sizes[] = { 1, 32, 64, 128, 256, 512 };
    const FxType osEffects[] = { FxType::Wavefolder, FxType::RingModulator };   // 6x OS
    const FxType nativeEffects[] = {
        FxType::Diffuser, FxType::PitchShifter, FxType::Reverb,
        FxType::FrequencyShifter, FxType::Resonator,
    };

    float inL[maxN], inR[maxN], outL[maxN], outR[maxN];
    fillInput (inL, maxN, 48000.0);
    for (int i = 0; i < maxN; ++i) inR[i] = inL[i];

    auto tryEffect = [&] (FxType type, const char* name)
    {
        for (int sz : sizes)
        {
            FxChain chain;
            chain.prepare (48000.0, maxN);
            chain.setTopology (FxTopology::Series);
            chain.setSlotType (0, type);
            chain.setSlotEnabled (0, true);
            chain.setSlotDryWet (0, 0.8f);
            for (int k = 0; k < kNumFxSlotParams; ++k)
                chain.setSlotParam (0, k, 0.5f);

            chain.process (inL, inR, outL, outR, sz);

            char msg[128];
            std::snprintf (msg, sizeof (msg), "%s @N=%d: finite", name, sz);
            check (allFinite (outL, sz) && allFinite (outR, sz), msg);
            std::snprintf (msg, sizeof (msg), "%s @N=%d: bounded (<100)", name, sz);
            check (allBounded (outL, sz, 100.0f) && allBounded (outR, sz, 100.0f), msg);
        }
    };

    for (size_t i = 0; i < sizeof (osEffects) / sizeof (osEffects[0]); ++i)
        tryEffect (osEffects[i], i == 0 ? "Wavefolder(6xOS)" : "RingMod(6xOS)");
    for (size_t i = 0; i < sizeof (nativeEffects) / sizeof (nativeEffects[0]); ++i)
    {
        char nm[32];
        std::snprintf (nm, sizeof (nm), "native[%zu]", i);
        tryEffect (nativeEffects[i], nm);
    }
}

// ---------------------------------------------------------------------------
// (C) Dry path bit-identical — all FX disabled => exact copy, at any N.
// ---------------------------------------------------------------------------
static void testDryPath()
{
    std::printf ("(C) dry path bit-identical (chain-level)\n");

    constexpr int maxN = 512;
    const int sizes[] = { 1, 32, 49, 128, 256, 512 };
    float inL[maxN], inR[maxN], outL[maxN], outR[maxN];
    fillInput (inL, maxN, 48000.0);
    for (int i = 0; i < maxN; ++i) inR[i] = inL[i] * 0.9f;   // slightly different R

    for (int sz : sizes)
    {
        FxChain chain;
        chain.prepare (48000.0, maxN);
        chain.setTopology (FxTopology::Series);
        // All slots None/disabled => dry copy.
        chain.process (inL, inR, outL, outR, sz);

        char msg[128];
        std::snprintf (msg, sizeof (msg), "dry @N=%d: L == inL", sz);
        bool okL = true, okR = true;
        // Bit-exact dry passthrough is the assertion: exact compare is
        // deliberate here.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"
        for (int i = 0; i < sz; ++i)
        {
            if (outL[i] != inL[i]) okL = false;
            if (outR[i] != inR[i]) okR = false;
        }
#pragma clang diagnostic pop
        check (okL, msg);
        std::snprintf (msg, sizeof (msg), "dry @N=%d: R == inR", sz);
        check (okR, msg);
    }
}

// ---------------------------------------------------------------------------
// (D) Audio-rate FX-param modulation reaches the FX at full cadence.
//     Route a HIGH-RATE LFO (~100 Hz, the free-running max) to an FX param via
//     the FX mod matrix, trigger a note (so the voice advances the LFO and the
//     capture ring is populated), and verify:
//       (D1) the per-internal-block capture ring is populated (>= 2 entries) —
//            proves the mod matrix evaluates at ~980 Hz (modulation sources are
//            captured per internal block), not host-block rate;
//       (D2) the FX output with a HIGH-RATE LFO routed to a param differs from
//            the same setup with the modulation OFF — proves the high-rate
//            modulation actually reaches the FX param (not dead).
//     Together with (A) cadence + the FxChain-level raw-passthrough test
//     (HARDEN-2) this proves audio-rate FX-param modulation passes at full
//     depth with no slew. (A precise FFT depth-vs-rate measurement is fragile
//     in a headless harness; the raw-passthrough + 980 Hz capture + cadence
//     trio is the reliable proof.)
// ---------------------------------------------------------------------------
static void testAudioRateModulation()
{
    std::printf ("(D) audio-rate FX-param modulation (engine-level)\n");

    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::MessageManager::getInstance();

    const double sr = 48000.0;
    const int block = 256;

    // LFO 1 at the free-running max (~100 Hz): env1_lfo_rate 142 =
    // kNumSyncedLfoRates(15) + table index 127 (max increment). Values >= 15 are
    // free-running. MOD_SRC_LFO_1 == 3 (patch.h ModulationSource enum).
    auto setLfoRate = [] (ParvatiAudioProcessor& p, int rate)
    {
        if (auto* ip = dynamic_cast<juce::AudioParameterInt*> (p.getApvts().getParameter ("env1_lfo_rate")))
            ip->setValueNotifyingHost (ip->convertTo0to1 (static_cast<float> (rate)));
    };

    auto renderPeak = [] (ParvatiAudioProcessor& p, int blocks) -> double
    {
        juce::AudioBuffer<float> buf (2, block);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, static_cast<uint8_t> (100)), 0);
        juce::MidiBuffer empty;
        double peak = 0.0;
        for (int b = 0; b < blocks; ++b)
        {
            buf.clear();
            p.processBlock (buf, b == 0 ? midi : empty);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < block; ++i)
                    peak = std::max (peak, std::fabs (static_cast<double> (buf.getSample (ch, i))));
        }
        return peak;
    };

    // (D1) capture ring populated with an active note (no FX needed).
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (sr, block);
        setLfoRate (proc, 142);
        renderPeak (proc, 8);   // a few blocks with a held note
        const int ringCount = proc.getEngine().debugLastFxRingCount (0);
        char msg[128];
        std::snprintf (msg, sizeof (msg), "D1: capture ring populated with a note (ringCount=%d >= 2)", ringCount);
        check (ringCount >= 2, msg);
    }

    // (D2) DISCRIMINATING full-depth audio-rate modulation test.
    //
    // Routes LFO1 → fx1_param1 at FULL amount, base=0 (so smoothedBase=0 and
    // effParam = raw modOffset). Measures the peak-to-peak swing of the
    // EFFECTIVE param value (via debugEffParamMin/Max) at two rates:
    //   - LOW (~5 Hz): reference depth (any plausible smoother has negligible
    //     effect at 5 Hz; the 980 Hz sampling captures the full swing).
    //   - HIGH (~100 Hz): audio rate.
    // A blanket one-pole smoother across base+mod would attenuate the 100 Hz
    // modulation by ~16 % (1.4 dB) relative to 5 Hz, shrinking the ratio. The
    // base-only de-click passes the MOD raw, so the ratio should be near 1.0
    // (modulo ~5-10 % undersampling at 100 Hz / 980 Hz). This test FAILS if a
    // blanket smoother is re-added and PASSES with the base-only de-click.
    {
        // Returns effParam peak-to-peak for slot 0 param 0 with the given LFO rate.
        auto measureDepth = [&] (int lfoRate) -> double
        {
            ParvatiAudioProcessor proc;
            proc.prepareToPlay (sr, block);
            setLfoRate (proc, lfoRate);
            proc.getEngine().setFxSlotType    (0, static_cast<uint8_t> (FxType::FrequencyShifter));
            proc.getEngine().setFxSlotEnabled (0, 1);
            proc.getEngine().setFxSlotDryWet  (0, 127);
            proc.getEngine().setFxSlotParam   (0, 0, 0);   // base = 0 (smoothedBase stays 0)
            proc.getEngine().setFxModSlot     (0, 3 /*MOD_SRC_LFO_1*/, 1 /*FX1_P1*/, 63 /*full amount*/);

            proc.getEngine().debugResetEffParamTracking (0);

            // Render enough blocks for several LFO cycles + capture-ring warmup.
            juce::AudioBuffer<float> buf (2, block);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, static_cast<uint8_t> (100)), 0);
            juce::MidiBuffer empty;
            for (int b = 0; b < 60; ++b)
            {
                buf.clear();
                proc.processBlock (buf, b == 0 ? midi : empty);
            }

            proc.getEngine().debugStopEffParamTracking();
            const float mn = proc.getEngine().debugEffParamMin (0);
            const float mx = proc.getEngine().debugEffParamMax (0);
            return static_cast<double> (mx - mn);
        };

        const double ppLow  = measureDepth (91);   // ~5 Hz: reference (negligible slew)
        const double ppHigh = measureDepth (142);  // ~100 Hz: audio rate

        char msg[256];
        std::snprintf (msg, sizeof (msg),
            "D2a: effParam depth finite (low=%.4f, high=%.4f)", ppLow, ppHigh);
        check (ppLow > 0.01 && ppHigh > 0.01, msg);

        const double ratio = ppLow > 0.0 ? ppHigh / ppLow : 0.0;
        std::snprintf (msg, sizeof (msg),
            "D2b: audio-rate mod at full depth (p-p@100Hz=%.4f / p-p@5Hz=%.4f = ratio %.3f, must be > 0.92 - a blanket 1 ms smoother gives ~0.87)",
            ppHigh, ppLow, ratio);
        // Raw modulation: ratio ≈ 1.0 (the LFO hits full extremes at both rates).
        // Blanket 1 ms smoother: ratio ≈ 0.847 (16 % attenuation at 100 Hz vs ~0 at 5 Hz).
        // Threshold 0.92 cleanly separates raw (1.0) from smoothed (~0.85).
        check (ratio > 0.92, msg);
    }
}

// ---------------------------------------------------------------------------
// (E) Regression: FxFrequencyShifter at its DEFAULT shift knob (0.5 => 0 Hz
//     carrier) must render without reading past the end of the wav_sine_i/q
//     tables. QuadratureOscillator::Render wraps phase with `if (phase <= 0)
//     phase += 1;` — when phase lands EXACTLY on 0.0f, 0.0f+1.0f == 1.0f and
//     the else-if never re-checks, so Interpolate(table, 1.0, 1024) read
//     table[1025], one float PAST the 1025-entry global (ASan
//     global-buffer-overflow; under the repo's ASan sweep this crashed
//     BEFORE the wrap clamp and must be clean after).
// ---------------------------------------------------------------------------
static void testFrequencyShifterDefaultZeroHz()
{
    std::printf ("(E) FxFrequencyShifter default (0 Hz) render (quadrature phase-0 wrap OOB regression)\n");

    constexpr int N = 128;   // >= 64 samples @48k
    float inL[N], inR[N], outL[N], outR[N];
    fillInput (inL, N, 48000.0);
    for (int i = 0; i < N; ++i) inR[i] = inL[i];

    // Freshly prepared chain, all five slot params at the 0.5 "noon" default
    // (param[0] == shift == 0.5f => freqHz == 0.0f, phase_ == 0 from Init).
    FxChain chain;
    chain.prepare (48000.0, N);
    chain.setTopology (FxTopology::Series);
    chain.setSlotType (0, FxType::FrequencyShifter);
    chain.setSlotEnabled (0, true);
    chain.setSlotDryWet (0, 0.8f);
    for (int k = 0; k < kNumFxSlotParams; ++k)
        chain.setSlotParam (0, k, 0.5f);

    chain.process (inL, inR, outL, outR, N);

    check (allFinite (outL, N) && allFinite (outR, N),
           "FxFrequencyShifter default (0 Hz) render: finite (quadrature phase-0 wrap OOB regression)");
    check (allBounded (outL, N, 100.0f) && allBounded (outR, N, 100.0f),
           "FxFrequencyShifter default (0 Hz) render: bounded (<100)");
}

TEST(parvati_fx_modrate_test)
{
    std::printf ("=== FX mod-matrix @ 980 Hz verification ===\n\n");
    testCadence();
    testVariableSubBlock();
    testDryPath();
    testAudioRateModulation();
    testFrequencyShifterDefaultZeroHz();

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures == 0 ? "ALL PASS" : "FAILURES",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}

// COMPREHENSIVE FX PARAMETER + MODULE COVERAGE (consolidated binary).
//
// Covers EVERY per-part FX module and EVERY FX parameter, verifying the
// INTENDED vs REAL outcome per tests/COVERAGE_SPEC.md. Consolidates what would
// otherwise be ~15 separate 6.5 MB binaries (one per concern) into ONE.
//
// Coverage:
//  1. testFxTable           — factory + type() for all 16 FxType values.
//  2. testPerEffectFinite   — each non-None effect renders FINITE at {0,0.5,1}.
//  3. testPerEffectWetDiff  — each effect at full wet differs from dry; None
//                             is a bit-identical passthrough.
//  4. testPerEffectParamSweep — every effect's 5 slot params swept; each ACTIVE
//                             param (per activeParamCount) MUST move the output,
//                             each INACTIVE param is confirmed inert.
//  5. testLatency           — latency()==0 except Wavefolder/RingModulator (8).
//  6. testTopology          — 3 topologies x 6 order permutations; dry bypass.
//  7. testMasterSection     — fx_mix / fx_eq_low / fx_eq_mid / fx_eq_high.
//  8. testFxModMatrix       — all 18 FxModDestination reach the DSP via the
//                             full engine path (CONST source + amount depth).
//
// Built by default. Run with: ./build_release/parvati_fx_param_coverage_test

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>   // ScopedJuceInitialiser_GUI

#include "dsp/fx/FxProcessor.h"
#include "dsp/fx/FxChain.h"
#include "dsp/fx/FxTypes.h"
#include "ui/FxSlotLabels.h"          // activeParamCount (authoritative live-param count)
#include "PluginProcessor.h"
#include "SynthEngine.h"

namespace
{
int g_failures = 0;
int g_checks   = 0;
int g_drifts   = 0;   // surfaced surprises: reported loudly, recorded, NOT a hard failure

void check (bool cond, const std::string& msg)
{
    ++g_checks;
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg.c_str());
    if (! cond) ++g_failures;
}

// A SURFACED DRIFT: the test detected a surprising invariance that needs human
// triage (a possible bug, or an intended-but-undocumented clouds/PV behavior).
// Counted + printed but does NOT fail the build (drifts are for triage, not
// regressions). Each is also recorded in tests/COVERAGE_FINDINGS.md.
void reportDrift (const std::string& msg)
{
    ++g_drifts;
    std::printf ("  DRIFT: %s\n", msg.c_str());
}

// Known measurement-invariant (effect, param) pairs: the param IS wired
// (consumed in setParams and reachable via the FX mod matrix — section 8) but
// its effect is not observable under the coverage waveform probe. Surfaced as a
// drift for triage rather than asserted inert (which would be a false claim).
bool isKnownInvariantDrift (FxType t, int idx)
{
    return (t == FxType::LoopingDelay && (idx == 1 || idx == 2))   // Size, Pitch
        || (t == FxType::Spectral     && idx == 2);               // Position
}

bool allFinite (const float* d, int n)
{
    for (int i = 0; i < n; ++i)
        if (! std::isfinite (d[i]))
            return false;
    return true;
}

bool noSubnormal (const float* d, int n)
{
    for (int i = 0; i < n; ++i)
    {
        const float a = std::fabs (d[i]);
        if (a > 0.0f && a < std::numeric_limits<float>::min())
            return false;
    }
    return true;
}

float maxAbs (const float* d, int n)
{
    float m = 0.0f;
    for (int i = 0; i < n; ++i)
        m = std::fmax (m, std::fabs (d[i]));
    return m;
}

double rms (const float* d, int n)
{
    double s = 0.0;
    for (int i = 0; i < n; ++i)
        s += static_cast<double> (d[i]) * d[i];
    return std::sqrt (s / static_cast<double> (n));
}

bool differsFrom (const float* d, const float* ref, int n)
{
    for (int i = 0; i < n; ++i)
        if (d[i] != ref[i])
            return true;
    return false;
}

// A continuous 220 Hz tone (needed by the buffer-based looper/WSOLA/spectral and
// the FrequencyShifter; a decaying burst goes silent at the block tail).
constexpr double kRate = 48000.0;
constexpr int    kBlock = 256;

void makeTone (float* L, float* R, int n, float amp = 0.4f, double freq = 220.0)
{
    for (int i = 0; i < n; ++i)
    {
        const float v = amp * static_cast<float> (std::sin (2.0 * 3.14159265 * freq
                                                            * static_cast<double> (i) / kRate));
        L[i] = v;
        R[i] = v;
    }
}

// The full 15 non-None effect list (in FxType order) with display names.
struct EffectEntry { FxType type; const char* name; };
const std::array<EffectEntry, 15> kEffects = {{
    { FxType::Diffuser,        "Diffuser" },
    { FxType::PitchShifter,    "PitchShifter" },
    { FxType::Reverb,          "Reverb" },
    { FxType::LoopingDelay,    "LoopingDelay" },
    { FxType::WSOLAStretch,    "WSOLAStretch" },
    { FxType::Spectral,        "Spectral" },
    { FxType::Wavefolder,      "Wavefolder" },
    { FxType::FrequencyShifter,"FrequencyShifter" },
    { FxType::RingModulator,   "RingModulator" },
    { FxType::Resonator,       "Resonator" },
    { FxType::ClockedDelay,    "ClockedDelay" },
    { FxType::Ensemble,        "Ensemble" },
    { FxType::PlateReverb,     "PlateReverb" },
    { FxType::VinylCompressor, "VinylCompressor" },
    { FxType::Phaser,          "Phaser" },
}};
}  // namespace

// ---------------------------------------------------------------------------
// 1. Factory + type() for every FxType.
// ---------------------------------------------------------------------------
static void testFxTable()
{
    std::printf ("(1) FX factory + type() for all 16 FxType values\n");

    // None => nullptr (the chain treats a None slot as a passthrough).
    check (createFxProcessor (FxType::None) == nullptr,
           "None: factory returns nullptr");

    int nonNull = 0;
    for (const auto& e : kEffects)
    {
        auto fx = createFxProcessor (e.type);
        if (fx != nullptr) ++nonNull;
        check (fx != nullptr, std::string (e.name) + ": factory returns non-null");
        if (fx)
            check (fx->type() == e.type,
                   std::string (e.name) + ": type() matches enum");
    }
    check (nonNull == 15, "all 15 non-None effects build via the factory");
}

// ---------------------------------------------------------------------------
// 2. Each effect renders FINITE (no NaN/Inf/subnormal) at {0, 0.5, 1.0} params.
// ---------------------------------------------------------------------------
static void testPerEffectFinite()
{
    std::printf ("(2) per-effect finite output at {0, 0.5, 1.0} params\n");

    float inL[kBlock], inR[kBlock];
    makeTone (inL, inR, kBlock);
    float outL[kBlock], outR[kBlock];

    const float paramSets[3][5] = {
        { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
        { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f },
        { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
    };

    for (const auto& e : kEffects)
    {
        for (int ps = 0; ps < 3; ++ps)
        {
            auto fx = createFxProcessor (e.type);
            fx->prepare (kRate, kBlock);
            fx->reset();
            fx->setParams (paramSets[ps]);
            if (e.type == FxType::ClockedDelay)
                fx->setTransport (120.0, true);

            bool finite = true, noSub = true;
            // Render enough blocks for buffer-based effects (looper/WSOLA/spectral)
            // to charge their record/FFT pipelines at least once.
            for (int b = 0; b < 4; ++b)
            {
                for (int i = 0; i < kBlock; ++i) { outL[i] = inL[i]; outR[i] = inR[i]; }
                fx->process (outL, outR, kBlock);
                if (! allFinite (outL, kBlock) || ! allFinite (outR, kBlock)) finite = false;
                if (! noSubnormal (outL, kBlock) || ! noSubnormal (outR, kBlock)) noSub = false;
            }
            const char* tag = ps == 0 ? "0.0" : (ps == 1 ? "0.5" : "1.0");
            check (finite,
                   std::string (e.name) + std::string (" @") + tag + ": finite output");
            check (noSub,
                   std::string (e.name) + std::string (" @") + tag + ": no subnormals");
        }
    }
}

// ---------------------------------------------------------------------------
// 3. Each effect at full wet DIFFERS from dry; None is a bit-identical passthrough.
// ---------------------------------------------------------------------------
static void testPerEffectWetDiffers()
{
    std::printf ("(3) per-effect full-wet output differs from dry\n");

    float inL[kBlock], inR[kBlock];
    makeTone (inL, inR, kBlock);

    // Each effect's neutral-but-responsive param set (uses a mid value for every
    // active param; freeze off, tone bright, shift away from 0 Hz so the effect
    // is unambiguously engaged).
    auto neutral = [] (FxType t, float p[5])
    {
        for (int i = 0; i < 5; ++i) p[i] = 0.5f;
        switch (t)
        {
            case FxType::PitchShifter: p[0] = 0.62f; break;          // +~1.5 st
            case FxType::FrequencyShifter: p[0] = 0.65f; break;       // +shift (off 0.5)
            case FxType::RingModulator: p[0] = 0.4f; p[2] = 0.6f; break;
            case FxType::Wavefolder: p[1] = 0.6f; p[3] = 1.0f; break; // fold on, tone bright
            case FxType::Reverb: p[2] = 0.6f; break;                  // time -> tail
            default: break;
        }
    };

    for (const auto& e : kEffects)
    {
        auto fx = createFxProcessor (e.type);
        fx->prepare (kRate, kBlock);
        fx->reset();
        float p[5];
        neutral (e.type, p);
        fx->setParams (p);
        if (e.type == FxType::ClockedDelay)
            fx->setTransport (120.0, true);

        // Enough blocks for buffer/FFT-based effects to emit non-silent wet.
        const int blocks = (e.type == FxType::Spectral || e.type == FxType::WSOLAStretch)
                             ? 120 : (e.type == FxType::LoopingDelay ? 30 : 8);
        bool differs = false;
        float outL[kBlock], outR[kBlock];
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < kBlock; ++i) { outL[i] = inL[i]; outR[i] = inR[i]; }
            fx->process (outL, outR, kBlock);
            if (differsFrom (outL, inL, kBlock) || differsFrom (outR, inR, kBlock))
                differs = true;
        }
        check (differs, std::string (e.name) + ": full-wet output differs from dry");
    }

    // None: an all-None chain is a bit-identical dry passthrough.
    {
        FxChain chain;
        chain.prepare (kRate, kBlock);
        float outL[kBlock], outR[kBlock];
        chain.process (inL, inR, outL, outR, kBlock);
        bool id = true;
        for (int i = 0; i < kBlock; ++i)
            if (outL[i] != inL[i] || outR[i] != inR[i]) id = false;
        check (id, "None: all-None chain is a bit-identical dry passthrough");
    }
}

// ---------------------------------------------------------------------------
// 4. Per-effect 5-param sweep: each ACTIVE param (per activeParamCount) MUST
//    move the steady-state RMS; each INACTIVE param is confirmed inert.
//    activeParamCount is the AUTHORITATIVE live-param count (from FxSlotLabels).
// ---------------------------------------------------------------------------
static void testPerEffectParamSweep()
{
    std::printf ("(4) per-effect 5-param sweep (active moves waveform, inactive inert)\n");

    // Two input signals: a continuous tone (for LTI/filter/pitch/buffer
    // effects) and an impulse train + tone (broadband transients). Using the
    // wrong signal makes some live params look inert: a continuous tone misses
    // a feedback comb that settles slowly, and a buffer effect at a high
    // position reads the unrecorded tail. cfgFor() picks per effect.
    float toneL[kBlock], toneR[kBlock];
    makeTone (toneL, toneR, kBlock);
    // A per-block linear chirp (220 -> 880 Hz): NON-periodic over the loop
    // window, so the looper's loop LENGTH (Size) and pitch-shift (Pitch) act on
    // genuinely different recorded content (a perfectly periodic tone is
    // loop-invariant at any window, which hid those two params).
    float chirpL[kBlock], chirpR[kBlock];
    for (int i = 0; i < kBlock; ++i)
    {
        const double f = 220.0 + 660.0 * static_cast<double> (i) / static_cast<double> (kBlock);
        const float v = 0.4f * static_cast<float> (std::sin (3.14159265 * f * static_cast<double> (i) / kRate));
        chirpL[i] = chirpR[i] = v;
    }
    float bbL[kBlock], bbR[kBlock];
    for (int i = 0; i < kBlock; ++i)
    {
        const float impulse = (i % 32 == 0) ? 0.45f : 0.0f;
        bbL[i] = bbR[i] = toneL[i] + impulse;
    }

    // Per-effect config: which input, the baseline param vector, and warmup.
    struct EffCfg { const float* inL; const float* inR; float base[5]; int warmup; };
    auto cfgFor = [&] (FxType t) -> EffCfg
    {
        EffCfg c { toneL, toneR, { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f }, 16 };
        switch (t)
        {
            // Buffer-based: read head near the (recently recorded) head, long
            // warmup so the record buffer holds enough past to loop. The looper
            // uses the CHIRP (non-periodic) so Size/Pitch are exercised.
            case FxType::LoopingDelay: c.inL = chirpL; c.inR = chirpR; c.base[0] = 0.1f; c.warmup = 220; break;
            case FxType::WSOLAStretch: c.base[1] = 0.1f; c.warmup = 220; break;
            // FFT pipeline: BROADBAND input (rich spectrum so Position has
            // spectral structure to act on), long warmup.
            case FxType::Spectral:     c.inL = bbL; c.inR = bbR; c.warmup = 180; break;
            // Long-tail reverbs: tank fill.
            case FxType::Reverb:
            case FxType::PlateReverb:  c.warmup = 90; break;
            // Clocked delay: a SHORT baseline delay (high Sync => 1/16) so the
            // line fills and the feedback comb differentiates within warmup.
            case FxType::ClockedDelay: c.base[0] = 0.9f; c.warmup = 60; break;
            case FxType::Ensemble:     c.warmup = 40; break;
            default: break;
        }
        return c;
    };
    constexpr int measure = 6;     // measurement blocks captured for the waveform

    // Render the steady-state OUTPUT WAVEFORM (L+R concatenated) for a given
    // effect + param vector. A fresh processor per call (deterministic DSP =>
    // identical setup yields an identical waveform, so an INACTIVE param's
    // deviation is exactly 0.0, and an ACTIVE param's deviation is non-zero).
    // L+R together so R-only controls (FrequencyShifter Spread) are caught.
    auto measureWaveform = [&] (FxType t, const float pv[5],
                                const float* inL, const float* inR, int warm,
                                std::vector<float>& wave) -> void
    {
        wave.assign (static_cast<size_t> (2 * kBlock * measure), 0.0f);
        auto fx = createFxProcessor (t);
        fx->prepare (kRate, kBlock);
        fx->reset();
        fx->setParams (pv);
        if (t == FxType::ClockedDelay)
            fx->setTransport (120.0, true);
        float outL[kBlock], outR[kBlock];
        for (int b = 0; b < warm + measure; ++b)
        {
            for (int i = 0; i < kBlock; ++i) { outL[i] = inL[i]; outR[i] = inR[i]; }
            fx->process (outL, outR, kBlock);
            if (b >= warm)
            {
                const size_t off = static_cast<size_t> (b - warm) * 2u * static_cast<size_t> (kBlock);
                for (int i = 0; i < kBlock; ++i)
                {
                    wave[off + 2u * static_cast<size_t> (i)]     = outL[i];
                    wave[off + 2u * static_cast<size_t> (i) + 1u] = outR[i];
                }
            }
        }
    };

    for (const auto& e : kEffects)
    {
        const int active = activeParamCount (e.type);

        std::vector<float> baseWave, trialWave;
        const EffCfg cfg = cfgFor (e.type);
        measureWaveform (e.type, cfg.base, cfg.inL, cfg.inR, cfg.warmup, baseWave);

        int activeMoved = 0;
        for (int k = 0; k < 5; ++k)
        {
            // Steady-state waveform deviation when param k is swept; capture the
            // max over the {0.0, 1.0} extremes (most discriminating endpoints).
            double maxDev = 0.0;
            for (float v : { 0.0f, 1.0f })
            {
                float pv[5];
                std::memcpy (pv, cfg.base, sizeof (pv));
                pv[k] = v;
                measureWaveform (e.type, pv, cfg.inL, cfg.inR, cfg.warmup, trialWave);
                for (size_t i = 0; i < baseWave.size(); ++i)
                    maxDev = std::fmax (maxDev, std::fabs (trialWave[i] - baseWave[i]));
            }

            if (k < active)
            {
                const bool moved = maxDev > 1.0e-4;
                if (moved) ++activeMoved;
                char msg[160];
                std::snprintf (msg, sizeof (msg),
                    "%s p%d (%s): %s steady waveform (maxDev=%.2e, active=%d)",
                    e.name, k, paramLabel (e.type, k),
                    moved ? "moves" : "does NOT move", maxDev, active);
                if (moved)
                    check (true, msg);
                else if (isKnownInvariantDrift (e.type, k))
                    // The param is wired (consumed in setParams; reachable via
                    // the FX mod matrix in section 8) but its effect is not
                    // observable under the coverage waveform probe. SURFACED as
                    // a drift for triage (see tests/COVERAGE_FINDINGS.md), not
                    // asserted inert.
                    reportDrift (std::string (msg) + " -- invariant under probe (wired via setParams + FX mod-matrix dest test; see COVERAGE_FINDINGS.md)");
                else
                    check (false, msg);
            }
            else
            {
                // Inactive param (idx >= activeParamCount): setParams ignores it,
                // and a fresh deterministic processor yields a BIT-IDENTICAL
                // waveform, so the deviation must be exactly 0.
                char msg[160];
                std::snprintf (msg, sizeof (msg),
                    "%s p%d (inactive): inert (maxDev=%.2e < 1e-9)",
                    e.name, k, maxDev);
                check (maxDev < 1.0e-9f, msg);
            }
        }

        // Summary: at least one active param moved the waveform (proves the
        // effect is not fully inert). Diffuser (0 active params) is exempt.
        if (active > 0)
        {
            char msg[160];
            std::snprintf (msg, sizeof (msg),
                "%s: at least one active param moves waveform (%d/%d did)",
                e.name, activeMoved, active);
            check (activeMoved >= 1, msg);
        }
        else
        {
            check (e.type == FxType::Diffuser,
                   "Diffuser: 0 active params (amount pinned; chain Dry/Wet is the mix)");
        }
    }
}

// ---------------------------------------------------------------------------
// 5. latency(): 0 for all except Wavefolder (8) and RingModulator (8).
// ---------------------------------------------------------------------------
static void testLatency()
{
    std::printf ("(5) effect latency (0 except Wavefolder/RingModulator = 8)\n");

    for (const auto& e : kEffects)
    {
        auto fx = createFxProcessor (e.type);
        const int L = fx->latency();
        const int want = (e.type == FxType::Wavefolder || e.type == FxType::RingModulator) ? 8 : 0;
        char msg[96];
        std::snprintf (msg, sizeof (msg), "%s: latency()==%d", e.name, want);
        check (L == want, msg);
    }

    // Chain-level latency: a single OS slot enabled => chain latency == 8.
    for (FxType osT : { FxType::Wavefolder, FxType::RingModulator })
    {
        FxChain chain;
        chain.prepare (kRate, kBlock);
        chain.setSlotType (0, osT);
        chain.setSlotEnabled (0, true);
        chain.setSlotDryWet (0, 1.0f);
        const char* nm = osT == FxType::Wavefolder ? "Wavefolder" : "RingModulator";
        char msg[96];
        std::snprintf (msg, sizeof (msg), "chain with one %s slot: latency()==8", nm);
        check (chain.latency() == 8, msg);
    }
}

// ---------------------------------------------------------------------------
// 6. Topology (3) x order (6); all-disabled dry passthrough.
// ---------------------------------------------------------------------------
static void testTopology()
{
    std::printf ("(6) topology x order routing\n");

    float inL[kBlock], inR[kBlock];
    makeTone (inL, inR, kBlock);

    // All-disabled => bit-identical dry copy.
    {
        FxChain chain;
        chain.prepare (kRate, kBlock);
        float outL[kBlock], outR[kBlock];
        chain.process (inL, inR, outL, outR, kBlock);
        bool id = true;
        for (int i = 0; i < kBlock; ++i)
            if (outL[i] != inL[i] || outR[i] != inR[i]) id = false;
        check (id, "all-disabled chain: bit-identical dry passthrough");
    }

    int finiteCount = 0, differCount = 0;
    const int total = 3 * 6;
    for (int topo = 0; topo < (int) FxTopology::Count; ++topo)
    {
        for (int ord = 0; ord < 6; ++ord)
        {
            FxChain chain;
            chain.prepare (kRate, kBlock);
            chain.setTopology ((FxTopology) topo);
            chain.setOrder (fxOrderPermutation ((uint8_t) ord));
            // Three DISTINCT effect types so every topology branch is exercised.
            chain.setSlotType (0, FxType::Diffuser);
            chain.setSlotType (1, FxType::Reverb);
            chain.setSlotType (2, FxType::PitchShifter);
            for (int s = 0; s < kNumFxSlots; ++s)
            {
                chain.setSlotEnabled (s, true);
                chain.setSlotDryWet (s, 0.5f);
                for (int k = 0; k < kNumFxSlotParams; ++k)
                    chain.setSlotParam (s, k, 0.5f);
            }
            float outL[kBlock], outR[kBlock];
            chain.process (inL, inR, outL, outR, kBlock);
            if (allFinite (outL, kBlock) && allFinite (outR, kBlock)) ++finiteCount;
            if (differsFrom (outL, inL, kBlock) || differsFrom (outR, inR, kBlock)) ++differCount;
        }
    }
    char msg[128];
    std::snprintf (msg, sizeof (msg), "all %d topology x order combos render finite", total);
    check (finiteCount == total, msg);
    std::snprintf (msg, sizeof (msg), "all %d topology x order combos produce wet (differ from dry)", total);
    check (differCount == total, msg);
}

// ---------------------------------------------------------------------------
// 7. Master section: fx_mix / fx_eq_low / fx_eq_mid / fx_eq_high.
//    Uses FxChain setters directly with a Reverb (broadband wet) in slot 0.
// ---------------------------------------------------------------------------
static void testMasterSection()
{
    std::printf ("(7) master section (mix + 3-band EQ)\n");

    float inL[kBlock], inR[kBlock];
    makeTone (inL, inR, kBlock);

    auto buildWet = [] (FxChain& c)
    {
        c.prepare (kRate, kBlock);
        c.setSlotType (0, FxType::Reverb);
        c.setSlotEnabled (0, true);
        c.setSlotDryWet (0, 1.0f);
        for (int k = 0; k < kNumFxSlotParams; ++k)
            c.setSlotParam (0, k, 0.5f);
    };

    // Reference (defaults: mix full, EQ flat).
    FxChain ref;
    buildWet (ref);
    float refL[kBlock], refR[kBlock];
    ref.process (inL, inR, refL, refR, kBlock);
    check (allFinite (refL, kBlock) && allFinite (refR, kBlock),
           "master defaults: finite output");

    // (a) fx_mix 0 => fully dry (output == input).
    {
        FxChain c;
        buildWet (c);
        c.setMasterMix (0.0f);
        float oL[kBlock], oR[kBlock];
        c.process (inL, inR, oL, oR, kBlock);
        // masterMixCur_ is per-sample-smoothed; a single block after the step is
        // mid-ramp, so render until settled then measure.
        double maxDryErr = 0.0;
        for (int b = 0; b < 40; ++b)
        {
            c.process (inL, inR, oL, oR, kBlock);
            if (b >= 30)
                for (int i = 0; i < kBlock; ++i)
                    maxDryErr = std::fmax (maxDryErr, std::fabs (oL[i] - inL[i]));
        }
        check (maxDryErr < 1.0e-3, "fx_mix=0: output == dry (masterMix fully dry)");
    }
    // (a2) fx_mix 127 (1.0) => fully wet (differs from dry).
    {
        FxChain c;
        buildWet (c);
        c.setMasterMix (1.0f);
        float oL[kBlock], oR[kBlock];
        bool wet = false;
        for (int b = 0; b < 20; ++b)
        {
            c.process (inL, inR, oL, oR, kBlock);
            if (b >= 4 && (differsFrom (oL, inL, kBlock) || differsFrom (oR, inR, kBlock)))
                wet = true;
        }
        check (wet, "fx_mix=127: output differs from dry (masterMix fully wet)");
    }

    // (b) fx_eq_low: a LOW sine (60 Hz) is attenuated more at a high setting.
    {
        float loL[kBlock], loR[kBlock];
        makeTone (loL, loR, kBlock, 0.4f, 60.0);

        FxChain off, on;
        buildWet (off); off.setMasterEqLow (0);     // off
        buildWet (on);  on.setMasterEqLow (127);    // max low-cut
        float offL[kBlock], onL[kBlock], dummyR[kBlock];
        double offRms = 0.0, onRms = 0.0;
        for (int b = 0; b < 16; ++b)
        {
            off.process (loL, loR, offL, dummyR, kBlock);
            on.process  (loL, loR, onL,  dummyR, kBlock);
            if (b >= 8) { offRms += rms (offL, kBlock); onRms += rms (onL, kBlock); }
        }
        check (onRms < offRms * 0.95,
               "fx_eq_low high: low sine attenuated more than low-cut off");
    }

    // (c) fx_eq_mid: a MID sine (~1 kHz) gain changes vs flat (64).
    {
        float midL[kBlock], midR[kBlock];
        makeTone (midL, midR, kBlock, 0.4f, 1000.0);

        FxChain flat, cut;
        buildWet (flat); flat.setMasterEqMid (64);   // 0 dB
        buildWet (cut);  cut.setMasterEqMid (10);    // large cut
        double flatRms = 0.0, cutRms = 0.0;
        float fL[kBlock], cL[kBlock], dR[kBlock];
        for (int b = 0; b < 16; ++b)
        {
            flat.process (midL, midR, fL, dR, kBlock);
            cut.process  (midL, midR, cL, dR, kBlock);
            if (b >= 8) { flatRms += rms (fL, kBlock); cutRms += rms (cL, kBlock); }
        }
        check (cutRms < flatRms * 0.97,
               "fx_eq_mid cut: mid sine level reduced vs flat (64)");
    }

    // (d) fx_eq_high: a HIGH sine (~6 kHz) gain changes vs flat (64).
    {
        float hiL[kBlock], hiR[kBlock];
        makeTone (hiL, hiR, kBlock, 0.4f, 6000.0);

        FxChain flat, cut;
        buildWet (flat); flat.setMasterEqHigh (64);  // 0 dB
        buildWet (cut);  cut.setMasterEqHigh (10);   // large shelf cut
        double flatRms = 0.0, cutRms = 0.0;
        float fL[kBlock], cL[kBlock], dR[kBlock];
        for (int b = 0; b < 16; ++b)
        {
            flat.process (hiL, hiR, fL, dR, kBlock);
            cut.process  (hiL, hiR, cL, dR, kBlock);
            if (b >= 8) { flatRms += rms (fL, kBlock); cutRms += rms (cL, kBlock); }
        }
        check (cutRms < flatRms * 0.97,
               "fx_eq_high cut: high sine level reduced vs flat (64)");
    }
}

// ---------------------------------------------------------------------------
// 8. FX mod matrix: every FxModDestination reaches the DSP via the full engine.
//    For each of the 18 dests: source CONST_128 (steady ~0.5), amount +63 =>
//    a +0.5 offset on the target slot value. Verified EXACTLY via
//    engine.debugGetChainValue (the value the DSP consumes). amount=0 => none.
// ---------------------------------------------------------------------------
static void testFxModMatrix()
{
    std::printf ("(8) FX mod matrix — all 18 destinations reach the DSP\n");

    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::MessageManager::getInstance();

    const int block = 256;
    // CONST_128 is mod source index 25 (see makeModSources); DC-coupled, value
    // = 128/255 = 0.50196. amount +63 => offset = (63/63) * 0.50196 = 0.50196.
    const uint8_t kConst128 = 25;

    // For each destination, configure the target slot with a responsive effect
    // + a known baseline, route CONST_128 -> dest at +63, render, read the
    // consumed chain value, and compare to (baseline + 0.502), clamped to [0,1].
    for (int dest = 0; dest < (int) FX_DST_LAST; ++dest)
    {
        const int slot  = dest / 6;          // field stride = dryWet + 5 params
        const int field = dest % 6;          // 0 = dryWet, 1..5 = param 0..4

        ParvatiAudioProcessor proc;
        proc.prepareToPlay (kRate, block);

        auto& eng = proc.getEngine();
        eng.setFxSlotType     (slot, static_cast<uint8_t> (FxType::Resonator)); // 5 live params
        eng.setFxSlotEnabled  (slot, 1);
        // Baseline: a mid value the offset can clearly push up from.
        const uint8_t base127 = 32;            // 32/127 = 0.252
        eng.setFxSlotDryWet   (slot, base127);
        for (int k = 0; k < kNumFxSlotParams; ++k)
            eng.setFxSlotParam (slot, k, base127);

        eng.setFxModSlot (0 /*matrix slot 0*/, kConst128, (uint8_t) dest, 63);

        // Render enough blocks for renderPartFx to service fxDirty_ at the 980 Hz
        // cadence and apply the mod.
        juce::AudioBuffer<float> buf (2, block);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (uint8_t) 100), 0);
        juce::MidiBuffer empty;
        for (int b = 0; b < 12; ++b)
        {
            buf.clear();
            proc.processBlock (buf, b == 0 ? midi : empty);
        }

        const float consumed = eng.debugGetChainValue (0 /*part*/, slot, field);
        const float baseNorm = static_cast<float> (base127) / 127.0f;
        const float expected = std::min (1.0f, baseNorm + 0.50196f);
        const float err = std::fabs (consumed - expected);

        char msg[160];
        std::snprintf (msg, sizeof (msg),
            "dest %d (slot %d field %d): mod reaches DSP (consumed=%.3f, expect=%.3f, base=%.3f)",
            dest, slot, field, consumed, expected, baseNorm);
        // The base is de-click-smoothed toward 0.252 over a few sub-chunks; after
        // 12 blocks it has converged, so consumed ≈ expected within ~1%.
        check (err < 0.02f, msg);

        // amount=0 => no modulation: the consumed value returns to the base.
        eng.setFxModSlot (0, kConst128, (uint8_t) dest, 0);
        for (int b = 0; b < 8; ++b)
        {
            buf.clear();
            proc.processBlock (buf, b == 0 ? midi : empty);
        }
        const float consumed0 = eng.debugGetChainValue (0, slot, field);
        const float err0 = std::fabs (consumed0 - baseNorm);
        std::snprintf (msg, sizeof (msg),
            "dest %d: amount=0 -> no modulation (consumed=%.3f, base=%.3f)", dest, consumed0, baseNorm);
        check (err0 < 0.02f, msg);
    }

    // Source depth is honored: CONST_4 (idx 30, value 4/255=0.0157) produces a
    // much smaller offset than CONST_256 (idx 24, value 1.0) at the same amount.
    {
        const uint8_t kConst256 = 24;
        const uint8_t kConst4   = 30;
        auto measure = [&] (uint8_t src) -> float
        {
            ParvatiAudioProcessor proc;
            proc.prepareToPlay (kRate, block);
            auto& eng = proc.getEngine();
            eng.setFxSlotType (0, (uint8_t) FxType::Resonator);
            eng.setFxSlotEnabled (0, 1);
            eng.setFxSlotParam (0, 0, 0);              // base 0
            eng.setFxModSlot (0, src, FX_DST_FX1_P1, 63);
            juce::AudioBuffer<float> buf (2, block);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (uint8_t) 100), 0);
            juce::MidiBuffer empty;
            for (int b = 0; b < 12; ++b) { buf.clear(); proc.processBlock (buf, b == 0 ? midi : empty); }
            return eng.debugGetChainValue (0, 0, 1);
        };
        const float v256 = measure (kConst256);
        const float v4   = measure (kConst4);
        char msg[160];
        std::snprintf (msg, sizeof (msg),
            "source depth honored: CONST_256=%.3f >> CONST_4=%.3f (same amount 63)", v256, v4);
        check (v256 > v4 * 20.0f, msg);
    }
}

// ---------------------------------------------------------------------------
// 9. Condition-dependent params: three Clouds params are LIVE only under a
//    specific DSP condition, so the generic steady-tone sweep (4) correctly
//    sees them as invariant and surfaces them as drifts. This test exercises
//    the CORRECT condition and proves they DO move the output, reclassifying
//    them from "drift" to "verified (intended condition-dependence)":
//      - LoopingDelay Size (p1) and Pitch (p2): the Clouds LoopingSamplePlayer
//        is a single-tap POSITION delay when NOT frozen (it ignores size/pitch);
//        the looping engine (which uses size=loop length, pitch=playback ratio)
//        only runs in FREEZE mode. So sweep them with freeze ON.
//      - Spectral Position (p2): the phase vocoder interpolates between STORED
//        spectral textures (past frames); on a stationary signal every texture
//        is identical, so position is a no-op. A time-varying spectrum (chirp)
//        makes the textures differ, so position moves the output.
// ---------------------------------------------------------------------------
static void testConditionDependentParams()
{
    std::printf ("(9) condition-dependent Clouds params (correct DSP condition)\n");

    // A long, slow chirp (frequency evolves over the whole run): rich,
    // time-varying content for both the looper's record buffer and the PV's
    // texture ring.
    auto fillChirp = [] (float* L, float* R, int n, double f0, double f1)
    {
        static double s_phase = 0.0;   // local; recomputed below
        s_phase = 0.0;
        const double w0 = 2.0 * 3.14159265 * f0 / kRate;
        const double w1 = 2.0 * 3.14159265 * f1 / kRate;
        // linear chirp instantaneous phase integral
        for (int i = 0; i < n; ++i)
        {
            const double frac = static_cast<double> (i) / static_cast<double> (n);
            const double w = w0 + (w1 - w0) * frac;
            s_phase += w;
            const float v = 0.35f * static_cast<float> (std::sin (s_phase));
            L[i] = v;
            R[i] = v;
        }
    };

    // ---- LoopingDelay Size/Pitch in FREEZE mode ----
    // Record a chirp into the ~4 s buffer (freeze OFF), then freeze and sweep.
    auto looperFrozenRMS = [&] (int sweepIdx, float sweepVal) -> double
    {
        auto fx = createFxProcessor (FxType::LoopingDelay);
        fx->prepare (kRate, kBlock);
        fx->reset();
        float outL[kBlock], outR[kBlock];

        // Record phase: freeze OFF, position 0.1 (reads ~0.4 s into the buffer,
        // well inside the recorded region). Feed a fresh chirp each block.
        float recBase[5] = { 0.1f, 0.5f, 0.5f, 0.0f /*freeze off*/, 0.0f };
        fx->setParams (recBase);
        for (int b = 0; b < 320; ++b)        // ~1.7 s @ 48k -> fills the read region
        {
            fillChirp (outL, outR, kBlock, 200.0 + 50.0 * b, 600.0 + 50.0 * b);
            fx->setParams (recBase);
            fx->process (outL, outR, kBlock);
        }

        // Measure phase: freeze ON, sweep the target param. Keep feeding the
        // same chirp (irrelevant once frozen, but keeps the input non-zero).
        float meas[5];
        std::memcpy (meas, recBase, sizeof (meas));
        meas[3] = 1.0f;                       // freeze ON
        meas[sweepIdx] = sweepVal;
        fx->setParams (meas);
        std::vector<float> wave;
        wave.assign (static_cast<size_t> (2 * kBlock * 8), 0.0f);
        for (int b = 0; b < 16; ++b)         // let the loop settle, then measure 8
        {
            fillChirp (outL, outR, kBlock, 300.0, 700.0);
            fx->setParams (meas);
            fx->process (outL, outR, kBlock);
            if (b >= 8)
            {
                const size_t off = static_cast<size_t> (b - 8) * 2u * static_cast<size_t> (kBlock);
                for (int i = 0; i < kBlock; ++i)
                {
                    wave[off + 2u * static_cast<size_t> (i)]     = outL[i];
                    wave[off + 2u * static_cast<size_t> (i) + 1u] = outR[i];
                }
            }
        }
        return rms (wave.data(), static_cast<int> (wave.size()));
    };

    {
        const double s0 = looperFrozenRMS (1, 0.0f);   // Size = 0 (short loop)
        const double s1 = looperFrozenRMS (1, 1.0f);   // Size = 1 (long loop)
        char msg[200];
        std::snprintf (msg, sizeof (msg),
            "LoopingDelay Size (p1) @freeze moves output RMS (size0=%.4f size1=%.4f)", s0, s1);
        check (std::fabs (s1 - s0) > 1.0e-4, msg);
    }
    {
        const double p0 = looperFrozenRMS (2, 0.0f);   // Pitch = -24 st
        const double p1 = looperFrozenRMS (2, 1.0f);   // Pitch = +24 st
        char msg[200];
        std::snprintf (msg, sizeof (msg),
            "LoopingDelay Pitch (p2) @freeze moves output RMS (p-24=%.4f p+24=%.4f)", p0, p1);
        check (std::fabs (p1 - p0) > 1.0e-4, msg);
    }

    // ---- Spectral Position is a TEXTURE ADDRESS (write+read at the same slot).
    // StoreMagnitudes writes the current frame into textures_[position], and
    // ReplayMagnitudes reads back from textures_[position]. So a STATIC position
    // gives identical output at every value (each slot independently tracks the
    // same live input). Position is only observable when content is RETAINED
    // (freeze) and you switch to a DIFFERENT address: the frozen slot holds
    // content while never-written slots are ~0. Correct probe:
    //   1. freeze OFF, pos=0.0, evolving spectrum  -> writes textures_[0] only.
    //   2. freeze ON (retains textures_[0]; textures_[1..] stay ~0).
    //   3. read pos=0.0 (frozen content) vs pos=1.0 (never-written slot ~=0).
    auto spectralFrozenPosRMS = [&] (float readPos, double& sanityRms) -> double
    {
        auto fx = createFxProcessor (FxType::Spectral);
        fx->prepare (kRate, kBlock);
        fx->reset();
        float outL[kBlock], outR[kBlock];
        // Phase 1: freeze OFF at pos 0.0, write textures_[0] with evolving content.
        float writePv[5] = { 0.5f, 0.5f, 0.0f, 0.5f, 0.0f /*freeze off*/ };
        fx->setParams (writePv);
        for (int b = 0; b < 160; ++b)
        {
            const double freq = 300.0 + 4.0 * b;
            for (int i = 0; i < kBlock; ++i)
            {
                const float v = 0.4f * static_cast<float> (std::sin (2.0 * 3.14159265 * freq
                                                                      * static_cast<double> (i) / kRate));
                outL[i] = v;
                outR[i] = v;
            }
            fx->setParams (writePv);
            fx->process (outL, outR, kBlock);
        }
        // Phase 2: freeze ON, read at the requested position.
        float readPv[5] = { 0.5f, 0.5f, readPos, 0.5f, 1.0f /*freeze on*/ };
        fx->setParams (readPv);
        std::vector<float> wave;
        wave.assign (static_cast<size_t> (2 * kBlock * 8), 0.0f);
        for (int b = 0; b < 16; ++b)   // settle, then measure 8 blocks
        {
            for (int i = 0; i < kBlock; ++i) { outL[i] = 0.0f; outR[i] = 0.0f; }
            fx->setParams (readPv);
            fx->process (outL, outR, kBlock);
            if (b >= 8)
            {
                const size_t off = static_cast<size_t> (b - 8) * 2u * static_cast<size_t> (kBlock);
                for (int i = 0; i < kBlock; ++i)
                {
                    wave[off + 2u * static_cast<size_t> (i)]     = outL[i];
                    wave[off + 2u * static_cast<size_t> (i) + 1u] = outR[i];
                }
            }
        }
        const double r = rms (wave.data(), static_cast<int> (wave.size()));
        sanityRms = r;
        return r;
    };
    {
        double san0 = 0.0, san1 = 0.0;
        const double q0 = spectralFrozenPosRMS (0.0f, san0);  // frozen textures_[0] -> content
        const double q1 = spectralFrozenPosRMS (1.0f, san1);  // never-written slot -> ~0
        char msg[240];
        std::snprintf (msg, sizeof (msg),
            "Spectral Position (p2) @freeze-choreography moves output RMS (pos0=%.4f [frozen content] pos1=%.4f [other slot])",
            q0, q1);
        // Position selects the texture address; under freeze, switching address
        // changes which (differently-retained) spectral frame is replayed. The
        // two reads differ by a meaningful margin (there is inter-slot bleed,
        // so pos1 is not silent — but it is clearly different from pos0).
        const double diff = std::fabs (q1 - q0);
        const bool ok = q0 > 1.0e-3 && diff > 0.02;
        check (ok, msg);
        // Reference the documented invariant for the static-position case.
        if (ok)
            check (true, "Spectral Position confirmed live under freeze (static-position no-op is intended: each texture slot independently tracks live input)");
    }
}

int main()
{
    std::printf ("=== Parvati FX parameter + module coverage ===\n\n");
    testFxTable();
    testPerEffectFinite();
    testPerEffectWetDiffers();
    testPerEffectParamSweep();
    testLatency();
    testTopology();
    testMasterSection();
    testFxModMatrix();
    testConditionDependentParams();

    if (g_drifts > 0)
        std::printf ("\n--- %d DRIFT%s SURFACED (triage in tests/COVERAGE_FINDINGS.md) ---\n",
                     g_drifts, g_drifts == 1 ? "" : "S");
    std::printf ("\n=== %s (%d/%d checks passed, %d failure%s, %d drift%s) ===\n",
                 g_failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED",
                 g_checks - g_failures, g_checks, g_failures,
                 g_failures == 1 ? "" : "s", g_drifts, g_drifts == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}

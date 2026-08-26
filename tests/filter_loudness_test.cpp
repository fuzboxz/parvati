// filter_loudness_test — loudness parity across the six filter cards.
//
// The six voicecard topologies play inside ONE instrument, so a patch must
// keep roughly its level when the user switches cards. This test pins the
// 2026-08-25 loudness calibration (see Source/dsp/analog_filter.h, "LOUDNESS
// PARITY"):
//
//   [1] Resonance maps. The 2-pole cards use Q = 1/(2*(1-res)); the 4-pole
//       cascade uses q = 0.5*(1-res)^(-0.616) per stage; the Ladder knob
//       passes the ladderResonanceKnob() remap (k = 4*knob above 0.1). The
//       old raw-Q mapping (Q = knob) left both cards droopy and silent at
//       low knob values. It sat up to 38 dB under the siblings. It read
//       near-flat at the maximum. All pinned at the model level. The
//       small-signal gain at the cutoff must land within ~1 dB of the
//       analytic values at res 0, res 0.5 and res 0.95. Two extra Ladder
//       pins: a res-0.95 band, and a knob-0.5 remap anchor measured against
//       the raw-knob identity law. Plus a four-card peak cluster pin
//       (<= 4 dB) at res 0.95.
//   [2] Typical-band parity. Hot program (saw 110 Hz, amp 0.9, drive 1.2)
//       across cutoff 500 Hz..6 kHz x resonance 0..0.85: the six-card RMS
//       spread must stay under 5 dB (measured: <= 4.6 dB, worst corner).
//   [3] Neutral-setting parity. Cutoff 2.5 kHz, resonance 0: spread under
//       3 dB (the "same patch, different card" case).
//   [4] Resonance extremes keep character, not chaos. At res 0.95 the ring
//       legitimately dominates, so the bound relaxes to 8 dB.
//   [5] Filter Drive parity. Drive 12 must not sink any card far below the
//       others (the old linear OTA knee law dropped the SMR4 ~16 dB; now the
//       spread stays under 8 dB, measured ~5 dB).
//
// Model-level test (direct ambika::dsp::AnalogFilter): deterministic, no
// processor or MIDI involved. Run: ./build_unified/hellcat_unified_tests
// filter_loudness_test

#include <algorithm>
#include <cmath>
#include "unified_test_runner.h"
#include <cstdio>
#include <vector>

#include <juce_dsp/juce_dsp.h>

#include "dsp/analog_filter.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg) { std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg); if (! cond) ++g_failures; }

constexpr double kFs = 48000.0;
constexpr int    kN  = 32768;   // render length; first quarter is settling
// Ladder remap anchor at knob 0.5 (see section [1]): remapped feedback is
// k = 2.0 exactly (the ideal 4-pole law). Empirically derived on this build:
// 0.77 dB. The identity law (raw knob) measures 2.09 dB, a 1.32 dB gap.
constexpr double kLadderKnob05AnchorDb = 0.77;

// Band-limited saw, 24 partials, normalized to peak ~amp (same shape as
// filter_topology_test's testSaw).
float testSaw (double hz, float amp, int i)
{
    double v = 0.0;
    for (int h = 1; h <= 24; ++h)
        v += std::sin (2.0 * juce::MathConstants<double>::pi * hz * h * double (i) / kFs) / double (h);
    return amp * static_cast<float> (v * (2.0 / juce::MathConstants<double>::pi));
}

double sawRms = 0.0;   // input RMS, filled once below

// Steady-state AC RMS of one card under the hot saw program.
double cardSawRms (ambika::dsp::FilterTopology topo, double cutoffHz, float res, float drive = 1.2f)
{
    ambika::dsp::AnalogFilter f;
    f.prepare (kFs, 64);
    f.setTopology (topo);
    f.setMode (0);
    f.setCutoffHz (static_cast<float> (cutoffHz));
    f.setResonance (res);
    f.setDrive (drive);
    f.commit();
    double sum = 0.0, mean = 0.0;
    int n = 0;
    std::vector<float> out;
    out.reserve (size_t (kN - kN / 4));
    for (int i = 0; i < kN; ++i)
    {
        if ((i % 40) == 0)
            f.commit();
        const float y = f.processSample (testSaw (110.0, 0.9f, i));
        if (i >= kN / 4) { out.push_back (y); mean += double (y); ++n; }
    }
    mean /= double (n);
    for (float v : out) { const double d = double (v) - mean; sum += d * d; }
    return std::sqrt (sum / double (n));
}

// Small-signal steady-state gain at one sine frequency (linear zone),
// NORMALIZED by the card's loudness trim: the pins compare against the
// analytic Q values, which describe the filter, not its calibration gain.
double cardSineGain (ambika::dsp::FilterTopology topo, double cutoffHz, double sineHz, float res)
{
    ambika::dsp::AnalogFilter f;
    f.prepare (kFs, 64);
    f.setTopology (topo);
    f.setMode (0);
    f.setCutoffHz (static_cast<float> (cutoffHz));
    f.setResonance (res);
    f.setDrive (1.2f);
    f.commit();
    const float trim = f.getLoudnessGain();
    double inSq = 0.0, outSq = 0.0;
    for (int i = 0; i < kN; ++i)
    {
        if ((i % 40) == 0)
            f.commit();
        const double ph = 2.0 * juce::MathConstants<double>::pi * sineHz * double (i) / kFs;
        const float x = 0.02f * static_cast<float> (std::sin (ph));
        const float y = f.processSample (x);
        if (i >= kN / 4) { inSq += double (x) * x; outSq += double (y) * double (y); }
    }
    return std::sqrt (outSq / juce::jmax (1e-30, inSq)) / juce::jmax (1e-9f, trim);
}

const ambika::dsp::FilterTopology kTopos[6] = {
    ambika::dsp::FilterTopology::FOUR_POLE_LADDER,  ambika::dsp::FilterTopology::FOUR_POLE_SSM2164,
    ambika::dsp::FilterTopology::TWO_POLE_SVF,      ambika::dsp::FilterTopology::FOUR_POLE_OTA,
    ambika::dsp::FilterTopology::TWO_POLE_POLIVOKS, ambika::dsp::FilterTopology::FOUR_POLE_IR3109
};

double spreadDb (const double rms[6])
{
    double lo = rms[0], hi = rms[0];
    for (int i = 1; i < 6; ++i) { lo = std::min (lo, rms[i]); hi = std::max (hi, rms[i]); }
    return 20.0 * std::log10 (hi / juce::jmax (1e-12, lo));
}
}  // namespace

TEST(filter_loudness_test)
{
    // Fill the input RMS once (the saw is deterministic).
    {
        double s = 0.0;
        for (int i = kN / 4; i < kN; ++i) { const double v = testSaw (110.0, 0.9f, i); s += v * v; }
        sawRms = std::sqrt (s / double (kN - kN / 4));
    }

    // ---- [1] Resonance maps: analytic small-signal gain at the cutoff ----
    std::printf ("[1] Resonance maps (gain at the cutoff, linear zone)\n");
    {
        struct Pin { ambika::dsp::FilterTopology topo; float res; double wantDb; double tol; const char* what; };
        const Pin pins[] = {
            // SVF: |H(w0)| = Q = 1/(2*(1-res)). Old broken map: -26 dB at res 0.
            { ambika::dsp::FilterTopology::TWO_POLE_SVF, 0.0f,  -6.02, 1.0, "SVF res 0    : Q 0.5   (-6.0 dB)" },
            { ambika::dsp::FilterTopology::TWO_POLE_SVF, 0.5f,   0.00, 1.0, "SVF res 0.5  : Q 1     (0.0 dB)" },
            { ambika::dsp::FilterTopology::TWO_POLE_SVF, 0.95f, 20.00, 1.5, "SVF res 0.95 : Q 10    (+20 dB)" },
            // 4P cascade: |H(w0)| = q^2 = 0.25*(1-res)^(-2*0.616). The exact
            // cascade baseline q(0) = 0.5 holds for any power law. The
            // 0.616 exponent lifts the peak onto the family cluster.
            { ambika::dsp::FilterTopology::FOUR_POLE_SSM2164, 0.0f,  -12.04, 1.0, "4P res 0    : cascade (-12.0 dB)" },
            { ambika::dsp::FilterTopology::FOUR_POLE_SSM2164, 0.5f,   -4.70, 1.0, "4P res 0.5  : q^2 0.59 (-4.7 dB)" },
            { ambika::dsp::FilterTopology::FOUR_POLE_SSM2164, 0.95f,  20.00, 1.5, "4P res 0.95 : q^2 10   (+20 dB)" },
        };
        for (const auto& p : pins)
        {
            const double g = cardSineGain (p.topo, 1000.0, 1000.0, p.res);
            const double db = 20.0 * std::log10 (juce::jmax (1e-12, g));
            char msg[128];
            std::snprintf (msg, sizeof (msg), "%s -> %.2f dB", p.what, db);
            check (std::fabs (db - p.wantDb) < p.tol, msg);
        }

        // Ladder relative pin: the remap (k = 4*knob above knob 0.1) must
        // keep the saturating ladder's peak in the family band. The JUCE
        // structure reads higher than the linear ideal (comp + saturation).
        {
            const double g  = cardSineGain (ambika::dsp::FilterTopology::FOUR_POLE_LADDER, 1000.0, 1000.0, 0.95f);
            const double db = 20.0 * std::log10 (juce::jmax (1e-12, g));
            char msg[128];
            std::snprintf (msg, sizeof (msg), "Ladder res 0.95 : peak within [18, 26] dB (got %.2f)", db);
            check (db >= 18.0 && db <= 26.0, msg);
        }

        // Ladder REMAP pin at knob 0.5 — independent of the accessor. The
        // remap sends JUCE r = 0.4444, so its internal feedback is exactly
        // k = 0.4 + 3.6*0.4444 = 2.0 (the ideal 4-pole law k = 4*knob).
        // The reference below feeds the RAW knob straight to a bare JUCE
        // ladder: k = 0.4 + 3.6*0.5 = 2.2 — the exact law a lost remap
        // regresses to. Around k = 2 the ideal feedback law C/(4-k)
        // predicts a ~0.9 dB gap; the saturating JUCE structure widens it.
        // Measured on this build: remapped 0.77 dB, identity 2.09 dB, gap
        // 1.32 dB. Two checks: the remapped peak sits at the anchor
        // (tolerance 0.5 dB), and it stays clearly BELOW the identity
        // reference (margin 0.4 dB, measured 1.32). A lost remap fails
        // BOTH: the paths measure equal and the anchor misses by 1.32 dB.
        {
            // Reference: raw JUCE ladder, knob 0.5, no remap (identity law).
            struct LadderTap : juce::dsp::LadderFilter<float>
            {
                using juce::dsp::LadderFilter<float>::processSample;
                using juce::dsp::LadderFilter<float>::updateSmoothers;
            };
            LadderTap raw;
            const juce::dsp::ProcessSpec spec { kFs, 64u, 1u };
            raw.prepare (spec);
            raw.setMode (juce::dsp::LadderFilterMode::LPF24);
            raw.setCutoffFrequencyHz (1000.0f);
            raw.setResonance (0.5f);   // RAW knob: k = 2.2, the identity law
            raw.setDrive (1.2f);
            double inSq = 0.0, outSq = 0.0;
            for (int i = 0; i < kN; ++i)
            {
                const double ph = 2.0 * juce::MathConstants<double>::pi * 1000.0 * double (i) / kFs;
                const float x = 0.02f * static_cast<float> (std::sin (ph));
                raw.updateSmoothers();
                const float y = raw.processSample (x, 0);
                if (i >= kN / 4) { inSq += double (x) * x; outSq += double (y) * double (y); }
            }
            const double identityDb = 20.0 * std::log10 (std::sqrt (outSq / juce::jmax (1e-30, inSq)));

            const double remappedDb = 20.0 * std::log10 (juce::jmax (1e-12,
                cardSineGain (ambika::dsp::FilterTopology::FOUR_POLE_LADDER, 1000.0, 1000.0, 0.5f)));
            std::printf ("     ladder knob 0.5: remapped=%.2f dB  identity(raw knob)=%.2f dB  delta=%.2f\n",
                         remappedDb, identityDb, identityDb - remappedDb);
            char msg[128];
            std::snprintf (msg, sizeof (msg), "Ladder knob 0.5 remap anchor: %.2f +- 0.5 dB (got %.2f)",
                           kLadderKnob05AnchorDb, remappedDb);
            check (std::fabs (remappedDb - kLadderKnob05AnchorDb) < 0.5, msg);
            std::snprintf (msg, sizeof (msg), "Ladder remap beats the identity law at knob 0.5 (delta %.2f >= 0.4 dB)",
                           identityDb - remappedDb);
            check (identityDb - remappedDb >= 0.4, msg);
        }

        // Peak cluster: at res 0.95 the four Q-law cards must agree. The OTA
        // cards are excluded: their ring suppression is pinned character.
        {
            const ambika::dsp::FilterTopology cluster[4] = {
                ambika::dsp::FilterTopology::FOUR_POLE_LADDER,
                ambika::dsp::FilterTopology::TWO_POLE_SVF,
                ambika::dsp::FilterTopology::FOUR_POLE_SSM2164,
                ambika::dsp::FilterTopology::TWO_POLE_POLIVOKS
            };
            double lo = 1e9, hi = -1e9;
            std::printf ("     peak cluster res 0.95 (trim-normalized dB):");
            for (auto t : cluster)
            {
                const double g  = cardSineGain (t, 1000.0, 1000.0, 0.95f);
                const double db = 20.0 * std::log10 (juce::jmax (1e-12, g));
                lo = std::min (lo, db);  hi = std::max (hi, db);
                std::printf (" %.2f", db);
            }
            std::printf ("\n");
            char msg[128];
            std::snprintf (msg, sizeof (msg), "Ladder/SVF/4P/Polivoks peak spread <= 4 dB at res 0.95 (got %.2f)", hi - lo);
            check (hi - lo <= 4.0, msg);
        }
    }

    // ---- [2] Typical-band parity: hot saw across the cutoff x res plane ----
    std::printf ("\n[2] Typical-band parity: saw 110 Hz amp 0.9, drive 1.2 (spread in dB, bound 5.0)\n");
    {
        bool allOk = true;
        for (double hz : { 500.0, 1000.0, 2500.0, 6000.0 })
        {
            for (float res : { 0.0f, 0.3f, 0.5f, 0.7f, 0.85f })
            {
                double rms[6];
                for (int c = 0; c < 6; ++c)
                    rms[c] = cardSawRms (kTopos[c], hz, res);
                const double sp = spreadDb (rms);
                std::printf ("     cutoff %5.0f res %.2f: spread %5.2f dB  (", hz, res, sp);
                for (int c = 0; c < 6; ++c)
                    std::printf ("%s%.1f ", c == 0 ? "" : " ", 20.0 * std::log10 (juce::jmax (1e-12, rms[c])));
                std::printf (")\n");
                if (sp > 5.0) allOk = false;
            }
        }
        check (allOk, "six-card RMS spread <= 5 dB across the typical band (500 Hz..6 kHz x res 0..0.85)");
    }

    // ---- [3] Neutral-setting parity: same patch, different card ----
    std::printf ("\n[3] Neutral setting: cutoff 2.5 kHz, resonance 0 (bound 3.0 dB)\n");
    {
        double rms[6];
        for (int c = 0; c < 6; ++c)
            rms[c] = cardSawRms (kTopos[c], 2500.0, 0.0f);
        const double sp = spreadDb (rms);
        std::printf ("     spread %.2f dB\n", sp);
        char msg[96];
        std::snprintf (msg, sizeof (msg), "six-card RMS spread <= 3 dB at the neutral setting (got %.2f)", sp);
        check (sp <= 3.0, msg);
    }

    // ---- [4] Resonance extreme: the ring dominates, bound relaxes ----
    std::printf ("\n[4] Resonance 0.95: ring character, bound 8.0 dB\n");
    {
        bool allOk = true;
        for (double hz : { 1000.0, 2500.0 })
        {
            double rms[6];
            for (int c = 0; c < 6; ++c)
                rms[c] = cardSawRms (kTopos[c], hz, 0.95f);
            const double sp = spreadDb (rms);
            std::printf ("     cutoff %5.0f res 0.95: spread %5.2f dB\n", hz, sp);
            if (sp > 8.0) allOk = false;
        }
        check (allOk, "six-card RMS spread <= 8 dB at resonance 0.95 (ring character band)");
    }

    // ---- [5] Filter Drive parity: drive 12 must not sink a card ----
    std::printf ("\n[5] Filter Drive 12: cutoff 1 kHz, resonance 0 (bound 8.0 dB)\n");
    {
        double rms[6];
        for (int c = 0; c < 6; ++c)
            rms[c] = cardSawRms (kTopos[c], 1000.0, 0.0f, 12.0f);
        const double sp = spreadDb (rms);
        std::printf ("     spread %.2f dB (old linear-knee law: ~15 dB)\n", sp);
        char msg[96];
        std::snprintf (msg, sizeof (msg), "six-card RMS spread <= 8 dB at Filter Drive 12 (got %.2f)", sp);
        check (sp <= 8.0, msg);
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "FILTER LOUDNESS TEST: FAILURES" : "FILTER LOUDNESS TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}

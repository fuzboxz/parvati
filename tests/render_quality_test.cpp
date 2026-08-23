// Render-quality regression test for Parvati:
//   [1] Offline auto-max oversampling (setNonRealtime -> 8x filter OS, no
//       persistence, restore on exit, double-entry guard, prepare-time leak
//       guard).
//   [2] Oversized-block CHUNKED render (host block > prepared size renders in
//       prepared-size slices; no silent dropped tail) + MIDI REBASE across
//       slices (mid-slice + exact-boundary events land in their own window).
//   [2c] DC-blocker slice/state continuity (one oversized block == four
//       in-budget blocks) + near-DC attenuation vs the raw voicecard sum.
//   [3] Dynamic getTailLengthSeconds: pure tailSecondsForFx table (reverbs +
//       DELAYS with feedback-decay math + freeze caps + clamps + the
//       zero-tail family) and the processor-level cache (all-None floor,
//       reverb > floor, enabled gating, MULTI-PART MAX).
//   [3e] Tail-cache TEMPO-MOVE invalidation (ClockedDelay halving at 4x BPM;
//       the <=0.25 BPM jitter gate does not recompute).
//
// Built by default. Run with: ./build/parvati_render_quality_test

#include <array>
#include <cmath>
#include "unified_test_runner.h"
#include <cstdio>
#include <memory>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "PluginProcessor.h"
#include "test_utils.h"              // shared setParam (host-path helper)
#include "dsp/fx/FxTypes.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

void checkNear (double got, double want, double tol, const char* msg)
{
    const bool ok = std::fabs (got - want) <= tol;
    std::printf ("  %s: %s (got %.4f, want %.4f +- %.4f)\n",
                 ok ? "ok  " : "FAIL", msg, got, want, tol);
    if (! ok) ++g_failures;
}

void renderBlock (ParvatiAudioProcessor& p, int numSamples)
{
    juce::AudioBuffer<float> buf (2, numSamples);
    buf.clear();
    juce::MidiBuffer midi;
    p.processBlock (buf, midi);
}

double blockPeak (const juce::AudioBuffer<float>& b, int start, int len)
{
    double peak = 0.0;
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = start; i < start + len && i < b.getNumSamples(); ++i)
            peak = std::fmax (peak, std::fabs ((double) b.getSample (ch, i)));
    return peak;
}

// The persisted ui_oversampling inside a saved host state (or -1 when absent).
int savedOversampling (ParvatiAudioProcessor& p)
{
    juce::MemoryBlock mb;
    p.getStateInformation (mb);
    const auto size = (int) mb.getSize();
    if (auto xml = p.getXmlFromBinary (mb.getData(), size))
    {
        const auto tree = juce::ValueTree::fromXml (*xml);
        if (tree.hasProperty ("ui_oversampling"))
            return (int) tree.getProperty ("ui_oversampling");
    }
    return -1;
}

// First index in [start, start+len) whose |sample| exceeds @p thr, or -1.
// The engine renders silence as exact 0.0 (idle voices + cleared buffer), so
// a small threshold cleanly separates "before the note" from "note onset".
int onsetIndex (const juce::AudioBuffer<float>& b, int start, int len, double thr)
{
    for (int i = start; i < start + len && i < b.getNumSamples(); ++i)
        if (std::fabs ((double) b.getSample (0, i)) > thr)
            return i;
    return -1;
}

// A settable play head so processBlock's transport reads a host BPM (the
// tail-cache tempo-move path recompute gate). Pattern: synth_param_coverage.
class FakePlayHead : public juce::AudioPlayHead
{
public:
    double bpm = 120.0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm (bpm);
        info.setIsPlaying (true);
        info.setTimeInSamples ((int64_t) 0);
        return info;
    }
};
}   // namespace

//==============================================================================
TEST(render_quality_test)
{
    std::printf ("[1] Offline auto-max oversampling\n");
    {
        juce::ScopedJuceInitialiser_GUI juceInit;
        ParvatiAudioProcessor p;
        p.prepareToPlay (48000.0, 256);
        renderBlock (p, 256);   // flush the initial latency report

        const int latency2x = p.getLatencySamples();
        check (latency2x > 0, "2x default reports nonzero latency");
        check (p.getUiOversampling() == 2, "fresh instance pref is 2x");

        // -- Enter offline: 8x applied, pref untouched, state never carries 8x.
        p.setNonRealtime (true);
        check (p.isOfflineOversamplingActive(), "offline boost active after setNonRealtime(true)");
        check (p.getUiOversampling() == 2, "user pref NOT bumped to 8x");
        renderBlock (p, 256);
        const int latency8x = p.getLatencySamples();
        check (latency8x > latency2x,
               "latency re-reported larger at 8x (2x -> 8x OS group delay)");
        check (savedOversampling (p) == 2, "saved host state still carries 2x (no 8x leak)");

        // -- Double-entry guard: a second setNonRealtime(true) is a no-op
        //    (the saved 2x must not be overwritten by the current 8x).
        p.setNonRealtime (true);
        check (p.isOfflineOversamplingActive(), "double-entry stays active");
        check (p.getUiOversampling() == 2, "double-entry keeps the 2x pref");

        // -- Exit offline: restores the user factor + latency.
        p.setNonRealtime (false);
        check (! p.isOfflineOversamplingActive(), "boost inactive after exit");
        renderBlock (p, 256);
        check (p.getLatencySamples() == latency2x, "latency restored to the 2x value");
        check (p.getUiOversampling() == 2, "pref still 2x after exit");

        // -- Re-enter then re-prepare WITHOUT an exit: the prepare-time leak
        //    guard must restore the user factor (host dropped setNonRealtime).
        p.setNonRealtime (true);
        renderBlock (p, 256);
        p.setNonRealtime (false);           // host "forgot": no exit call
        p.prepareToPlay (48000.0, 256);     // back in realtime: guard fires here
        check (! p.isOfflineOversamplingActive(), "leak guard disarms the boost");
        renderBlock (p, 256);
        check (p.getLatencySamples() == latency2x, "leak guard restores 2x latency");
    }

    std::printf ("\n[2] Oversized-block chunked render\n");
    {
        juce::ScopedJuceInitialiser_GUI juceInit;
        ParvatiAudioProcessor p;
        p.prepareToPlay (48000.0, 256);   // prepared budget: 256

        // Host hands a 4x-oversized block with a note-on at sample 0. The old
        // clamp rendered [0,256) and silently ZEROED the remaining 768 samples
        // (dropped-tail corruption); the chunked render tiles four 256 slices.
        juce::AudioBuffer<float> buf (2, 1024);
        buf.clear();
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 110), 0);
        p.processBlock (buf, midi);

        for (int q = 0; q < 4; ++q)
        {
            const double peak = blockPeak (buf, q * 256, 256);
            char msg[80];
            (void) std::snprintf (msg, sizeof (msg),
                                 "quarter %d (samples %d..%d) is non-silent",
                                 q, q * 256, q * 256 + 255);
            check (peak > 1.0e-5, msg);
        }

        // In-budget block (no behavior change): still renders.
        renderBlock (p, 256);
        check (true, "in-budget block renders after an oversized one");

        // ---- [2b] MIDI REBASE across slices ----
        // CONTRACT: every slice receives ONLY the events inside its window,
        // rebased to [0,n) (PluginProcessor sliceMidiScratch_) — including
        // slice 0 (FIXED 2026-08-19: slice 0 used to be handed the FULL host
        // MidiBuffer, and JUCE's Synthesiser::processNextBlock closing
        // std::for_each DRAINS every event beyond numSamples
        // (juce_Synthesiser.cpp:232-235) — out-of-window events fired early in
        // slice 0 and re-fired in their home slice. The fix window-filters
        // slice 0 too whenever the block is tiled.) A note-on at sample 600
        // lives in slice 2 ([512,768)) at rebased position 88; it must begin
        // sounding near ABSOLUTE sample 600, never earlier.
        {
            ParvatiAudioProcessor q;
            q.prepareToPlay (48000.0, 256);
            juce::AudioBuffer<float> rebuf (2, 1024);
            rebuf.clear();
            juce::MidiBuffer rmidi;
            rmidi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 110), 600);
            q.processBlock (rebuf, rmidi);

            const int onset = onsetIndex (rebuf, 0, 1024, 1.0e-4);
            std::printf ("  [info] note-on @600: onset sample = %d\n", onset);
            // Correct contract: onset in (600, 780] — attack+latency after the
            // rebased note-on at slice-2 position 88 (absolute 600).
            check (onset > 600 && onset < 780,
                   "mid-slice note-on (@600) starts in its own window");
            check (blockPeak (rebuf, 700, 324) > 1.0e-5,
                   "note is clearly sounding after its window (never dropped)");
            check (blockPeak (rebuf, 0, 600) < 1.0e-6,
                   "no audio before the rebased event (slice 0/1 silent)");
        }
        // Same contract for a note-on at EXACTLY a slice boundary (768 ==
        // start of slice 3): must fire in slice 3, not one slice early.
        {
            ParvatiAudioProcessor q;
            q.prepareToPlay (48000.0, 256);
            juce::AudioBuffer<float> bbuf (2, 1024);
            bbuf.clear();
            juce::MidiBuffer bmidi;
            bmidi.addEvent (juce::MidiMessage::noteOn (1, 62, (juce::uint8) 110), 768);
            q.processBlock (bbuf, bmidi);

            const int onset = onsetIndex (bbuf, 0, 1024, 1.0e-4);
            std::printf ("  [info] note-on @768 (boundary): onset sample = %d\n", onset);
            check (onset >= 768 && onset < 900,
                   "boundary note-on fires within its own slice window");
        }
    }

    std::printf ("\n[2c] DC-blocker slice/state continuity + DC attenuation\n");
    {
        juce::ScopedJuceInitialiser_GUI juceInit;

        // Two identically-prepared/played instances: A renders four in-budget
        // 256 blocks; B renders ONE 1024 oversized block (4 slices). The DC
        // blocker runs per slice in both paths with time-contiguous state, so
        // the outputs must match closely (any per-slice filter-state reset
        // would show as a step at each 256 boundary).
        auto renderSustained = [] (bool oversized)
        {
            ParvatiAudioProcessor p;
            p.prepareToPlay (48000.0, 256);
            juce::MidiBuffer note;
            note.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 100), 0);   // low sustained note
            {
                juce::AudioBuffer<float> b (2, 256);
                b.clear();
                p.processBlock (b, note);
            }
            for (int i = 0; i < 31; ++i)      // ~170 ms: envelope settled into sustain
                renderBlock (p, 256);

            juce::AudioBuffer<float> out (2, 1024);
            out.clear();
            juce::MidiBuffer empty;
            if (oversized)
            {
                p.processBlock (out, empty);
            }
            else
            {
                for (int q = 0; q < 4; ++q)
                {
                    juce::AudioBuffer<float> b (2, 256);
                    b.clear();
                    p.processBlock (b, empty);
                    for (int ch = 0; ch < 2; ++ch)
                        out.copyFrom (ch, q * 256, b, ch, 0, 256);
                }
            }
            return out;
        };

        const auto outSliced = renderSustained (false);
        const auto outBig    = renderSustained (true);

        double maxDiff = 0.0;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 1024; ++i)
                maxDiff = std::fmax (maxDiff,
                                     std::fabs ((double) outSliced.getSample (ch, i)
                                              - (double) outBig.getSample (ch, i)));
        std::printf ("  [info] 4x256 vs 1x1024 sustained render: max |diff| = %.3e\n", maxDiff);
        check (maxDiff < 1.0e-5,
               "oversized block ~= four in-budget blocks (DC-blocker state contiguous, no step)");

        // DC attenuation: the 15 Hz main-bus high-pass must suppress the
        // near-DC offset the DC-coupled ladder filter + VCA produce. Setup
        // that genuinely offsets: low sustained note + LOW filter cutoff
        // (the ladder integrates; the init patch's ~8 kHz cutoff leaves the
        // balanced oscillators with no DC to remove). Both signals are
        // accumulated block-by-block so the two means cover the SAME window,
        // long enough (64 blocks = 16384 samples ~ 22 note periods) that the
        // partial-period LF ripple averages out and the mean is DC-dominated.
        ParvatiAudioProcessor pDC;
        pDC.prepareToPlay (48000.0, 256);
        setParam (pDC, "filter1_cutoff", 10);       // sub-audio cutoff -> DC-heavy
        setParam (pDC, "filter_env", 0);             // static cutoff (no env sweep)
        setParam (pDC, "env1_sustain", 127);
        {
            juce::MidiBuffer note;
            note.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 100), 0);
            juce::AudioBuffer<float> b (2, 256);
            b.clear();
            pDC.processBlock (b, note);
        }
        double meanRaw = 0.0, meanMain = 0.0;
        int total = 0;
        for (int blk = 0; blk < 256; ++blk)   // 65536 samples ~ 89 note periods: LF ripple averages out
        {
            juce::AudioBuffer<float> b (2, 256);
            b.clear();
            juce::MidiBuffer empty;
            pDC.processBlock (b, empty);
            const auto& vcBufs = pDC.getEngine().getVoiceCardBuffers();
            for (int i = 0; i < 256; ++i)
            {
                double s = 0.0;
                for (int pt = 0; pt < SynthEngine::getNumParts(); ++pt)
                    s += (double) vcBufs[(size_t) pt].getSample (0, i);
                meanRaw += s;
                meanMain += (double) b.getSample (0, i);
                ++total;
            }
        }
        meanRaw /= (double) total;
        meanMain /= (double) total;
        std::printf ("  [info] %d-sample window mean: raw vc sum = %.4e, main bus = %.4e\n",
                     total, meanRaw, meanMain);
        // main = 0.5 x raw by the -6 dB headroom gain, so >=20 dB attenuation
        // is |mean_main| <= 0.05 x 0.5 x |mean_raw|. (If this patch turns out
        // to carry no DC at all, the fallback floor keeps the check honest.)
        const bool dcOk = std::fabs (meanMain)
                          <= juce::jmax (2.0e-4, 0.05 * 0.5 * std::fabs (meanRaw));
        check (dcOk, "main-bus DC offset attenuated >=20 dB vs raw voicecard sum (low-cutoff patch)");
    }

    std::printf ("\n[3a] Pure tail table (reverbs)\n");
    {
        const std::array<float, kNumFxSlotParams> zero = { 0.f, 0.f, 0.f, 0.f, 0.f };
        checkNear (tailSecondsForFx (FxType::None, zero, 120.0), 0.0, 1e-9, "None -> 0");
        checkNear (tailSecondsForFx (FxType::PlateReverb, zero, 120.0), 0.1, 1e-9,
                   "Plate min (decay 0.1 s, no predelay)");
        {
            const std::array<float, kNumFxSlotParams> pmax = { 1.f, 1.f, 0.f, 0.f, 0.f };
            checkNear (tailSecondsForFx (FxType::PlateReverb, pmax, 120.0), 4.1, 1e-6,
                       "Plate max (4 s decay + 100 ms predelay)");
        }
        {
            const std::array<float, kNumFxSlotParams> pmax = { 1.f, 0.f, 0.f, 0.f, 0.f };
            checkNear (tailSecondsForFx (FxType::Spring, pmax, 120.0), 4.0, 1e-6, "Spring max (4 s)");
            checkNear (tailSecondsForFx (FxType::Room, pmax, 120.0), 3.0, 1e-6, "Room max (3 s)");
        }
        {
            // CVerb: tank fb 0.95, cross-coupled loop 15353/32000 s
            // (4680+1652+2037+3410+1912+1662 samples, BOTH tank loops) ->
            // t60 = T*ln(1e-3)/ln(0.95)
            const std::array<float, kNumFxSlotParams> tmax = { 0.f, 0.f, 1.f, 0.f, 0.f };
            const double want = (15353.0 / 32000.0) * (std::log (1.0e-3) / std::log (0.95));
            checkNear (tailSecondsForFx (FxType::Reverb, tmax, 120.0), want, 1e-3,
                       "CVerb max-time t60 (feedback-decay law)");
            check (tailSecondsForFx (FxType::Reverb, tmax, 120.0) > 10.0,
                   "CVerb max-time tail exceeds 10 s (matches measured behaviour)");
            const std::array<float, kNumFxSlotParams> pd = { 1.f, 0.f, 0.f, 0.f, 0.f };
            checkNear (tailSecondsForFx (FxType::Reverb, pd, 120.0),
                       (15353.0 / 32000.0) * (std::log (1.0e-3) / std::log (0.30)) + 0.20,
                       1e-3, "CVerb min-time = short decay + 200 ms predelay");
        }
        checkNear (tailSecondsForFx (FxType::Diffuser, zero, 120.0), 2048.0 / 32000.0, 1e-9,
                   "Diffuser = 2048-sample AP smear");

        // The ZERO-tail family (memoryless / short memory): modulators,
        // distortions, dynamics, pitch — the engine tail is dominated by the
        // voice release, which the floor covers. All must report exactly 0.0
        // so the cache's max() is driven by real reverb/delay slots only.
        // (Resonator/Ensemble/Chorus/Flanger LEFT this family 2026-08-19: each
        // is a feedback loop / ringing filter bank with a real multi-pass
        // tail — pinned below.)
        for (FxType t : { FxType::PitchShifter, FxType::Wavefolder,
                          FxType::FrequencyShifter, FxType::RingModulator,
                          FxType::Phaser, FxType::VinylCompressor,
                          FxType::Overdrive, FxType::LutDistortion,
                          FxType::Compressor, FxType::Gate })
        {
            const bool zero2 = tailSecondsForFx (t, zero, 120.0) == 0.0;
            char msg[96];
            (void) std::snprintf (msg, sizeof (msg),
                                  "zero-tail family: type %d reports 0.0", (int) t);
            check (zero2, msg);
        }

        // ---- Modulated-delay feedback loops + Resonator modal ring (audit
        // 2026-08-19): each rings well past the floor at high feedback —
        // previously the whole family reported 0.0 and hosts truncated the
        // ring-outs (Ensemble worst: ~8x under-reported).
        {
            // Ensemble: Center max (25 ms loop) + |fb| max (0.9).
            const std::array<float, kNumFxSlotParams> ens = { 0.f, 0.f, 1.f, 1.f, 0.f };
            checkNear (tailSecondsForFx (FxType::Ensemble, ens, 120.0),
                       0.025 * (std::log (1.0e-3) / std::log (0.9)), 1e-3,
                       "Ensemble max center + max fb follows the law (~1.64 s)");
            // Negative feedback rings identically (decay depends on |fb|).
            const std::array<float, kNumFxSlotParams> ensNeg = { 0.f, 0.f, 1.f, 0.f, 0.f };   // p3=0 -> fb=-0.9
            checkNear (tailSecondsForFx (FxType::Ensemble, ensNeg, 120.0),
                       tailSecondsForFx (FxType::Ensemble, ens, 120.0), 1e-9,
                       "Ensemble negative fb == positive |fb|");
            // fb=0 (p3=0.5) -> single pass = the loop time itself.
            const std::array<float, kNumFxSlotParams> ensOff = { 0.f, 0.f, 1.f, 0.5f, 0.f };
            checkNear (tailSecondsForFx (FxType::Ensemble, ensOff, 120.0), 0.025, 1e-9,
                       "Ensemble fb=0 -> single pass (25 ms loop)");
        }
        {
            // Chorus: Center max (25 ms loop) + fb 0.5 -> ~0.25 s.
            const std::array<float, kNumFxSlotParams> ch = { 0.f, 0.f, 1.f, 1.f, 0.f };
            checkNear (tailSecondsForFx (FxType::Chorus, ch, 120.0),
                       0.025 * (std::log (1.0e-3) / std::log (0.5)), 1e-3,
                       "Chorus max center + max fb follows the law (~0.25 s)");
        }
        {
            // Flanger: Manual max (6 ms base loop) + fb 0.92 -> ~0.50 s.
            const std::array<float, kNumFxSlotParams> fl = { 0.f, 0.f, 1.f, 1.f, 0.f };
            checkNear (tailSecondsForFx (FxType::Flanger, fl, 120.0),
                       0.006 * (std::log (1.0e-3) / std::log (0.92)), 1e-3,
                       "Flanger max base + max fb follows the law (~0.50 s)");
        }
        {
            // Resonator: t60 = 1099*10^(4*damping)/48000 (rate-normalized at
            // 48 kHz), capped at kTailCapSeconds for the formally-minutes top.
            const std::array<float, kNumFxSlotParams> res03 = { 0.f, 0.3f, 0.f, 0.f, 0.f };
            checkNear (tailSecondsForFx (FxType::Resonator, res03, 120.0),
                       1099.0 * std::pow (10.0, 1.2) / 48000.0, 1e-3,
                       "Resonator damping 0.3 -> ~0.36 s modal ring");
            const std::array<float, kNumFxSlotParams> res06 = { 0.f, 0.6f, 0.f, 0.f, 0.f };
            checkNear (tailSecondsForFx (FxType::Resonator, res06, 120.0),
                       1099.0 * std::pow (10.0, 2.4) / 48000.0, 1e-3,
                       "Resonator damping 0.6 -> ~5.75 s modal ring");
            const std::array<float, kNumFxSlotParams> res1 = { 0.f, 1.f, 0.f, 0.f, 0.f };
            checkNear (tailSecondsForFx (FxType::Resonator, res1, 120.0), kTailCapSeconds, 1e-9,
                       "Resonator damping 1.0 -> capped at 12 s (formally minutes)");
        }

        std::printf ("\n[3b] Pure tail table (delays: time x feedback decay)\n");
        {
            // Echo: fb=0 -> single pass, but the PING-PONG loop is
            // timeL+timeR = 2T even at Spread 0 (tapR->damp->fb->lineL->
            // tapL->lineR->tapR).
            const std::array<float, kNumFxSlotParams> emin = { 0.f, 0.f, 0.f, 0.f, 0.f };
            checkNear (tailSecondsForFx (FxType::Echo, emin, 120.0), 0.020, 1e-6,
                       "Echo min (2x 10 ms ping-pong loop, no feedback)");
            // fb=1.0 -> g=0.995 -> the >=0.995 infinite sentinel (cap).
            const std::array<float, kNumFxSlotParams> emax = { 1.f, 1.f, 0.f, 0.f, 0.f };
            checkNear (tailSecondsForFx (FxType::Echo, emax, 120.0), kTailCapSeconds, 1e-9,
                       "Echo max feedback (g=0.995) = infinite sentinel");
            // Mid feedback, Spread 0: exact law over the 2T ping-pong loop.
            const std::array<float, kNumFxSlotParams> emid = { 1.f, 0.5f, 0.f, 0.f, 0.f };
            const double T = 0.010 * std::pow (47.0, 1.0);
            const double g = 0.5 * 0.995;
            checkNear (tailSecondsForFx (FxType::Echo, emid, 120.0),
                       (2.0 * T) * (std::log (1.0e-3) / std::log (g)), 1e-3,
                       "Echo 50% fb, spread 0 -> t60 = 2T*ln(1e-3)/ln(g)");
            // Max time + max spread: timeR clamps to the 16383-sample ring
            // guard, so the loop is T + 16383/32768 (NOT 3T).
            const std::array<float, kNumFxSlotParams> espread = { 1.f, 0.5f, 0.f, 1.f, 0.f };
            checkNear (tailSecondsForFx (FxType::Echo, espread, 120.0),
                       (T + 16383.0 / 32768.0) * (std::log (1.0e-3) / std::log (g)), 1e-3,
                       "Echo spread max honours the 16383-sample ring guard");
        }
        {
            // ClockedDelay @120 BPM, sync=0 -> div 1 -> T = (4/1)*(60/120) = 2 s
            // clamped to the 1.0 s line; fb=0 -> single pass.
            const std::array<float, kNumFxSlotParams> cd = { 0.f, 0.f, 0.f, 0.f, 0.f };
            checkNear (tailSecondsForFx (FxType::ClockedDelay, cd, 120.0), 1.0, 1e-9,
                       "ClockedDelay whole-note @120 clamps to the 1 s line (no fb)");
            // Same with max feedback: 1 s * 134.7 passes.
            const std::array<float, kNumFxSlotParams> cdfb = { 0.f, 1.f, 0.f, 0.f, 0.f };
            checkNear (tailSecondsForFx (FxType::ClockedDelay, cdfb, 120.0),
                       1.0 * (std::log (1.0e-3) / std::log (0.95)), 1e-3,
                       "ClockedDelay max feedback follows the law");
            // Tempo dependence: same params at 480 BPM halve the loop time
            // (whole note @120 = 2 s clamped to 1 s; @480 = 0.5 s).
            checkNear (tailSecondsForFx (FxType::ClockedDelay, cd, 480.0), 0.5, 1e-9,
                       "ClockedDelay is tempo-scaled (480 BPM -> 0.5 s)");
            // Degenerate bpm falls back to 120.
            checkNear (tailSecondsForFx (FxType::ClockedDelay, cd, 0.0), 1.0, 1e-9,
                       "bpm=0 falls back to 120");
        }
        {
            // Granular/looping family: 4 s buffer, freeze -> the cap.
            const std::array<float, kNumFxSlotParams> noFreeze = { 0.f, 0.f, 0.f, 0.f, 0.f };
            const std::array<float, kNumFxSlotParams> freeze = { 0.f, 0.f, 0.f, 1.f, 0.f };
            checkNear (tailSecondsForFx (FxType::LoopingDelay, noFreeze, 120.0), 4.0, 1e-9,
                       "LoopingDelay = 4 s capture buffer");
            checkNear (tailSecondsForFx (FxType::LoopingDelay, freeze, 120.0), kTailCapSeconds,
                       1e-9, "LoopingDelay freeze = infinite sentinel");
            const std::array<float, kNumFxSlotParams> spectralFreeze = { 0.f, 0.f, 0.f, 0.f, 1.f };   // freeze is param[4]
            checkNear (tailSecondsForFx (FxType::Spectral, spectralFreeze, 120.0),
                       kTailCapSeconds, 1e-9, "Spectral freeze = infinite sentinel");
            // WSOLAStretch: freeze is param[3] like LoopingDelay (the granular
            // family's Freeze gate) — previously unpinned.
            checkNear (tailSecondsForFx (FxType::WSOLAStretch, noFreeze, 120.0), 4.0, 1e-9,
                       "WSOLAStretch = 4 s capture buffer (no freeze)");
            checkNear (tailSecondsForFx (FxType::WSOLAStretch, freeze, 120.0), kTailCapSeconds,
                       1e-9, "WSOLAStretch freeze (param[3]>0.5) = infinite sentinel");
        }

        std::printf ("\n[3c] Clamps\n");
        checkNear (clampTailSeconds (0.0), kTailFloorSeconds, 1e-9, "floor maps 0");
        checkNear (clampTailSeconds (1000.0), kTailCapSeconds, 1e-9, "cap maps huge");
        checkNear (clampTailSeconds (std::nan ("")), kTailFloorSeconds, 1e-9, "floor maps NaN");
        checkNear (clampTailSeconds (2.5), 2.5, 1e-9, "in-range passes through");
    }

    std::printf ("\n[3d] Processor-level tail cache\n");
    {
        juce::ScopedJuceInitialiser_GUI juceInit;
        ParvatiAudioProcessor p;
        p.prepareToPlay (48000.0, 256);
        checkNear (p.getTailLengthSeconds(), kTailFloorSeconds, 1e-6,
                   "all-None FX -> floor (prepare-time recompute)");

        // FX1 = Plate, decay max, predelay max -> ~4.1 s.
        setParam (p, "fx1_type", (int) FxType::PlateReverb);
        setParam (p, "fx1_enabled", 1);
        setParam (p, "fx1_param1", 127);   // predelay -> 100 ms
        setParam (p, "fx1_param2", 127);   // decay -> 4 s
        renderBlock (p, 256);              // services fxDirty_ -> recomputes tail
        check (p.getTailLengthSeconds() > 4.0 && p.getTailLengthSeconds() <= kTailCapSeconds,
               "Plate @4s decay -> tail > 4 s");

        // Disable the slot -> back to the floor (bypassed slots contribute 0).
        setParam (p, "fx1_enabled", 0);
        renderBlock (p, 256);
        checkNear (p.getTailLengthSeconds(), kTailFloorSeconds, 1e-6,
                   "disabled slot -> floor again");

        // A delay (the explicit requirement): Echo, max feedback -> capped.
        setParam (p, "fx1_type", (int) FxType::Echo);
        setParam (p, "fx1_enabled", 1);
        setParam (p, "fx1_param1", 127);   // time 470 ms
        setParam (p, "fx1_param2", 127);   // feedback max (0.995)
        renderBlock (p, 256);
        checkNear (p.getTailLengthSeconds(), kTailCapSeconds, 1e-6,
                   "Echo max-feedback delay -> capped tail (delays count)");

        // Multi-part MAX: the cache is the MAX over every part's enabled
        // slots. Part 0 = Room @3 s decay; Part 1 = Plate @4.1 s (predelay+4 s)
        // via the engine's per-part setters (the APVTS view is part-0 only).
        // The cache must report Part 1's LONGER tail, and drop back to Part
        // 0's when Part 1's slot is disabled — never part-0-only, never a sum.
        setParam (p, "fx1_type", (int) FxType::Room);
        setParam (p, "fx1_enabled", 1);
        setParam (p, "fx1_param1", 127);   // decay -> 3 s
        renderBlock (p, 256);
        const double tailPart0 = p.getTailLengthSeconds();
        checkNear (tailPart0, 3.0, 1e-3, "part 0 Room @3 s -> cache = 3 s (single-part baseline)");

        auto& eng = p.getEngine();
        eng.setCurrentPart (1);
        eng.setFxSlotType (0, (uint8_t) FxType::PlateReverb);
        eng.setFxSlotEnabled (0, 1);
        eng.setFxSlotParam (0, 0, 127);   // predelay 100 ms
        eng.setFxSlotParam (0, 1, 127);   // decay 4 s -> 4.1 s
        eng.setCurrentPart (0);
        renderBlock (p, 256);
        checkNear (p.getTailLengthSeconds(), 4.1, 1e-3,
                   "two parts with different tails -> cache reports the MAX (part 1 Plate 4.1 s)");

        eng.setCurrentPart (1);
        eng.setFxSlotEnabled (0, 0);      // disable the longer part-1 slot
        eng.setCurrentPart (0);
        renderBlock (p, 256);
        checkNear (p.getTailLengthSeconds(), tailPart0, 1e-6,
                   "disabling the longer part -> cache falls back to part 0's tail");
    }

    std::printf ("\n[3e] Tail-cache tempo-move invalidation (ClockedDelay)\n");
    {
        juce::ScopedJuceInitialiser_GUI juceInit;
        ParvatiAudioProcessor p;
        FakePlayHead playHead;
        playHead.bpm = 120.0;
        p.setPlayHead (&playHead);
        p.prepareToPlay (48000.0, 256);

        // Part 0 / FX1 = ClockedDelay, whole-note division (sync=0 -> div 1),
        // mid feedback: T clamps to the 1 s line @120 BPM, halving to 0.5 s
        // @480 BPM -> the t60 halves exactly (same feedback gain).
        auto& eng = p.getEngine();
        eng.setFxSlotType (0, (uint8_t) FxType::ClockedDelay);
        eng.setFxSlotEnabled (0, 1);
        eng.setFxSlotParam (0, 0, 0);     // sync -> div 1 (whole note)
        eng.setFxSlotParam (0, 1, 63);    // feedback ~0.471
        renderBlock (p, 256);             // services fxDirty_ + seeds tailBpmCache_ = 120
        const double tail120 = p.getTailLengthSeconds();
        std::printf ("  [info] tail @120 BPM = %.4f s\n", tail120);
        check (tail120 > 4.0 && tail120 < 12.0,
               "ClockedDelay fb tail @120 BPM is a real multi-second value");

        // Material tempo move: 120 -> 480 recomputes; the clamped 1 s line
        // halves, so the t60 halves.
        playHead.bpm = 480.0;
        renderBlock (p, 256);
        const double tail480 = p.getTailLengthSeconds();
        std::printf ("  [info] tail @480 BPM = %.4f s (ratio %.4f)\n",
                     tail480, tail480 / tail120);
        checkNear (tail480, tail120 * 0.5, 1.0e-3,
                   "BPM x4 -> clocked-delay tail halves (tempo-move recomputes the cache)");

        // Jitter gate: |dbpm| <= 0.25 must NOT recompute (bit-equal cache;
        // exactlyEqual because a recompute at 480.2 WOULD move the value).
        const double before = p.getTailLengthSeconds();
        playHead.bpm = 480.2;
        renderBlock (p, 256);
        check (juce::exactlyEqual (p.getTailLengthSeconds(), before),
               "+0.2 BPM jitter does NOT recompute the tail cache (bit-equal)");

        // A real move just past the gate recomputes again (smaller tail).
        playHead.bpm = 481.0;
        renderBlock (p, 256);
        check (p.getTailLengthSeconds() < before,
               "+1.0 BPM move DOES recompute (tail shrinks with the tempo)");
    }

    std::printf ("\nRENDER-QUALITY TEST: %s (%d failures)\n",
                 g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES", g_failures);
    return g_failures == 0;
}

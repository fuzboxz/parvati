// osc_preview_live_test — the OSC waveform preview's fluidity parity + live
// engine-derived modulation (2026-08-23 pass; docs/LIVE_MOD_FEEDBACK_DESIGN.md).
//
// The OscPreviewDisplay now glides EXACTLY like the FilterResponseDisplay:
// the displayed oscillator parameter is a critically-damped smoothed value
// (tau 130 ms, rebuilt every time its byte-quantized level moves) instead of
// the former 8-step quantized rebuild + 66 ms morph-restart chain, and a
// LiveOscValues provider (engine telemetry -> LiveFeedbackHub::liveOsc) makes
// the preview follow the EFFECTIVE (modulation-applied) osc parameter while
// a voice sounds — the same contract the filter preview has for cutoff.
//
// Sections:
//   [a] DETERMINISM PIN (design load-bearing): two FRESH ambika::dsp::
//       Oscillator renders of the same (shape, param) — including
//       FILTERED_NOISE with its per-instance LFSR — are bit-identical. The
//       smoothed per-byte rebuild in timerCallback relies on this: a moving
//       parameter reshapes the waveform, never flickers between renders of
//       the same byte. (A fresh instance is value-initialized; Reset() is
//       never called, so the global RNG is untouched.)
//   [b] BASE-PARAM GLIDE: a full-range parameter sweep bumps
//       previewGeneration() far more often than the OLD 8-step quantization
//       ever could (> 16 rebuilds vs the old <= 8 across 0..1), and once
//       settled the generation goes quiet (idle = no repaints).
//   [c] LIVE OVERLAY: a moving provider arms the overlay (temporal gate),
//       it hides after the hold window once the live byte settles, hides at
//       once on an inactive provider / unset provider.
//   [d] ENGINE END-TO-END: SynthEngine telemetry effOscParam[0] equals the
//       knob byte at rest (with the init patch's default LFO_4 -> PARAMETER_1
//       routing zeroed, exactly the filter_env=0 technique of
//       ui_telemetry_test [6]) and departs from it under an
//       ENV_1 -> PARAMETER_1 matrix routing.
//
// [b]/[c] use the editor_test [25](c) headless technique: the display is
// constructed standalone and NEVER parented, so the ctor-started 30 Hz poll
// timer keeps running (nothing starves the visibility hooks of a component
// that was never added anywhere), and the CFRunLoop is pumped directly.
// Every "active" phase drives CONTINUOUSLY MOVING values (pump-starvation
// proof: the temporal hold can never expire mid-pump) and every "settled"
// phase pumps well past the hold window.

#include <cmath>
#include <cstdio>
#include <functional>

#include "test_utils.h"          // setInt/setChoice/setParam host-style writes
#include "unified_test_runner.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

#include "PluginProcessor.h"

#include "dsp/constants.h"       // kAudioBlockSize
#include "dsp/oscillator.h"
#include "dsp/patch.h"           // WAVEFORM_*
#include "ui/ModTelemetryTypes.h"
#include "ui/OscPreviewDisplay.h"

namespace
{
void pumpMs (int ms)
{
#ifdef __APPLE__
    CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.001 * ms, false);
#else
    juce::Thread::sleep (ms);
#endif
}

// Pump until @p pred is true or @p timeoutMs elapsed (returns whether pred
// held). Bounds every wait so a regression fails an assertion instead of
// hanging the fork.
bool pumpUntil (const std::function<bool()>& pred, int timeoutMs)
{
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    while (juce::Time::getMillisecondCounterHiRes() - t0 < static_cast<double> (timeoutMs))
    {
        if (pred())
            return true;
        pumpMs (20);
    }
    return pred();
}

// The same 24-bit one-cycle increment OscPreviewDisplay::buildSampled uses
// (65536 / 40 -> integral 1638, fractional 102): renders one full cycle into
// the 40-sample block.
ambika::dsp::uint24_t oneCycleIncrement()
{
    ambika::dsp::uint24_t inc;
    inc.integral   = 1638;
    inc.fractional = 102;
    return inc;
}

// One FRESH-instance render of (shape, param) — the exact recipe of
// OscPreviewDisplay::buildSampled.
void renderOnce (uint8_t shape, uint8_t param, uint8_t (&out)[ambika::dsp::kAudioBlockSize])
{
    ambika::dsp::Oscillator osc;
    osc.set_parameter (param);
    uint8_t syncIn[ambika::dsp::kAudioBlockSize] = {};
    uint8_t syncOut[ambika::dsp::kAudioBlockSize] = {};
    osc.Render (shape, /*note=*/60, oneCycleIncrement(), syncIn, syncOut, out);
}

constexpr int    kBlock = 512;
constexpr double kRate  = 48000.0;

constexpr double kMsPerBlock = 1000.0 * kBlock / kRate;   // 10.67 ms
int blocksForMs (double ms) { return static_cast<int> (ms / kMsPerBlock) + 1; }

juce::MidiMessage noteOnMsg()  { return juce::MidiMessage::noteOn  (1, 60, 0.9f); }
}  // namespace

TEST(osc_preview_live_test)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    using ambika::dsp::kAudioBlockSize;

    // =========================================================================
    std::printf ("[a] Fresh-instance render determinism (same shape+param -> identical buffer)\n");
    {
        // Shapes covering every render family: band-limited PWM, a CZ pair,
        // FM, dirty PWM, quad saw, FILTERED NOISE (the per-instance LFSR —
        // the strongest determinism risk), a wavetable and wavequence.
        const uint8_t shapes[] = {
            ambika::dsp::WAVEFORM_SQUARE,          // analytic family anchor
            ambika::dsp::WAVEFORM_CZ_SAW,
            ambika::dsp::WAVEFORM_CZ_PLS_BP,
            ambika::dsp::WAVEFORM_FM,
            ambika::dsp::WAVEFORM_DIRTY_PWM,
            ambika::dsp::WAVEFORM_QUAD_SAW_PAD,
            ambika::dsp::WAVEFORM_FILTERED_NOISE,
            ambika::dsp::WAVEFORM_WAVETABLE_1,
            ambika::dsp::WAVEFORM_WAVEQUENCE,
        };
        for (const uint8_t shape : shapes)
        {
            for (const uint8_t param : { uint8_t { 0 }, uint8_t { 40 }, uint8_t { 127 } })
            {
                uint8_t a[kAudioBlockSize], b[kAudioBlockSize];
                renderOnce (shape, param, a);
                renderOnce (shape, param, b);
                bool same = true;
                for (int i = 0; i < kAudioBlockSize; ++i)
                    if (a[i] != b[i]) { same = false; break; }
                char m[128];
                std::snprintf (m, sizeof (m),
                               "[a] shape %u param %u renders bit-identically twice", (unsigned) shape, (unsigned) param);
                CHECK(same, m);
                if (! same)
                    break;
            }
        }
    }

    // =========================================================================
    std::printf ("[b] Base-parameter glide (sweep rebuilds > old 8-step quantization; idle quiet)\n");
    {
        // A DSP-SAMPLED shape (the path the old quantize+morph chain was
        // built around) driven through a mutable getter — no APVTS needed
        // for the display-side behaviour.
        float shape01 = static_cast<float> (ambika::dsp::WAVEFORM_CZ_SAW)
                        / static_cast<float> (ambika::dsp::WAVEFORM_LAST - 1);
        float param01 = 0.0f;
        OscPreviewDisplay disp ("Glide",
                                [&shape01] { return shape01; },
                                [&param01] { return param01; });
        disp.setBounds (0, 0, 220, 64);
        pumpMs (150);   // settle the ctor state (seeded converged, no anim)

        const int gen0 = disp.previewGeneration();
        // rebuildCycle increments generation_; the ctor calls it exactly once
        // (and the seeded converged state cannot rebuild on the first ticks),
        // so >= 1 actually pins "the ctor built the initial cycle".
        CHECK(gen0 >= 1, "[b] ctor built the initial cycle (generation seeded)");

        // Full-range sweep in 16 steps: each step advances the target by
        // 1/16 ~= 8 param bytes, so the SMOOTHED value crosses ~127 byte
        // boundaries across the sweep. The OLD 8-step code rebuilt at most 8
        // times over 0..1 (paramByte>>4 buckets); > 16 excludes it with a
        // starvation-proof margin even if only a fraction of the pump
        // windows deliver timer ticks.
        for (int s = 1; s <= 16; ++s)
        {
            param01 = static_cast<float> (s) / 16.0f;
            pumpMs (250);
        }
        const int gen1 = disp.previewGeneration();
        {
            char m[128];
            std::snprintf (m, sizeof (m),
                           "[b] sweep rebuilt %d times (want > 16; old 8-step code max 8)", gen1 - gen0);
            CHECK(gen1 - gen0 > 16, m);
        }

        // Settled: after tau-settle (~2x130 ms) + margin, the generation
        // must go QUIET (converged smoothing cannot move the byte; idle =
        // no repaints, no rebuilds).
        pumpMs (600);
        const int gen2 = disp.previewGeneration();
        pumpMs (400);
        const int gen3 = disp.previewGeneration();
        CHECK(gen3 == gen2, "[b] idle generation quiet after convergence (no jitter rebuilds)");
    }

    // =========================================================================
    std::printf ("[c] Live overlay temporal gate (moving arms; settled hides; inactive/unset hides)\n");
    {
        // Moving provider: bytes move EVERY tick (sin sweep around 0.6), so
        // the temporal hold can never expire mid-pump (pump-starvation
        // proof, the editor_test [25](c) lesson).
        struct LiveProv
        {
            bool  active = true;
            bool  moving = false;
            float frozen = 0.8f;
            int   call   = 0;
            parvati::LiveOscValues operator()()
            {
                if (! moving)
                    return { active, frozen, };
                const float p = 0.6f + 0.15f * std::sin (0.4f * static_cast<float> (++call));
                return { active, p };
            }
        };
        LiveProv lv;
        float shape01 = static_cast<float> (ambika::dsp::WAVEFORM_CZ_SAW)
                        / static_cast<float> (ambika::dsp::WAVEFORM_LAST - 1);
        OscPreviewDisplay disp ("Live",
                                [&shape01] { return shape01; },
                                [] { return 0.2f; });
        disp.setBounds (0, 0, 220, 64);
        disp.setLiveValuesProvider ([&lv] { return lv(); });

        // (1) Moving: the overlay arms and STAYS armed while the bytes move
        //     every tick (the gate re-arms each tick; pump length irrelevant).
        lv.moving = true;
        CHECK(pumpUntil ([&disp] { return disp.liveOverlayActiveForTest(); }, 1500),
              "[c] overlay armed under moving live modulation");

        // (2) Settled (temporal gate): freeze at a fixed byte -> the last
        //     movement re-arms once, then the ~270 ms hold expires and the
        //     overlay hides (pump well past the budget).
        lv.moving = false;
        CHECK(pumpUntil ([&disp] { return ! disp.liveOverlayActiveForTest(); }, 1500),
              "[c] overlay hides once the live byte settles (hold expired)");

        // (3) Re-arm: moving again brings it back (the hide above did not
        //     latch anything off).
        lv.moving = true;
        CHECK(pumpUntil ([&disp] { return disp.liveOverlayActiveForTest(); }, 1500),
              "[c] overlay re-arms when modulation moves again");

        // (4) Voice gone (provider inactive): hide at once.
        lv.active = false;
        CHECK(pumpUntil ([&disp] { return ! disp.liveOverlayActiveForTest(); }, 1500),
              "[c] overlay hides on an inactive provider (voice gone)");

        // (5) Unset provider: hides too (the immediate-unset path).
        lv.active = true;
        lv.moving = true;
        CHECK(pumpUntil ([&disp] { return disp.liveOverlayActiveForTest(); }, 1500),
              "[c] overlay armed again before the unset check");
        disp.setLiveValuesProvider (std::function<parvati::LiveOscValues()> {});
        CHECK(! disp.liveOverlayActiveForTest(),
              "[c] unset provider hides the overlay immediately");
    }

    // =========================================================================
    std::printf ("[d] Engine end-to-end: effOscParam follows the knob at rest, departs under ENV_1->P1\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (kRate, kBlock);
        auto& eng = proc.getEngine();
        // Belt-and-braces: zero every plugin matrix row that targets the osc
        // parameters — mod1 (ENV_1->P1), mod5 (LFO_2->P1), mod2 (ENV_1->P2),
        // mod6 (LFO_2->P2). The plugin's startup patch is the CONTROLLER init
        // patch (ParameterLayout.cpp InitPatch::bytes) whose every mod row
        // amounts to 0 (its mod8 row is LFO_4->FILTER_CUTOFF, unrelated to the
        // osc params), so this is a no-op today and only keeps the at-rest pin
        // immune to a future default change (same technique as
        // ui_telemetry_test [6]'s filter_env = 0). The DSP voicecard-fallback
        // kInitPatch does carry LFO_4->P1 at 63 (dsp/voice.cpp modulation[7])
        // but that patch never flows through the APVTS. Both knob bytes are
        // set explicitly so the pins do not depend on the factory defaults
        // (osc2_param defaults to 32).
        setInt (proc, "mod1_amount", 0);
        setInt (proc, "mod5_amount", 0);
        setInt (proc, "mod2_amount", 0);
        setInt (proc, "mod6_amount", 0);
        setInt (proc, "osc1_param", 100);
        setInt (proc, "osc2_param", 7);
        proc.syncAllParamsToEngine();
        eng.setUiTelemetryPart (0);
        renderBlocks (proc, 2, nullptr, kBlock);                          // part service lands

        const auto on = noteOnMsg();
        renderBlocks (proc, 3, &on, kBlock);
        renderBlocks (proc, blocksForMs (1000.0), nullptr, kBlock);      // envelopes settled

        parvati::ModTelemetrySnapshot snap;
        CHECK(proc.getEngine().readUiTelemetry (snap), "[d] frame valid while held");
        CHECK(snap.voiceActive, "[d] voiceActive while held");
        CHECK(snap.effOscParam[0] == 100, "[d] effOscParam[0] == knob byte (100) with no modulation");
        CHECK(snap.effOscParam[1] == 7,   "[d] effOscParam[1] == knob byte (7) with no modulation");

        bool constant = true;
        for (int i = 0; i < 20; ++i)
        {
            renderBlocks (proc, 2, nullptr, kBlock);
            parvati::ModTelemetrySnapshot s2;
            if (! proc.getEngine().readUiTelemetry (s2) || s2.effOscParam[0] != 100)
                constant = false;
        }
        CHECK(constant, "[d] effOscParam[0] constant with no modulation");

        // Strong ENV_1 -> PARAMETER_1 matrix routing: the factory patch's
        // modulation[0] (APVTS row mod1) already routes {MOD_SRC_ENV_1,
        // MOD_DST_PARAMETER_1} at amount 0 — only the amount needs raising.
        // ENV_1 is DC-coupled, so with amount 63 and env1 sustained near its
        // ceiling the 14-bit accumulator (param*128 + 63*env1value) drives the
        // effective byte far above the knob byte while held.
        ParvatiAudioProcessor proc2;
        proc2.prepareToPlay (kRate, kBlock);
        auto& eng2 = proc2.getEngine();
        // Zero the OTHER osc-param rows (LFO_2->P1/P2, ENV_1->P2 — all default
        // 0) so the departure below is attributable to the ENV_1 -> P1
        // routing alone (LFO_2 is free-running at the factory rate and would
        // wobble the byte if its amount were ever raised).
        setInt (proc2, "mod5_amount", 0);
        setInt (proc2, "mod6_amount", 0);
        setInt (proc2, "mod2_amount", 0);
        setInt (proc2, "mod1_amount", 63);               // ENV_1 -> PARAMETER_1 (factory routing, amount raised)
        setInt (proc2, "env1_sustain", 100);
        setInt (proc2, "osc1_param", 20);
        setInt (proc2, "osc2_param", 9);
        proc2.syncAllParamsToEngine();
        eng2.setUiTelemetryPart (0);
        renderBlocks (proc2, 2, nullptr, kBlock);
        renderBlocks (proc2, 3, &on, kBlock);
        renderBlocks (proc2, blocksForMs (1000.0), nullptr, kBlock);      // env 1 rests at sustain

        parvati::ModTelemetrySnapshot snap2;
        CHECK(eng2.readUiTelemetry (snap2), "[d] frame valid (modulated) while held");
        {
            char m[128];
            std::snprintf (m, sizeof (m),
                           "[d] effOscParam[0] departs from the knob under ENV_1 -> P1 (=%u, knob 20)",
                           (unsigned) snap2.effOscParam[0]);
            CHECK(snap2.effOscParam[0] > 60, m);
        }
        CHECK(snap2.effOscParam[1] == 9,
              "[d] effOscParam[1] untouched by the osc-1-only routing");
    }

    return true;
}

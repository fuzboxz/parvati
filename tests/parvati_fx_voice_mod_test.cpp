// FX mod-matrix: per-voice source handling.
//
// Proves two fixes to how synth modulation sources reach the FX section:
//
//   (1) AC/DC coupling now mirrors the SYNTH voice mod matrix. LFO_1..4 /
//       PITCH_BEND / NOTE are AC-coupled (128 = neutral, bipolar ±1); all other
//       sources stay DC-coupled (0 = neutral, unipolar 0..1). Before this, an
//       LFO / bend / note at rest (128) injected a static +0.126 offset (at
//       amount 63) into the FX param instead of zero modulation. Verified by
//       routing PITCH_BEND → fx1_param1: at rest the effective param equals the
//       base (not base+0.5), a full up-bend drives it to 1, a full down-bend to 0.
//
//   (2) The FX representative voice now tracks the MOST-RECENTLY-TRIGGERED
//       active voice per part (not an arbitrary first-active voice), with a
//       short (~5 ms) crossfade on any voice identity change so per-voice
//       sources (VELOCITY / NOTE / per-note MPE) glide instead of jumping.
//       Verified by routing VELOCITY → fx1_param1: striking a second note (new
//       velocity) on a different voice switches the tracked voice and the
//       effective param settles at the new velocity; the crossfade phase is
//       observed to arm (< 1) on the switch and settle (== 1) afterwards.
//
// Run: ./build_unified/parvati_unified_tests parvati_fx_voice_mod_test

#include <cmath>
#include "unified_test_runner.h"
#include "test_utils.h"   // renderBlocks
#include <cstdio>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "SynthEngine.h"
#include "dsp/fx/FxTypes.h"
#include "dsp/patch.h"   // MOD_SRC_* enum values

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

// ModulationSource enum indices used below (patch.h).
constexpr int kSrcVelocity   = ambika::dsp::MOD_SRC_VELOCITY;     // 14 (DC)
constexpr int kSrcPitchBend  = ambika::dsp::MOD_SRC_PITCH_BEND;   // 16 (AC)
constexpr int kDstFx1Param1  = FX_DST_FX1_P1;                     // 1

// Configure fx1 (FrequencyShifter: a cheap always-finite effect) enabled at full
// wet, with fx1_param1 routed from @p source at full amount. The EFFECTIVE param
// is read back via the engine's debug tracker regardless of the DSP output.
void routeParam1 (ParvatiAudioProcessor& proc, int source, uint8_t baseParam1)
{
    auto& eng = proc.getEngine();
    eng.setFxSlotType    (0, static_cast<uint8_t> (FxType::FrequencyShifter));
    eng.setFxSlotEnabled (0, 1);
    eng.setFxSlotDryWet  (0, 127);
    eng.setFxSlotParam   (0, 0, baseParam1);          // param1 base (0..127)
    eng.setFxModSlot     (0, static_cast<uint8_t> (source),
                             static_cast<uint8_t> (kDstFx1Param1), 63);
}

// Average effective fx1_param1 over a short render (debug min/max track the
// effective value; for a held source they converge, so the mean is the level).
float meanEffParam (ParvatiAudioProcessor& proc, int block, int blocks)
{
    proc.getEngine().debugResetEffParamTracking (0);
    renderBlocks (proc, blocks, nullptr, block);
    proc.getEngine().debugStopEffParamTracking();
    const float mn = proc.getEngine().debugEffParamMin (0);
    const float mx = proc.getEngine().debugEffParamMax (0);
    return 0.5f * (mn + mx);
}
}  // namespace

// ---------------------------------------------------------------------------
// (1a) AC centering: PITCH_BEND at rest (128) => effective param == base.
//      OLD DC behaviour gave base + 128/255 (~+0.50), so this discriminates.
// ---------------------------------------------------------------------------
static void testAcCentering()
{
    std::printf ("(1a) AC centering (PITCH_BEND at rest = neutral)\n");

    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::MessageManager::getInstance();

    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);
    routeParam1 (proc, kSrcPitchBend, 32);   // base param1 = 32/127 ~ 0.252

    const auto noteOn = juce::MidiMessage::noteOn (1, 60, static_cast<uint8_t> (100));
    renderBlocks (proc, 8, &noteOn, 256, 0);        // sustain, NO pitch-bend movement

    const float eff = meanEffParam (proc, 256, 6);
    const float base = 32.0f / 127.0f;
    char msg[160];
    std::snprintf (msg, sizeof (msg), "rest effParam=%.4f (AC: ~base=%.4f; OLD DC would be ~%.4f)",
                   eff, base, base + 128.0f / 255.0f);
    check (std::fabs (eff - base) < 0.03f, msg);
}

// ---------------------------------------------------------------------------
// (1b) AC bipolar: a full up-bend drives the param to 1, a full down-bend to 0.
//      OLD DC behaviour only ever ADDED (min ~ base+1/255), so the down check
//      (eff near 0) discriminates.
// ---------------------------------------------------------------------------
static void testAcBipolar()
{
    std::printf ("(1b) AC bipolar (up-bend -> 1, down-bend -> 0)\n");

    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::MessageManager::getInstance();

    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);
    routeParam1 (proc, kSrcPitchBend, 32);   // base ~ 0.252

    const auto noteOn = juce::MidiMessage::noteOn (1, 60, static_cast<uint8_t> (100));
    renderBlocks (proc, 8, &noteOn, 256, 0);        // sustain

    // Full UP bend (wheel 16383) => src ~ 255 => norm ~ +0.99 => eff clamps to 1.
    const auto bendUp = juce::MidiMessage::pitchWheel (1, 16383);
    proc.getEngine().debugResetEffParamTracking (0);
    renderBlocks (proc, 6, &bendUp, 256, 0);
    proc.getEngine().debugStopEffParamTracking();
    const float upMax = proc.getEngine().debugEffParamMax (0);

    // Full DOWN bend (wheel 0) => src ~ 1 => norm ~ -0.99 => eff clamps to 0.
    const auto bendDown = juce::MidiMessage::pitchWheel (1, 0);
    proc.getEngine().debugResetEffParamTracking (0);
    renderBlocks (proc, 6, &bendDown, 256, 0);
    proc.getEngine().debugStopEffParamTracking();
    const float downMin = proc.getEngine().debugEffParamMin (0);

    char msg[160];
    std::snprintf (msg, sizeof (msg), "up-bend effParam max=%.4f (expect > 0.92)", upMax);
    check (upMax > 0.92f, msg);
    std::snprintf (msg, sizeof (msg), "down-bend effParam min=%.4f (expect < 0.08; OLD DC ~ 0.26)", downMin);
    check (downMin < 0.08f, msg);
}

// ---------------------------------------------------------------------------
// (2a) Representative voice tracks the MOST-RECENTLY-TRIGGERED active voice.
//      Route VELOCITY -> fx1_param1: a second note (new velocity) on a different
//      voice switches the tracked voice and the effective param settles at the
//      new velocity.
// ---------------------------------------------------------------------------
static void testMostRecentVoiceTracking()
{
    std::printf ("(2a) representative voice tracks the newest note (VELOCITY)\n");

    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::MessageManager::getInstance();

    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);
    routeParam1 (proc, kSrcVelocity, 0);      // base 0 => effParam = velocity/255

    // Note 1 (velocity 100). Default Part 0 is POLY (6 voices): note 60 -> voice 0.
    const auto n1 = juce::MidiMessage::noteOn (1, 60, static_cast<uint8_t> (100));
    renderBlocks (proc, 10, &n1, 256, 0);
    const int v1 = proc.getEngine().debugFxTrackedVoice (0);
    const float eff1 = meanEffParam (proc, 256, 4);

    // Note 2 (velocity 30) on a DIFFERENT key, note 60 still held => a different
    // voice (more recently triggered) becomes the tracked voice.
    const auto n2 = juce::MidiMessage::noteOn (1, 64, static_cast<uint8_t> (30));
    renderBlocks (proc, 10, &n2, 256, 0);           // crossfade (~5 ms) completes within one block
    const int v2 = proc.getEngine().debugFxTrackedVoice (0);
    const float eff2 = meanEffParam (proc, 256, 4);

    char msg[200];
    std::snprintf (msg, sizeof (msg), "tracked voice switched on the newer note (v1=%d, v2=%d)", v1, v2);
    check (v2 != v1 && v2 >= 0, msg);

    std::snprintf (msg, sizeof (msg), "note1 effParam=%.4f (~midi-vel 100 -> %.4f)", eff1, 100.0f / 127.0f);
    check (std::fabs (eff1 - 100.0f / 127.0f) < 0.04f, msg);

    std::snprintf (msg, sizeof (msg), "note2 effParam=%.4f (~midi-vel 30 -> %.4f) — FX followed the newest note",
                   eff2, 30.0f / 127.0f);
    check (std::fabs (eff2 - 30.0f / 127.0f) < 0.04f, msg);
}

// ---------------------------------------------------------------------------
// (2b) Crossfade arms on a voice change (phase < 1) and settles (phase == 1).
//      Uses 32-sample blocks so the ~5 ms crossfade spans several blocks and the
//      in-progress phase is observable (at 256 samples/block it would finish
//      within a single block).
// ---------------------------------------------------------------------------
static void testCrossfadeArmed()
{
    std::printf ("(2b) crossfade arms on voice change, then settles\n");

    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::MessageManager::getInstance();

    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 32);         // small block: crossfade spans ~8 blocks
    routeParam1 (proc, kSrcVelocity, 0);

    const auto n1 = juce::MidiMessage::noteOn (1, 60, static_cast<uint8_t> (100));
    renderBlocks (proc, 40, &n1, 32, 0);            // settle note 1
    const float phaseAfter1 = proc.getEngine().debugFxFadePhase (0);

    // Trigger note 2 (different voice) then read the phase after ONE 32-sample
    // block (~0.67 ms): the crossfade must be ARMED (0 < phase < 1).
    const auto n2 = juce::MidiMessage::noteOn (1, 64, static_cast<uint8_t> (30));
    renderBlocks (proc, 1, &n2, 32, 0);
    const float phaseMid = proc.getEngine().debugFxFadePhase (0);

    // Render the rest of the crossfade (~5 ms ~ 8 blocks): it must SETTLE (== 1).
    renderBlocks (proc, 12, nullptr, 32);
    const float phaseSettled = proc.getEngine().debugFxFadePhase (0);

    char msg[160];
    std::snprintf (msg, sizeof (msg), "after note1: crossfade settled (phase=%.3f == 1)", phaseAfter1);
    check (std::fabs (phaseAfter1 - 1.0f) < 1.0e-4f, msg);

    std::snprintf (msg, sizeof (msg), "1 block after the switch: crossfade ARMED (phase=%.3f in (0,1))", phaseMid);
    check (phaseMid > 0.0f && phaseMid < 1.0f, msg);

    std::snprintf (msg, sizeof (msg), "after ~5 ms: crossfade settled (phase=%.3f == 1)", phaseSettled);
    check (std::fabs (phaseSettled - 1.0f) < 1.0e-4f, msg);
}

TEST(parvati_fx_voice_mod_test)
{
    std::printf ("=== FX mod-matrix voice-source handling ===\n\n");
    testAcCentering();
    testAcBipolar();
    testMostRecentVoiceTracking();
    testCrossfadeArmed();

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures == 0 ? "ALL PASS" : "FAILURES",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}

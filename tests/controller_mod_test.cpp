// Controller-modulation regression test. Verifies that the global MIDI
// controllers (mod wheel CC1, breath CC2, foot pedal CC4) and the per-voice
// channel-pressure (MPE expression) are wired to their mod-matrix sources AND
// are AUDIBLE when routed.
//
// The mod wheel / breath / foot were once dead (SynthEngine::handleController
// deferred them to a no-op base handler); they are now written to all voices
// (MOD_SRC_WHEEL / WHEEL_2 / EXPRESSION, value<<1) via applyGlobalModSource,
// faithful to the firmware. Channel pressure routes per-voice to
// MOD_SRC_AFTERTOUCH. There was no regression test for any of this (the only
// coverage was a throwaway smoke binary). This is the durable replacement.
//
// Each controller is routed to MOD_DST_FILTER_CUTOFF (amount 63) so a controller
// move changes the cutoff -> a measurable RMS change on a sustaining note.
// Run: ./build_unified/hellcat_unified_tests controller_mod_test

#include <cmath>
#include "unified_test_runner.h"
#include <cstdio>
#include <functional>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "PluginProcessor.h"
#include "test_utils.h"              // shared setParam (host-path helper)

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

// RMS over a stereo buffer.
double bufferRms (const juce::AudioBuffer<float>& b)
{
    double sum = 0.0;
    long n = 0;
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const double s = static_cast<double> (b.getSample (ch, i));
            sum += s * s;
            ++n;
        }
    return n ? std::sqrt (sum / static_cast<double> (n)) : 0.0;
}

// Set up one modulation routing (source -> Filter Cutoff, amount 63) on slot
// `slot`, so the given source is audible.
void routeToCutoff (HellcatAudioProcessor& proc, int slot, int sourceEnum)
{
    char id[32];
    std::snprintf (id, sizeof (id), "mod%d_source", slot);    setParam (proc, id, sourceEnum);
    std::snprintf (id, sizeof (id), "mod%d_dest", slot);      setParam (proc, id, 12);  // MOD_DST_FILTER_CUTOFF
    std::snprintf (id, sizeof (id), "mod%d_amount", slot);    setParam (proc, id, 63);
}

// Render `blocks`, injecting @p inject (if any) in block 0; return RMS of the
// last `meas` blocks (the steady-state response after the controller move).
double measurePhase (HellcatAudioProcessor& proc, const juce::MidiMessage* inject)
{
    constexpr int kBlock = 512;
    constexpr int kBlocks = 50;
    constexpr int kMeas = 20;
    juce::AudioBuffer<float> buf (2, kBlock);
    double acc = 0.0;
    int cnt = 0;
    for (int b = 0; b < kBlocks; ++b)
    {
        juce::MidiBuffer midi;
        if (b == 0 && inject != nullptr)
            midi.addEvent (*inject, 0);
        buf.clear();
        proc.processBlock (buf, midi);
        if (b >= kBlocks - kMeas)
        {
            acc += bufferRms (buf);
            ++cnt;
        }
    }
    return cnt ? acc / static_cast<double> (cnt) : 0.0;
}

// Play a sustaining note (no release) + a short warm-up so the attack/decay
// settles into the sustain segment before measurement.
void playSustainingNote (HellcatAudioProcessor& proc)
{
    constexpr int kBlock = 512;
    {
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
        juce::AudioBuffer<float> buf (2, kBlock);
        buf.clear();
        proc.processBlock (buf, midi);
    }
    for (int b = 0; b < 30; ++b)
    {
        juce::AudioBuffer<float> buf (2, kBlock);
        buf.clear();
        juce::MidiBuffer m;
        proc.processBlock (buf, m);
    }
}

// One controller audibility check: route `sourceEnum`->Cutoff on `slot`, sustain
// a note, then compare RMS with the controller at 0 vs 127. The diff must exceed
// @p threshold (the controller is audible). All other state is constant across
// the two phases, so the diff isolates this controller's effect.
bool controllerAudible (int slot, int sourceEnum,
                        const std::function<juce::MidiMessage (int val)>& makeMsg,
                        const char* label, double threshold)
{
    HellcatAudioProcessor proc;
    proc.prepareToPlay (48000.0, 512);
    // Pin the card: the loudness sweep of a cutoff move is card-dependent
    // (this test targets the CONTROLLER wiring, not the filter).
    setParam (proc, "filter_card", 3);   // Ladder
    routeToCutoff (proc, slot, sourceEnum);
    proc.syncAllParamsToEngine();
    playSustainingNote (proc);

    auto msg0   = makeMsg (0);
    auto msg127 = makeMsg (127);
    const double r0   = measurePhase (proc, &msg0);
    const double r127 = measurePhase (proc, &msg127);
    const double diff = std::fabs (r127 - r0);

    char m[160];
    std::snprintf (m, sizeof (m),
                   "%s audible (rms 0=%.5f 127=%.5f diff=%.5f, need >%.4f)",
                   label, r0, r127, diff, threshold);
    const bool ok = diff > threshold;
    check (ok, m);
    return ok;
}
}  // namespace

TEST(controller_mod_test)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf ("[1] Global MIDI controllers -> mod-matrix sources (audibility)\n");
    // CC1 mod wheel  -> MOD_SRC_WHEEL      (handleController -> applyGlobalModSource)
    controllerAudible (0, 17 /*MOD_SRC_WHEEL*/,
                       [] (int v) { return juce::MidiMessage::controllerEvent (1, 1, v); },
                       "mod wheel (CC1) -> Cutoff", 0.005);
    // CC2 breath     -> MOD_SRC_WHEEL_2
    controllerAudible (1, 18 /*MOD_SRC_WHEEL_2*/,
                       [] (int v) { return juce::MidiMessage::controllerEvent (1, 2, v); },
                       "breath (CC2) -> Cutoff", 0.005);
    // CC4 foot pedal -> MOD_SRC_EXPRESSION
    controllerAudible (2, 19 /*MOD_SRC_EXPRESSION*/,
                       [] (int v) { return juce::MidiMessage::controllerEvent (1, 4, v); },
                       "foot pedal (CC4) -> Cutoff", 0.005);

    std::printf ("\n[2] Per-voice channel pressure (MPE expression)\n");
    // Channel pressure -> MOD_SRC_AFTERTOUCH (per-voice, only the active voice).
    controllerAudible (3, 15 /*MOD_SRC_AFTERTOUCH*/,
                       [] (int v) { return juce::MidiMessage::channelPressureChange (1, v); },
                       "channel pressure -> Cutoff (MPE)", 0.005);

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "CONTROLLER MOD TEST: FAILURES" : "CONTROLLER MOD TEST: ALL PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}

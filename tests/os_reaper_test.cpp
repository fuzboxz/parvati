// Retired-Oversampling reaper unit test (audit F3 lifecycle).
//
// Pins, DETERMINISTICALLY (no timers — the reaper is invoked explicitly), the
// parking/reaping lifecycle of AmbikaVoice's staged filter-oversampling swaps:
//   - each consumed swap parks exactly ONE displaced Oversampling object
//     (the parking is bounded by kRetiredOsCap = 2 per voice)
//   - a 3rd park inside one reaper interval hits the documented FALLBACK
//     (inline delete on the audio thread — bounded memory, no leak): the
//     parked count stays at the cap, it never grows
//   - SynthEngine::reapRetiredAudioObjects() clears every parked slot
//   - repeated flip+reap cycles never grow the parked count
//
// Uses the processor-level public seams (prepareToPlay / processBlock /
// setOversamplingFactor) so the staged swap is exercised through the REAL
// audio-thread install path (AmbikaVoice::fillInternalBlock ->
// consumeStagedOversampling) of a SUSTAINED note — a held voice is the only
// deterministic consumer (idle voices install on their next note).
//
// Run: ./build_unified/parvati_unified_tests os_reaper_test

#include <cstdio>
#include "unified_test_runner.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>  // ScopedJuceInitialiser_GUI

#include "AmbikaVoice.h"
#include "PluginProcessor.h"
#include "test_utils.h"              // shared setParam (host-path helper)
#include "SynthEngine.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

void renderBlock (ParvatiAudioProcessor& p, int numSamples)
{
    juce::AudioBuffer<float> buf (2, numSamples);
    buf.clear();
    juce::MidiBuffer midi;
    p.processBlock (buf, midi);
}

// Sum of debugRetiredOsCount() across every engine voice (the whole-pool
// view the reaper walks). Only voices that RENDERED a swap ever park, so a
// single held note keeps this deterministic: exactly one voice contributes.
int totalRetired (ParvatiAudioProcessor& p)
{
    int n = 0;
    auto& engine = p.getEngine();
    for (int i = 0; i < engine.getNumVoices(); ++i)
        if (auto* v = dynamic_cast<AmbikaVoice*> (engine.getVoice (i)))
            n += v->debugRetiredOsCount();
    return n;
}
}  // namespace

TEST(os_reaper_test)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf ("=== Parvati retired-OS reaper (park capacity / fallback / reap) ===\n");

    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);

    // Hold a note for the WHOLE test: sustain 127 keeps the amp envelope (and
    // the voice) active, so every staged OS swap is consumed on the next block.
    setParam (proc, "env2_sustain", 127);
    {
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
        proc.processBlock (buf, midi);
    }

    // ---- Baseline: the ctor-staged 2x object installs via prepare(); the
    // displaced object was nullptr, so NOTHING is parked yet.
    std::printf ("\n[1] baseline: sustained note, nothing parked\n");
    renderBlock (proc, 256);
    renderBlock (proc, 256);
    check (totalRetired (proc) == 0, "no retired objects after the initial install");

    // ---- Two swaps inside one reaper interval: one parked each ----
    std::printf ("\n[2] consumed swaps park the displaced object\n");
    proc.setOversamplingFactor (4);    // swap 1 (2x displaced)
    renderBlock (proc, 256);
    check (totalRetired (proc) == 1, "first swap parks exactly 1 object");

    proc.setOversamplingFactor (8);    // swap 2 (4x displaced)
    renderBlock (proc, 256);
    check (totalRetired (proc) == 2, "second swap parks a 2nd object (cap reached)");

    // ---- Third swap without reaping: the fallback (inline delete) fires and
    // the parked count stays at the cap — bounded, never a leak.
    std::printf ("\n[3] 3rd park inside one interval -> fallback, no growth\n");
    proc.setOversamplingFactor (2);    // swap 3 (8x displaced, parking full)
    renderBlock (proc, 256);
    check (totalRetired (proc) == 2, "3rd park falls back (inline delete); count stays at cap 2");

    // ---- Explicit reap clears everything ----
    std::printf ("\n[4] reapRetiredAudioObjects() clears all slots\n");
    proc.getEngine().reapRetiredAudioObjects();
    check (totalRetired (proc) == 0, "explicit reap clears every parked slot");

    // ---- Repeated flip+reap cycles never grow ----
    std::printf ("\n[5] repeated flip+reap cycles stay bounded\n");
    bool bounded = true;
    for (int i = 0; i < 5; ++i)
    {
        proc.setOversamplingFactor (i % 2 == 0 ? 4 : 8);
        renderBlock (proc, 256);
        const int parked = totalRetired (proc);
        if (parked != 1)   // exactly the one displaced object of this cycle
            bounded = false;
        proc.getEngine().reapRetiredAudioObjects();
        if (totalRetired (proc) != 0)
            bounded = false;
    }
    check (bounded, "each cycle parks exactly 1 and reaps back to 0 (no growth)");

    std::printf ("\n=== %s (%d failure%s) ===\n",
                 g_failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}

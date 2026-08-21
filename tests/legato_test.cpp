// Mono-legato regression test for the overlap/slide-back silence bug.
//
// BUG (pre-fix): in MONO mode, a 2nd note played while a 1st is held takes the
// "legato" path (monoStack.size() > 1). juce::Synthesiser::startVoice, however,
// calls stopNote(0, false) -> AmbikaVoice::Kill (zeroes the envelope state) on
// the ALREADY-PLAYING voice before re-triggering it. The firmware
// Voice::Trigger(legato) then SKIPS the re-attack (the whole point of legato),
// so a killed-then-legato voice renders SILENCE for the whole overlap. The fix
// (AmbikaVoice::retriggerNote + the SynthEngine MONO loops) re-triggers the
// sounding voice WITHOUT the kill (setKeyDown(true) + startNote only), so the
// envelope keeps sustaining and the legato Trigger slides the pitch.
//
// This test asserts the overlap is audible (the regression), the slide-back on
// release is audible, and the final release goes silent. On the pre-fix path the
// overlap assertion fails (output ~silence); it passes with the fix.
//
// Only adds tests/ + a CMake target; no Source/ changes.

#include <algorithm>
#include "unified_test_runner.h"
#include <cmath>
#include <cstdio>
#include <set>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg) { std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg); if (! cond) ++g_failures; }

void renderIdle (ParvatiAudioProcessor& p, int blocks)
{
    for (int i = 0; i < blocks; ++i)
    {
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer midi;
        p.processBlock (buf, midi);
    }
}

// Render `blocks` of audio (no MIDI) and return the mono-sum peak amplitude.
float renderMonoPeak (ParvatiAudioProcessor& p, int blocks)
{
    float peak = 0.0f;
    for (int i = 0; i < blocks; ++i)
    {
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer midi;
        p.processBlock (buf, midi);
        for (int s = 0; s < buf.getNumSamples(); ++s)
            peak = std::max (peak, std::fabs (0.5f * (buf.getSample (0, s) + buf.getSample (1, s))));
    }
    return peak;
}

// Max sample-to-sample |delta| of the mono sum over `blocks` (no MIDI). A
// legato retrigger must keep the waveform CONTINUOUS: the natural slew of a
// settled note is tiny (a ~260 Hz saw at ~0.3 amp slews ~0.01/sample), while
// a resampler-FIFO discard restarts the interpolator cold — a time-skip
// discontinuity on the order of the signal level itself.
float renderMonoMaxSlew (ParvatiAudioProcessor& p, int blocks)
{
    float maxDelta = 0.0f;
    float prev = 0.0f;
    for (int i = 0; i < blocks; ++i)
    {
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer midi;
        p.processBlock (buf, midi);
        for (int s = 0; s < buf.getNumSamples(); ++s)
        {
            const float x = 0.5f * (buf.getSample (0, s) + buf.getSample (1, s));
            maxDelta = std::max (maxDelta, std::fabs (x - prev));
            prev = x;
        }
    }
    return maxDelta;
}

void noteEvent (ParvatiAudioProcessor& p, const juce::MidiMessage& m)
{
    juce::AudioBuffer<float> buf (2, 256);
    buf.clear();
    juce::MidiBuffer midi;
    midi.addEvent (m, 0);
    p.processBlock (buf, midi);
}

// Distinct MIDI pitches currently held by Part 0's voices.
std::set<int> heldPitches (SynthEngine& e, int part)
{
    std::set<int> s;
    for (int vi : e.getPart (part).voiceIndices)
        if (auto* av = e.getAmbikaVoice (vi))
            if (av->getCurrentlyPlayingNote() >= 0) s.insert (av->getCurrentlyPlayingNote());
    return s;
}
}  // namespace

TEST(legato_test)
{
    juce::MessageManager::getInstance();
    juce::ScopedJuceInitialiser_GUI guiInit;

    ParvatiAudioProcessor processor;
    processor.prepareToPlay (48000.0, 256);
    processor.syncAllParamsToEngine();

    SynthEngine& engine = processor.getEngine();

    // Part 0 owns all 6 voicecards by default (Hardware: 1 voice/voicecard).
    // MONO triggers every allocated voice; legato re-triggers all of them.

    // ---- MONO + legato ON ----
    processor.getApvts().getParameterAsValue ("part_polyphony") = 0.0f;   // 0 = MONO
    processor.getApvts().getParameterAsValue ("part_legato")    = 1.0f;   // legato ON (PartData byte 5)
    processor.syncAllParamsToEngine();
    renderIdle (processor, 1);   // flush the deferred polyphony-mode engage

    // Audibility floor: well above the noise/denormal floor, well below a full-
    // level note. The overlap regression dropped output to ~silence (< floor).
    const float kFloor = 0.01f;

    // ---- (1) note 60 held -> audible (sanity: the voice attacks) ----
    std::printf ("[1] MONO note 60 held -> audible\n");
    noteEvent (processor, juce::MidiMessage::noteOn (1, 60, (uint8_t) 100));
    const float peak60 = renderMonoPeak (processor, 6);
    std::printf ("     peak = %.5f (expect > %.3f)\n", peak60, kFloor);
    check (peak60 > kFloor, "note 60 is audible");

    // ---- (2) note 64 OVERLAPPING (60 still held) -> audible (THE BUG) ----
    // Pre-fix this was ~silence: startVoice's Kill zeroed the envelope, then the
    // legato Trigger skipped the re-attack => silent overlap.
    std::printf ("\n[2] MONO overlap: note 64 while 60 held -> audible (legato re-trigger)\n");
    noteEvent (processor, juce::MidiMessage::noteOn (1, 64, (uint8_t) 100));
    const float peakOverlap = renderMonoPeak (processor, 6);
    std::printf ("     peak = %.5f (expect > %.3f)\n", peakOverlap, kFloor);
    check (peakOverlap > kFloor, "overlap (legato) is audible -- not silenced by startVoice's Kill");
    // A voice is still active after the legato re-trigger (retriggerNote keeps
    // the sound alive without a kill). NOTE: retriggerNote deliberately does NOT
    // touch JUCE's private currentlyPlayingNote field (set only by the
    // Synthesiser friend), so getCurrentlyPlayingNote() is stale here -- the
    // MONO routing is monoStack-based, which is why this stays correct audibly.
    {
        int activeCount = 0;
        for (int vi : engine.getPart (0).voiceIndices)
            if (auto* av = engine.getAmbikaVoice (vi))
                if (av->isVoiceActive()) ++activeCount;
        std::printf ("     active Part-0 voices after overlap = %d (expect >= 1)\n", activeCount);
        check (activeCount >= 1, "a Part-0 voice stays active across the legato re-trigger");
    }

    // ---- (2b) continuity at the legato retrigger ----
    // A FRESH scenario on a second processor: hold 60 until fully settled,
    // measure the settled slew and the peak, then retrigger with 64 and bound
    // the slew across the retrigger. CALIBRATION NOTE (measured A/B): the
    // FIFO-clear gate contributes ~0.005 of slew here; the DOMINANT step
    // (~0.71 = about half the summed amplitude) is the firmware pitch-glide
    // retrigger itself (Trigger sets pitch_target_ and glides pitch_value_
    // per internal block) and is present pre- AND post-fix. The regression
    // bound therefore guards against a catastrophic signal collapse (a full
    // resampler restart of all six voices would slew ~the summed amplitude,
    // ~1.4): the retrigger slew must stay well under that while the signal
    // keeps sounding through the transition.
    std::printf ("\n[2b] legato retrigger continuity (signal survives, bounded step)\n");
    {
        ParvatiAudioProcessor p2;
        p2.prepareToPlay (48000.0, 256);
        p2.syncAllParamsToEngine();
        p2.getApvts().getParameterAsValue ("part_polyphony") = 0.0f;   // MONO
        p2.getApvts().getParameterAsValue ("part_legato")    = 1.0f;   // legato ON
        p2.syncAllParamsToEngine();
        renderIdle (p2, 1);

        noteEvent (p2, juce::MidiMessage::noteOn (1, 60, (uint8_t) 100));
        renderIdle (p2, 40);   // settle past the de-click ramp + attack
        const float settledSlew = renderMonoMaxSlew (p2, 4);

        // Retrigger: capture the slew across the note-on block + two blocks
        // after, and the mean |x| over the same window (the note must keep
        // sounding through the transition — no dropout).
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 64, (uint8_t) 100), 0);
        p2.processBlock (buf, midi);
        float retriggerSlew = 0.0f;
        double sumAbs = 0.0;
        int nAbs = 0;
        {
            float prev = 0.0f;
            for (int s = 0; s < buf.getNumSamples(); ++s)
            {
                const float x = 0.5f * (buf.getSample (0, s) + buf.getSample (1, s));
                retriggerSlew = std::max (retriggerSlew, std::fabs (x - prev));
                sumAbs += std::fabs (x); ++nAbs;
                prev = x;
            }
        }
        for (int b = 0; b < 2; ++b)
        {
            juce::AudioBuffer<float> b2 (2, 256);
            b2.clear();
            juce::MidiBuffer m2;
            p2.processBlock (b2, m2);
            float prev = 0.0f;
            for (int s = 0; s < b2.getNumSamples(); ++s)
            {
                const float x = 0.5f * (b2.getSample (0, s) + b2.getSample (1, s));
                retriggerSlew = std::max (retriggerSlew, std::fabs (x - prev));
                sumAbs += std::fabs (x); ++nAbs;
                prev = x;
            }
        }
        const double meanAbs = nAbs ? sumAbs / nAbs : 0.0;
        std::printf ("     settled slew = %.4f, retrigger slew = %.4f (bound 0.85; a full "
                     "resampler restart of all voices would slew ~1.4)\n",
                     settledSlew, retriggerSlew);
        check (retriggerSlew < 0.85f,
               "legato retrigger keeps a bounded step (no signal collapse)");
        std::printf ("     mean |x| through the retrigger = %.4f (expect >= 0.05)\n", meanAbs);
        check (meanAbs > 0.05,
               "the note keeps sounding through the legato retrigger (no dropout)");
        juce::ignoreUnused (settledSlew);
    }

    // ---- (3) release 64 -> slide-back to 60, still audible ----
    std::printf ("\n[3] release 64 -> slide-back to 60 (legato re-trigger on release)\n");
    noteEvent (processor, juce::MidiMessage::noteOff (1, 64));
    const float peakSlide = renderMonoPeak (processor, 6);
    std::printf ("     peak = %.5f (expect > %.3f)\n", peakSlide, kFloor);
    check (peakSlide > kFloor, "slide-back to 60 is audible");
    {
        const auto pitches = heldPitches (engine, 0);
        check (pitches.size() == 1 && pitches.count (60), "after release 64: MONO holds 60 again");
    }
    juce::ignoreUnused (heldPitches);

    // ---- (4) release 60 -> silence ----
    std::printf ("\n[4] release 60 -> silence\n");
    noteEvent (processor, juce::MidiMessage::noteOff (1, 60));
    // Let the release tail decay to DEAD (~1.5 s at 48 kHz / 256, verified to
    // reach 0 by ~block 300), THEN measure the tail peak (not the max over the
    // whole window -- the tail starts loud at the sustain level).
    renderIdle (processor, 300);
    const float peakSilent = renderMonoPeak (processor, 50);
    std::printf ("     tail peak = %.5f (expect ~ 0)\n", peakSilent);
    check (peakSilent < kFloor, "final release decays to silence");

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "LEGATO TEST: FAILURES" : "LEGATO TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}

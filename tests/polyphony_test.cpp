// Polyphony mode test — verifies the 5 firmware modes (Mono/Poly/Unison2x/
// Cyclic/Chain) drive the engine's voice allocator faithfully.
//
// Harness: a ParvatiAudioProcessor; part 0 is given a known voice set via
// setPartVoiceAllocation; notes are played on MIDI channel 1 (part 0's default
// channel) and the active voices are inspected on the engine.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <vector>

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

void noteEvent (ParvatiAudioProcessor& p, const juce::MidiMessage& m)
{
    juce::AudioBuffer<float> buf (2, 256);
    buf.clear();
    juce::MidiBuffer midi;
    midi.addEvent (m, 0);
    p.processBlock (buf, midi);
}

// Render `blocks` audio blocks (no MIDI) and return the mono sum of samples.
// The SAW/SQUARE path (init patch) has no routed noise/random, so output is
// deterministic for a given patch + trigger — enabling a clean A/B diff.
std::vector<float> renderMono (ParvatiAudioProcessor& p, int blocks)
{
    std::vector<float> out;
    for (int i = 0; i < blocks; ++i)
    {
        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        juce::MidiBuffer midi;
        p.processBlock (buf, midi);
        for (int s = 0; s < buf.getNumSamples(); ++s)
            out.push_back (0.5f * (buf.getSample (0, s) + buf.getSample (1, s)));
    }
    return out;
}

double diffRms (const std::vector<float>& a, const std::vector<float>& b)
{
    const size_t n = std::min (a.size(), b.size());
    if (n == 0) return 0.0;
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const double d = a[i] - b[i];
        acc += d * d;
    }
    return std::sqrt (acc / static_cast<double> (n));
}

float peakAbs (const std::vector<float>& a)
{
    float pk = 0.0f;
    for (float x : a) pk = std::max (pk, std::fabs (x));
    return pk;
}

// Count active voices in a part and collect the distinct pitches they hold.
int activeVoices (SynthEngine& e, int part, std::set<int>& pitches)
{
    pitches.clear();
    int n = 0;
    for (int vi : e.getPart (part).voiceIndices)
        if (auto* av = e.getAmbikaVoice (vi))
            if (av->getCurrentlyPlayingNote() >= 0) { ++n; pitches.insert (av->getCurrentlyPlayingNote()); }
    return n;
}

// Set part 0's polyphony mode (1-based choice index) and apply it.
// The mode engage is DEFERRED to the audio thread (markAllocationDirty), so we
// process one idle block to service it before the caller inspects state.
void setMode (ParvatiAudioProcessor& p, int modeIndex)
{
    p.getApvts().getParameterAsValue ("part_polyphony") = static_cast<float> (modeIndex);
    p.syncAllParamsToEngine();
    renderIdle (p, 1);   // flush the deferred allocation/mode change
}
}

int main()
{
    juce::MessageManager::getInstance();
    juce::ScopedJuceInitialiser_GUI guiInit;

    ParvatiAudioProcessor processor;
    processor.prepareToPlay (48000.0, 256);
    processor.syncAllParamsToEngine();

    SynthEngine& engine = processor.getEngine();

    // ---- (0) Hardware default = 6 voices (faithful Ambika) ----
    // The engine defaults to VoiceMode::Hardware (1 voice per voicecard); the
    // single-part default allocation {0x3f,0,...} puts all 6 voicecards on
    // Part 0 => 6 voices total.
    {
        int total = 0;
        for (int p = 0; p < kNumParts; ++p)
            total += static_cast<int> (engine.getPart (p).voiceIndices.size());
        std::printf ("[0] Hardware default voice count = %d (expect 6)\n", total);
        check (total == 6, "default (Hardware) engine exposes 6 voices across parts");
    }

    // The remainder of this test exercises the Extended (16-slot) allocator
    // (UNISON_2X / CYCLIC / CHAIN, multi-slot-per-voicecard), so opt into it.
    engine.setVoiceMode (VoiceMode::Extended);

    // Part 0 owns voicecard 0 only -> voices {0,1,2} (3 voices), predictable.
    engine.setPartVoiceAllocation (0, 0x01);
    renderIdle (processor, 2);

    // ---- (a) part_polyphony routes + default is Poly (1) ----
    std::printf ("[a] part_polyphony routing + default\n");
    {
        ParvatiAudioProcessor fresh;
        fresh.prepareToPlay (48000.0, 256);
        const float def = fresh.getApvts().getRawParameterValue ("part_polyphony")->load();
        std::printf ("     default part_polyphony = %.0f (expect 1 Poly)\n", def);
        check (static_cast<int> (def) == 1, "default polyphony is Poly (1)");
    }
    setMode (processor, 2);   // Unison 2x
    const int byte15 = engine.getPart (0).partBytes[15];
    const int pmMode = engine.getPart (0).polyphonyMode;
    std::printf ("     after set 2: partBytes[15]=%d, polyphonyMode=%d\n", byte15, pmMode);
    check (byte15 == 2 && pmMode == 2, "part_polyphony routes to PartData byte 15 + engine mode");

    // ---- (b) POLY: two notes -> two distinct voices ----
    std::printf ("\n[b] POLY: two notes -> two distinct voices\n");
    setMode (processor, 1);
    noteEvent (processor, juce::MidiMessage::noteOn (1, 60, (uint8_t) 100));
    noteEvent (processor, juce::MidiMessage::noteOn (1, 64, (uint8_t) 100));
    renderIdle (processor, 2);
    {
        std::set<int> pitches;
        const int n = activeVoices (engine, 0, pitches);
        std::printf ("     active=%d distinct pitches=%zu\n", n, pitches.size());
        check (n >= 2, "POLY holds >= 2 voices for 2 notes");
        check (pitches.count (60) && pitches.count (64), "POLY voices hold both 60 and 64");
    }
    noteEvent (processor, juce::MidiMessage::noteOff (1, 60));
    noteEvent (processor, juce::MidiMessage::noteOff (1, 64));
    renderIdle (processor, 2);

    // ---- (c) MONO: note priority + legato retrigger ----
    std::printf ("\n[c] MONO: note priority + retrigger on release\n");
    setMode (processor, 0);
    noteEvent (processor, juce::MidiMessage::noteOn (1, 60, (uint8_t) 100));
    noteEvent (processor, juce::MidiMessage::noteOn (1, 64, (uint8_t) 100));  // newer = top
    renderIdle (processor, 2);
    {
        std::set<int> pitches;
        const int n = activeVoices (engine, 0, pitches);
        std::printf ("     after 60+64: active=%d distinct=%zu (expect all hold 64)\n", n, pitches.size());
        check (pitches.size() == 1 && pitches.count (64), "MONO sounds only the most-recent note (64)");
    }
    noteEvent (processor, juce::MidiMessage::noteOff (1, 64));   // release top -> retrigger 60
    renderIdle (processor, 2);
    {
        std::set<int> pitches;
        activeVoices (engine, 0, pitches);
        std::printf ("     after release 64: distinct=%zu (expect 60)\n", pitches.size());
        check (pitches.size() == 1 && pitches.count (60), "MONO retriggers the prior note (60) on release");
    }
    noteEvent (processor, juce::MidiMessage::noteOff (1, 60));
    renderIdle (processor, 2);

    // ---- (d) CYCLIC: three notes -> three distinct voices (round-robin) ----
    std::printf ("\n[d] CYCLIC: three notes -> round-robin across voices\n");
    setMode (processor, 3);
    noteEvent (processor, juce::MidiMessage::noteOn (1, 60, (uint8_t) 100));
    noteEvent (processor, juce::MidiMessage::noteOn (1, 61, (uint8_t) 100));
    noteEvent (processor, juce::MidiMessage::noteOn (1, 62, (uint8_t) 100));
    renderIdle (processor, 2);
    {
        // Count how many distinct voice indices are active.
        std::set<int> activeVi;
        for (int vi : engine.getPart (0).voiceIndices)
            if (auto* av = engine.getAmbikaVoice (vi))
                if (av->getCurrentlyPlayingNote() >= 0) activeVi.insert (vi);
        std::printf ("     distinct active voices = %zu (expect 3)\n", activeVi.size());
        check (activeVi.size() == 3, "CYCLIC spreads 3 notes across 3 distinct voices");
    }
    noteEvent (processor, juce::MidiMessage::noteOff (1, 60));
    noteEvent (processor, juce::MidiMessage::noteOff (1, 61));
    noteEvent (processor, juce::MidiMessage::noteOff (1, 62));
    renderIdle (processor, 2);

    // ---- (e) UNISON_2X: one note -> two voices ----
    std::printf ("\n[e] UNISON_2X: one note -> two voices\n");
    setMode (processor, 2);
    noteEvent (processor, juce::MidiMessage::noteOn (1, 60, (uint8_t) 100));
    renderIdle (processor, 2);
    {
        // Count voices holding the note just played (release tails from prior
        // sections hold other notes, so counting currentNote==60 is immune).
        int holders = 0;
        for (int vi : engine.getPart (0).voiceIndices)
            if (auto* av = engine.getAmbikaVoice (vi))
                if (av->getCurrentlyPlayingNote() == 60) ++holders;
        std::printf ("     voices holding note 60 = %d (expect 2)\n", holders);
        check (holders == 2, "UNISON_2X triggers 2 voices for 1 note");
    }
    noteEvent (processor, juce::MidiMessage::noteOff (1, 60));
    renderIdle (processor, 2);

    // ---- (f) SPREAD: per-voice detune drift (MONO triggers all of a part's
    //         voices; spread detunes each by i*spread in 1/128-semitone units) ----
    std::printf ("\n[f] SPREAD: per-voice detune across unison voices (MONO)\n");
    setMode (processor, 0);   // MONO -> all of part 0's voices ({0,1,2}) trigger
    // (1) routing: part_spread reaches PartData byte 3.
    processor.getApvts().getParameterAsValue ("part_spread") = 40.0f;
    processor.syncAllParamsToEngine();
    check (engine.getPart (0).partBytes[3] == 40, "part_spread=40 routes to PartData byte 3");
    // (2) A/B render: spread=0 (coherent unison) vs spread=40 (detuned unison).
    auto renderAtSpread = [&] (int spread) -> std::vector<float> {
        processor.getApvts().getParameterAsValue ("part_spread") = static_cast<float> (spread);
        processor.syncAllParamsToEngine();
        noteEvent (processor, juce::MidiMessage::noteOn (1, 60, (uint8_t) 100));
        auto out = renderMono (processor, 8);
        noteEvent (processor, juce::MidiMessage::noteOff (1, 60));
        renderIdle (processor, 1);
        return out;
    };
    const auto zeroOut   = renderAtSpread (0);
    const auto spreadOut = renderAtSpread (40);
    // (3) audible with spread applied.
    const float pk = peakAbs (spreadOut);
    std::printf ("     MONO spread=40 peak = %.5f\n", pk);
    check (pk > 0.01, "MONO with spread>0 produces audible audio");
    // (4) the detuned unison renders differently from the coherent unison.
    const double dRms = diffRms (zeroOut, spreadOut);
    std::printf ("     |spread0 - spread40| RMS = %.6f (expect > 1e-3)\n", dRms);
    check (dRms > 1e-3, "spread=40 output differs from spread=0 (per-voice detune)");

    // ---- (g) CHAIN: internal 2x voice doubling (Option A, no MIDI forward) ----
    std::printf ("\n[g] CHAIN: internal voice doubling (auto-partner)\n");
    // Part 0 = vc0 only (3 voices); Parts 1..5 = nothing -> vc1..5 are free.
    for (int i = 0; i < 6; ++i)
        engine.setPartVoiceAllocation (i, i == 0 ? 0x01 : 0x00);
    renderIdle (processor, 2);
    check (engine.getPart (0).voiceIndices.size() == 3, "Part 0 base allocation = 3 voices (vc0)");
    setMode (processor, 4);   // CHAIN -> rebuildVoiceAllocation auto-doubles via a free partner
    {
        const size_t doubled = engine.getPart (0).voiceIndices.size();
        std::printf ("     CHAIN voiceIndices = %zu (expect 6: 3 base + 3 partner)\n", doubled);
        check (doubled == 6, "CHAIN doubles Part 0 to 6 voices (internal partner from free vc)");
    }
    // 4 simultaneous distinct notes -> all 4 sounding. With only the base 3
    // voices (POLY) the 4th would steal, so 4 distinct held notes proves the
    // doubled capacity. Count only the played notes (immune to release tails).
    noteEvent (processor, juce::MidiMessage::noteOn  (1, 60, (uint8_t) 100));
    noteEvent (processor, juce::MidiMessage::noteOn  (1, 62, (uint8_t) 100));
    noteEvent (processor, juce::MidiMessage::noteOn  (1, 64, (uint8_t) 100));
    noteEvent (processor, juce::MidiMessage::noteOn  (1, 65, (uint8_t) 100));
    renderIdle (processor, 2);
    {
        std::set<int> held;
        for (int vi : engine.getPart (0).voiceIndices)
            if (auto* av = engine.getAmbikaVoice (vi))
            {
                const int nn = av->getCurrentlyPlayingNote();
                if (nn == 60 || nn == 62 || nn == 64 || nn == 65) held.insert (nn);
            }
        std::printf ("     4 notes held -> distinct sounding = %zu (expect 4)\n", held.size());
        check (held.size() == 4, "CHAIN sustains 4 distinct notes (> base 3) — doubled capacity");
    }
    noteEvent (processor, juce::MidiMessage::noteOff (1, 60));
    noteEvent (processor, juce::MidiMessage::noteOff (1, 62));
    noteEvent (processor, juce::MidiMessage::noteOff (1, 64));
    noteEvent (processor, juce::MidiMessage::noteOff (1, 65));
    renderIdle (processor, 2);
    // Switching back to POLY releases the partner (back to the base 3).
    setMode (processor, 1);
    {
        const size_t back = engine.getPart (0).voiceIndices.size();
        std::printf ("     back to POLY: voiceIndices = %zu (expect 3)\n", back);
        check (back == 3, "POLY releases the CHAIN partner (back to 3 voices)");
    }

    // ---- (h) released voice frees even with NO ENV->VCA routing (envelopesDead) ----
    // The init patch's only ENV->VCA routing is mod11 (ENV2->VCA, amount 32).
    // Setting its amount to 0 makes the multiplicative VCA hold ~253 forever
    // (vca() never collapses below 2), so WITHOUT the envelopesDead() guard a
    // released voice would linger active (stuck meter / reduced polyphony)
    // until JUCE stole it. The guard in AmbikaVoice::renderNextBlock frees the
    // voice once ALL three envelopes reach DEAD.
    std::printf ("\n[h] released voice frees with no ENV->VCA routing (envelopesDead)\n");
    {
        ParvatiAudioProcessor p;
        p.prepareToPlay (48000.0, 256);
        p.syncAllParamsToEngine();
        SynthEngine& e = p.getEngine();
        e.setPartVoiceAllocation (0, 0x01);   // Part 0 = vc0 only (3 voices)
        renderIdle (p, 2);

        // Disable the only ENV->VCA routing so the VCA never closes (< 2).
        p.getApvts().getParameterAsValue ("mod11_amount") = 0.0f;
        p.syncAllParamsToEngine();
        renderIdle (p, 1);

        // Trigger -> must be active (VCA ~253, audible).
        noteEvent (p, juce::MidiMessage::noteOn (1, 60, (uint8_t) 100));
        renderIdle (p, 4);
        {
            std::set<int> pitches;
            const int activeBefore = activeVoices (e, 0, pitches);
            std::printf ("     note-on: active=%d (expect >= 1)\n", activeBefore);
            check (activeBefore >= 1, "note 60 is active before release");
        }
        // Release + render well past every envelope's release segment.
        noteEvent (p, juce::MidiMessage::noteOff (1, 60, (uint8_t) 100));
        renderIdle (p, 500);   // ~2.7 s at 48 kHz / 256 -> release -> DEAD
        {
            std::set<int> pitches;
            const int activeAfter = activeVoices (e, 0, pitches);
            std::printf ("     after release+idle: active=%d (expect 0)\n", activeAfter);
            check (activeAfter == 0, "released voice freed via envelopesDead() (no ENV->VCA routing)");
        }
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "POLYPHONY TEST: FAILURES" : "POLYPHONY TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}

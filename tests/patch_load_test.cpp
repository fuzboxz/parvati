// Regression coverage for the preset-switch + DSP fixes:
//   B4 — switching a preset while notes are held leaves NO stuck voice
//        (SynthEngine::resetAllVoices is called at the start of every load).
//   B5 — Mix Crush actually affects the rendered audio (sample-and-hold decimator).

#include <algorithm>
#include <cmath>
#include <cstdio>
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

// Render @p blocks of empty MIDI; returns the last block's mono samples (L).
std::vector<float> renderIdle (ParvatiAudioProcessor& proc, int blocks)
{
    std::vector<float> last;
    for (int b = 0; b < blocks; ++b)
    {
        juce::AudioBuffer<float> buf (2, 256); buf.clear();
        juce::MidiBuffer empty;
        proc.processBlock (buf, empty);
        last.assign (buf.getReadPointer (0), buf.getReadPointer (0) + 256);
    }
    return last;
}

void noteOn (ParvatiAudioProcessor& proc, int ch, int note, int vel)
{
    juce::AudioBuffer<float> buf (2, 256); buf.clear();
    juce::MidiBuffer m;
    m.addEvent (juce::MidiMessage::noteOn (ch, (uint8_t) note, (uint8_t) vel), 0);
    proc.processBlock (buf, m);
}

int countActiveVoices (ParvatiAudioProcessor& proc)
{
    auto& e = proc.getEngine();
    int c = 0;
    for (int i = 0; i < 16; ++i)
        if (auto* v = e.getAmbikaVoice (i); v && v->getCurrentlyPlayingNote() >= 0)
            ++c;
    return c;
}

int countActiveInPart (ParvatiAudioProcessor& proc, int part)
{
    auto& e = proc.getEngine();
    int c = 0;
    for (int i = 0; i < 16; ++i)
        if (auto* v = e.getAmbikaVoice (i); v && v->getCurrentlyPlayingNote() >= 0 && v->getPartIndex() == part)
            ++c;
    return c;
}

double peak (const std::vector<float>& s)
{
    double p = 0.0;
    for (float v : s) p = std::max (p, std::fabs ((double) v));
    return p;
}
}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI guiInit;
    std::printf ("=== Parvati Patch-Load + DSP Regression ===\n");

    ParvatiAudioProcessor proc;
    proc.prepareToPlay (48000.0, 256);
    auto& eng = proc.getEngine();
    auto setP = [&] (const char* id, float v) { proc.getApvts().getParameterAsValue (id) = v; };
    (void) eng;

    // Default is single-part (Part 0 = all voicecards) on MIDI ch1.
    setP ("part_select", 1.0f);
    setP ("part_polyphony", 1.0f);   // POLY
    proc.syncAllParamsToEngine();
    renderIdle (proc, 2);

    // ---------------------------------------------------------------------
    std::printf ("\n[B4] No stuck voice after switching a preset mid-note\n");
    {
        noteOn (proc, 1, 60, 110);
        renderIdle (proc, 4);
        check (countActiveVoices (proc) > 0, "a held note triggers a voice");

        // Load a different template while the note is still held.
        const auto tdir = ParvatiAudioProcessor::getTemplatesDir();
        const juce::File poly6 = tdir.getChildFile ("Poly 6.parvati");
        bool loaded = poly6.existsAsFile() && proc.loadParvatiMultiFile (poly6);
        check (loaded, "Poly 6 template loaded mid-note");
        renderIdle (proc, 6);   // let the reset + release settle

        const int stuck = countActiveVoices (proc);
        char m[96];
        std::snprintf (m, sizeof (m), "no voice stuck after the switch (active=%d, expect 0)", stuck);
        check (stuck == 0, m);
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[B5] Mix Crush alters the rendered output\n");
    {
        // Two engines, same note, only Mix Crush differs. They start phase-aligned
        // (same note + init), so any divergence is the crush (pre-filter sample-
        // and-hold). A no-op crush would leave the two waveforms ~identical.
        auto setup = [] (ParvatiAudioProcessor& p, int crush) {
            p.prepareToPlay (48000.0, 256);
            p.getApvts().getParameterAsValue ("part_select")   = 1.0f;
            p.getApvts().getParameterAsValue ("part_polyphony") = 1.0f;   // POLY
            p.getApvts().getParameterAsValue ("mix_crush")      = (float) crush;
            p.syncAllParamsToEngine();
            renderIdle (p, 2);
            noteOn (p, 1, 60, 110);
            renderIdle (p, 8);
        };
        ParvatiAudioProcessor clean, crushed;
        setup (clean, 0);
        setup (crushed, 28);   // crush() = 29 -> hold 29 samples
        const auto a = renderIdle (clean, 1);
        const auto b = renderIdle (crushed, 1);
        check (peak (a) > 0.01f, "crush=0 produces audible output");
        check (peak (b) > 0.01f, "crush=28 still produces audible output");

        int diffs = 0;
        for (size_t i = 0; i < a.size() && i < b.size(); ++i)
            if (std::fabs ((double) a[i] - (double) b[i]) > 1e-4)
                ++diffs;
        char m[128];
        std::snprintf (m, sizeof (m), "crush materially alters the waveform (%d/%zu samples differ)", diffs, a.size());
        check (diffs > a.size() / 4, m);
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[B8] Multitimbral MIDI input (loaded Multitimbral template)\n");
    {
        ParvatiAudioProcessor mp;
        mp.prepareToPlay (48000.0, 256);
        const juce::File mt = ParvatiAudioProcessor::getTemplatesDir().getChildFile ("Multitimbral.parvati");
        bool loaded = mt.existsAsFile() && mp.loadParvatiMultiFile (mt);
        check (loaded, "Multitimbral template loaded");
        renderIdle (mp, 2);   // flush the deferred rebuild so routing/allocation settle

        noteOn (mp, 1, 60, 110);   // MIDI ch1 -> Part 0
        renderIdle (mp, 4);
        check (countActiveInPart (mp, 0) > 0, "MIDI ch1 triggers Part 0 voices");

        noteOn (mp, 2, 64, 110);   // MIDI ch2 -> Part 1
        renderIdle (mp, 4);
        check (countActiveInPart (mp, 1) > 0, "MIDI ch2 triggers Part 1 voices (multitimbral input)");

        mp.getEngine().resetAllVoices();
        renderIdle (mp, 2);
        // Same-block multi-channel: ch1 + ch2 note-ons in ONE processBlock (how a
        // host like Ableton delivers a multi-channel MIDI clip).
        {
            juce::AudioBuffer<float> buf (2, 256); buf.clear();
            juce::MidiBuffer m;
            m.addEvent (juce::MidiMessage::noteOn (1, 60, (uint8_t) 110), 0);
            m.addEvent (juce::MidiMessage::noteOn (2, 64, (uint8_t) 110), 1);
            mp.processBlock (buf, m);
        }
        renderIdle (mp, 4);
        check (countActiveInPart (mp, 0) > 0, "same-block: ch1 -> Part 0");
        check (countActiveInPart (mp, 1) > 0, "same-block: ch2 -> Part 1");
        // Sanity: the loaded template really routed Parts 0/1 to channels 1/2.
        check (mp.getEngine().getPartChannel (0) == 1, "template: Part 0 on MIDI ch 1");
        check (mp.getEngine().getPartChannel (1) == 2, "template: Part 1 on MIDI ch 2");
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[B9] Multi-load does not clobber Part 0 (engine authoritative)\n");
    {
        ParvatiAudioProcessor mp;
        mp.prepareToPlay (48000.0, 256);
        // Pre-load: init patch has osc1_shape = 0 (NONE). The Mono template
        // carries osc1_shape = 1 (Saw). A clobber (syncAllParamsToEngine pushing
        // stale APVTS back into engine) would leave engine patchBytes[0] at 0.
        const auto tdir = ParvatiAudioProcessor::getTemplatesDir();
        const juce::File mono = tdir.getChildFile ("Mono.parvati");
        check (mono.existsAsFile() && mp.loadParvatiMultiFile (mono), "Mono template loaded");
        renderIdle (mp, 2);

        const uint8_t engineByte = mp.getEngine().getPart (0).patchBytes[0];
        check (engineByte == 1, "Part 0 osc1_shape in engine storage = 1 (Saw, not clobbered)");

        const float apvtsVal = *mp.getApvts().getRawParameterValue ("osc1_shape");
        char m[96];
        std::snprintf (m, sizeof (m), "APVTS osc1_shape reflects engine (%.0f, expect 1)", apvtsVal);
        check (juce::roundToInt (apvtsVal) == 1, m);
    }

    proc.getEngine().resetAllVoices();
    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "PATCH-LOAD TEST: FAILURES" : "PATCH-LOAD TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}

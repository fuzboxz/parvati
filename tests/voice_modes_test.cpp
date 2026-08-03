// Voice-modes / polyphony-modes / templates coverage test.
//
// Proves:
//   A. The VoiceMode capacity switch: Hardware default => 6 voices (single-part
//      default alloc {0x3f} => Part 0 = 1 voice/voicecard across all 6 cards);
//      Extended => Part 0 = 16 voices (full per-voicecard blocks); back to
//      Hardware returns to 6. The default is Hardware.
//   B. Each polyphony mode (Mono/Poly/Unison2x, + Cyclic/Chain smoke) drives the
//      Part-0 allocator correctly on MIDI channel 1.
//   C. Each .parvati template loads and leaves the engine in the advertised
//      config (voice mode + Part-0 voice_allocation + Part-0 polyphony).
//
// Only adds tests/ + a CMake target; no Source/ changes.

#include <cstdio>
#include <set>
#include <string>

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

// Distinct MIDI pitches currently held by a Part's voices (immune to unison:
// MONO / UNISON trigger several voices on the same pitch).
std::set<int> heldPitches (SynthEngine& e, int part)
{
    std::set<int> s;
    for (int vi : e.getPart (part).voiceIndices)
        if (auto* av = e.getAmbikaVoice (vi))
            if (av->getCurrentlyPlayingNote() >= 0) s.insert (av->getCurrentlyPlayingNote());
    return s;
}

// Voices of a Part holding exactly @p pitch (used for UNISON_2X == 2).
int voicesHolding (SynthEngine& e, int part, int pitch)
{
    int n = 0;
    for (int vi : e.getPart (part).voiceIndices)
        if (auto* av = e.getAmbikaVoice (vi))
            if (av->getCurrentlyPlayingNote() == pitch) ++n;
    return n;
}

int totalVoices (SynthEngine& e)
{
    int t = 0;
    for (int p = 0; p < SynthEngine::getNumParts(); ++p)
        t += static_cast<int> (e.getPart (p).voiceIndices.size());
    return t;
}

// Set Part 0's polyphony mode (1-based choice) and flush the deferred rebuild.
void setMode (ParvatiAudioProcessor& p, int modeIndex)
{
    p.getApvts().getParameterAsValue ("part_select") = 1.0f;   // edit Part 0
    p.getApvts().getParameterAsValue ("part_polyphony") = static_cast<float> (modeIndex);
    p.syncAllParamsToEngine();
    renderIdle (p, 2);
}

void allNotesOff (ParvatiAudioProcessor& p)
{
    for (int n = 0; n < 128; ++n)
        noteEvent (p, juce::MidiMessage::noteOff (1, (uint8_t) n));
    renderIdle (p, 2);
}
}  // namespace

int main()
{
    juce::MessageManager::getInstance();
    juce::ScopedJuceInitialiser_GUI guiInit;

    // ======================================================================
    std::printf ("[A] Voice capacity modes (Hardware default / Extended)\n");
    {
        ParvatiAudioProcessor p;
        p.prepareToPlay (48000.0, 256);
        p.syncAllParamsToEngine();
        SynthEngine& e = p.getEngine();

        // A1. Fresh => Hardware default; single-part {0x3f} => Part 0 = 6, rest 0.
        {
            const int p0   = static_cast<int> (e.getPart (0).voiceIndices.size());
            const int tot  = totalVoices (e);
            int others = 0;
            for (int i = 1; i < SynthEngine::getNumParts(); ++i)
                others += static_cast<int> (e.getPart (i).voiceIndices.size());
            std::printf ("     Hardware default: Part0=%d total=%d others=%d (expect 6/6/0)\n", p0, tot, others);
            check (p0 == 6, "Hardware default: Part 0 = 6 voices (1 per voicecard)");
            check (tot == 6, "Hardware default: 6 voices total");
            check (others == 0, "Hardware default: Parts 1-5 own no voices");
        }
        // A2. Extended => Part 0 = 16 (all 6 voicecard blocks: 3+3+3+3+2+2).
        e.setVoiceMode (VoiceMode::Extended);
        renderIdle (p, 2);
        {
            const int p0  = static_cast<int> (e.getPart (0).voiceIndices.size());
            const int tot = totalVoices (e);
            std::printf ("     Extended: Part0=%d total=%d (expect 16/16)\n", p0, tot);
            check (p0 == 16, "Extended: Part 0 = 16 voices (full per-voicecard blocks)");
            check (tot == 16, "Extended: 16 voices total");
        }
        // A3. Back to Hardware => Part 0 = 6 again.
        e.setVoiceMode (VoiceMode::Hardware);
        renderIdle (p, 2);
        {
            const int p0 = static_cast<int> (e.getPart (0).voiceIndices.size());
            std::printf ("     back to Hardware: Part0=%d (expect 6)\n", p0);
            check (p0 == 6, "Hardware (re-switched): Part 0 back to 6 voices");
        }
    }

    // ======================================================================
    std::printf ("\n[B] Polyphony modes (Part 0, MIDI ch1, Hardware 6-voice default)\n");
    {
        ParvatiAudioProcessor p;
        p.prepareToPlay (48000.0, 256);
        p.syncAllParamsToEngine();
        SynthEngine& e = p.getEngine();

        // B-Mono: one pitch sounds; a 2nd note re-steals to the newer pitch.
        setMode (p, 0);   // Mono
        noteEvent (p, juce::MidiMessage::noteOn (1, 60, (uint8_t) 100));
        renderIdle (p, 2);
        {
            auto s = heldPitches (e, 0);
            std::printf ("     Mono 1 note: distinct pitches=%zu (expect 1: {60})\n", s.size());
            check (s.size() == 1 && s.count (60), "Mono: one note => one distinct pitch (60)");
        }
        noteEvent (p, juce::MidiMessage::noteOn (1, 64, (uint8_t) 100));   // newer = top
        renderIdle (p, 2);
        {
            auto s = heldPitches (e, 0);
            std::printf ("     Mono 2nd note: distinct pitches=%zu (expect 1: {64})\n", s.size());
            check (s.size() == 1 && s.count (64), "Mono: 2nd note re-steals to the newer pitch (64)");
        }
        allNotesOff (p);

        // B-Poly: 3 distinct notes => 3 distinct pitches.
        setMode (p, 1);   // Poly
        noteEvent (p, juce::MidiMessage::noteOn (1, 60, (uint8_t) 100));
        noteEvent (p, juce::MidiMessage::noteOn (1, 64, (uint8_t) 100));
        noteEvent (p, juce::MidiMessage::noteOn (1, 67, (uint8_t) 100));
        renderIdle (p, 2);
        {
            auto s = heldPitches (e, 0);
            std::printf ("     Poly 3 notes: distinct pitches=%zu (expect 3)\n", s.size());
            check (s.size() == 3, "Poly: 3 distinct notes => 3 distinct pitches");
        }
        allNotesOff (p);

        // B-Unison2x: one note => exactly 2 voices holding it.
        setMode (p, 2);   // Unison 2x
        noteEvent (p, juce::MidiMessage::noteOn (1, 60, (uint8_t) 100));
        renderIdle (p, 2);
        {
            const int h = voicesHolding (e, 0, 60);
            std::printf ("     Unison2x 1 note: voices holding 60=%d (expect 2)\n", h);
            check (h == 2, "Unison2x: one note => 2 voices");
        }
        allNotesOff (p);

        // B-Cyclic / B-Chain: smoke only (no crash; allocator mechanics are
        // covered in depth by polyphony_test). Voice set stays non-empty + sane.
        setMode (p, 3);   // Cyclic
        noteEvent (p, juce::MidiMessage::noteOn (1, 60, (uint8_t) 100));
        renderIdle (p, 2);
        check (! e.getPart (0).voiceIndices.empty(), "Cyclic: Part 0 keeps a non-empty voice set");
        allNotesOff (p);

        setMode (p, 4);   // Chain (may auto-double; just assert no crash + sane)
        noteEvent (p, juce::MidiMessage::noteOn (1, 60, (uint8_t) 100));
        renderIdle (p, 2);
        check (! e.getPart (0).voiceIndices.empty(), "Chain: Part 0 keeps a non-empty voice set");
        allNotesOff (p);
    }

    // ======================================================================
    std::printf ("\n[C] Templates load to their advertised config\n");
    {
        // Constructing a processor extracts the TEMPLATES bank to app-data.
        ParvatiAudioProcessor p;
        p.prepareToPlay (48000.0, 256);
        p.syncAllParamsToEngine();
        SynthEngine& e = p.getEngine();
        const juce::File tdir = ParvatiAudioProcessor::getTemplatesDir();

        struct Tpl { const char* file; int vm; int p0Alloc; int p0Poly; int p1Alloc; };
        const Tpl tpl[] = {
            { "Mono.parvati",        0, 0x3f, 0, 0    },
            { "Poly 6.parvati",      0, 0x3f, 1, 0    },
            { "Poly 16.parvati",     1, 0x3f, 1, 0    },
            { "Unison.parvati",      1, 0x3f, 2, 0    },
            { "Multitimbral.parvati", 0, 0x15, 1, 0x2a },
        };
        for (const auto& t : tpl)
        {
            const juce::File f = tdir.getChildFile (t.file);
            std::printf ("  %s\n", t.file);
            const bool loaded = p.loadParvatiMultiFile (f);
            check (loaded, "template loaded");
            if (! loaded) continue;
            renderIdle (p, 2);   // flush the deferred rebuild so state is settled

            const int  vm       = p.getUiVoiceMode();
            const int  p0Alloc  = e.getPartVoiceAllocation (0);
            const int  p1Alloc  = e.getPartVoiceAllocation (1);
            const int  p0Poly   = e.getPart (0).partBytes[15];
            char m[128];
            std::snprintf (m, sizeof (m),
                "%s: vm=%d(exp %d) p0alloc=0x%02x(exp 0x%02x) p0poly=%d(exp %d) p1alloc=0x%02x(exp 0x%02x)",
                t.file, vm, t.vm, p0Alloc, t.p0Alloc, p0Poly, t.p0Poly, p1Alloc, t.p1Alloc);
            std::printf ("     %s\n", m);
            check (vm == t.vm,      (std::string (t.file) + ": voice mode").c_str());
            check (p0Alloc == t.p0Alloc, (std::string (t.file) + ": Part 0 voice_allocation").c_str());
            check (p0Poly == t.p0Poly,   (std::string (t.file) + ": Part 0 polyphony").c_str());
            check (p1Alloc == t.p1Alloc, (std::string (t.file) + ": Part 1 voice_allocation").c_str());
        }
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "VOICE MODES TEST: FAILURES" : "VOICE MODES TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}

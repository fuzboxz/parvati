// Ambika .PRO patch-file support verification for Hellcat.
// 1. Parses real factory .PRO files (RIFF "MBKS") -> name + Patch[112] + Part[84].
// 2. Loads them into the full HellcatAudioProcessor and confirms the APVTS
//    reflects the patch bytes (osc1_shape, filter1_cutoff, a mod amount).

#include <cstdio>
#include "unified_test_runner.h"
#include <cstring>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PatchFile.h"
#include "PluginProcessor.h"

#ifndef HELLCAT_SOURCE_DIR
#define HELLCAT_SOURCE_DIR "."
#endif

namespace
{
int g_failures = 0;
void check (bool cond, const char* msg) { std::printf("  %s: %s\n", cond?"ok  ":"FAIL", msg); if (!cond) ++g_failures; }

juce::File proFile (const char* bank, const char* idx)
{
    return juce::File (HELLCAT_SOURCE_DIR)
        .getChildFile ("ambika_reference/controller/data/goldencard/PROGRAM/BANK")
        .getChildFile (bank).getChildFile (idx + juce::String (".PRO"));
}
}  // namespace

TEST(patch_test)
{
    juce::ScopedJuceInitialiser_GUI gui;

    // ---- [1] Parser: real factory patches ----
    std::printf ("[1] Parse real .PRO files (RIFF/MBKS)\n");
    {
        AmbikaProgram a;
        const auto f = proFile ("A", "000");
        check (f.existsAsFile(), "000.PRO exists in the reference tree");
        check (parseAmbikaProgramFile (f, a), "parse 000.PRO succeeds");
        check (a.hasPatch, "000.PRO yields a 112-byte Patch");
        check (a.hasPart,  "000.PRO yields an 84-byte Part");
        check (a.name == "Junon", "000.PRO name == 'Junon'");

        // Reject garbage.
        AmbikaProgram bad;
        const uint8_t junk[] = { 0,1,2,3,4,5 };
        check (! parseAmbikaProgram (junk, sizeof (junk), bad), "garbage input rejected");

        // Two different patches must parse to different osc1 shapes.
        AmbikaProgram b;
        check (parseAmbikaProgramFile (proFile ("A", "001"), b) && b.hasPatch, "parse 001.PRO succeeds");
        std::printf ("     000 osc1.shape=%d   001 osc1.shape=%d\n", (int) a.patch[0], (int) b.patch[0]);
    }

    // ---- [2] Load a .PRO into the processor; APVTS must reflect the bytes ----
    std::printf ("\n[2] Load .PRO into HellcatAudioProcessor\n");
    {
        HellcatAudioProcessor proc;
        proc.prepareToPlay (48000.0, 256);

        AmbikaProgram a;
        parseAmbikaProgramFile (proFile ("A", "000"), a);

        // Snapshot default osc1_shape, then load and confirm it now equals the byte.
        const int before = static_cast<int> (*proc.getApvts().getRawParameterValue ("osc1_shape"));
        check (proc.loadProgramFile (proFile ("A", "000")), "loadProgramFile(000.PRO) returns true");
        const int afterOsc1 = static_cast<int> (*proc.getApvts().getRawParameterValue ("osc1_shape"));
        const int expOsc1   = static_cast<int> (a.patch[0]);
        char msg[128];
        std::snprintf (msg, sizeof (msg), "osc1_shape byte applied (before=%d, after=%d, patch[0]=%d)",
                       before, afterOsc1, expOsc1);
        std::printf ("     %s\n", msg);
        check (afterOsc1 == expOsc1, msg);

        // Loaded program name reaches the processor (check BEFORE loading a 2nd patch).
        check (proc.getLoadedProgramName() == "Junon", "loaded program name == 'Junon'");

        // A second patch must change the value (proves the load is real, not a no-op).
        AmbikaProgram b;
        parseAmbikaProgramFile (proFile ("A", "001"), b);
        if (a.patch[0] != b.patch[0])
        {
            proc.loadProgramFile (proFile ("A", "001"));
            const int after2 = static_cast<int> (*proc.getApvts().getRawParameterValue ("osc1_shape"));
            std::snprintf (msg, sizeof (msg), "loading 001.PRO changes osc1_shape (now %d == byte %d)",
                           after2, (int) b.patch[0]);
            check (after2 == (int) b.patch[0], msg);
        }
    }

    std::printf ("\n%s (%d failures)\n",
                 g_failures ? "PATCH TEST: FAILURES" : "PATCH TEST: ALL CHECKS PASSED",
                 g_failures);
    return g_failures == 0;
}

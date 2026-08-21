// Patch SAVE/LOAD round-trip verification for Parvati.
//
// Proves "presets can be saved and both loaded" end to end:
//   (A) PatchFile unit round-trips: parse a reference .PRO / .MUL, write it,
//       re-parse it, and assert byte-equality of every field (writer == inverse
//       of parser).
//   (B) End-to-end .PRO: processor.loadProgramFile -> saveProgramFile into a
//       2nd processor -> assert every APVTS raw value matches.
//   (C) End-to-end .MUL: processor.loadMultiFile -> saveMultiFile into a 2nd
//       processor -> assert per-part channel/keyrange/voice-allocation match
//       AND the current part's APVTS raw values match.
//
// Built by default. Run with: ./build/parvati_roundtrip_test

#include <cmath>
#include "unified_test_runner.h"
#include <cstdio>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "ParameterLayout.h"
#include "PatchFile.h"
#include "PluginProcessor.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

// Reference Ambika factory presets (GPL-3.0). Discovered via PARVATI_SOURCE_DIR
// (defined by the CMake target), so this works in any build tree that has the
// vendored reference tree present.
juce::File goldencardDir()
{
    return juce::File (PARVATI_SOURCE_DIR)
        .getChildFile ("ambika_reference/controller/data/goldencard");
}
juce::File refPRO() { return goldencardDir().getChildFile ("PROGRAM/BANK/A/000.PRO"); }
juce::File refMUL() { return goldencardDir().getChildFile ("MULTI/BANK/A/000.MUL"); }

// Integer-valued APVTS raw values: allow an exact match or a sub-integer
// tolerance (paranoia vs float denormalization jitter; values are always
// integer-valued so any tolerance < 0.5 is exact in practice).
bool rawEqual (float a, float b) { return std::fabs (a - b) <= 0.5f; }

int countApvtsMismatches (ParvatiAudioProcessor& a, ParvatiAudioProcessor& b)
{
    int mism = 0;
    for (const auto& d : getPatchParamDescriptors())
    {
        const float va = a.getApvts().getRawParameterValue (d.paramID)->load();
        const float vb = b.getApvts().getRawParameterValue (d.paramID)->load();
        if (! rawEqual (va, vb))
            ++mism;
    }
    return mism;
}
}  // namespace

TEST(roundtrip_test)
{
    juce::ScopedJuceInitialiser_GUI juceInit;   // AudioProcessor needs this once.

    const juce::File tmpDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("parvati_roundtrip_test");
    tmpDir.createDirectory();

    // ---------------------------------------------------------------------
    std::printf ("[1] Reference files present\n");
    check (refPRO().existsAsFile(), "reference 000.PRO exists");
    check (refMUL().existsAsFile(), "reference 000.MUL exists");

    // ---------------------------------------------------------------------
    std::printf ("\n[2] PatchFile unit round-trip: .PRO (parse -> write -> parse)\n");
    {
        AmbikaProgram p1, p2;
        check (parseAmbikaProgramFile (refPRO(), p1), "parse reference .PRO");
        const juce::File t = tmpDir.getChildFile ("rt.PRO");
        check (writeAmbikaProgramFile (t, p1), "write .PRO");
        check (parseAmbikaProgramFile (t, p2), "re-parse written .PRO");
        check (p1.hasPatch == p2.hasPatch && p1.hasPart == p2.hasPart, ".PRO has flags equal");
        check (p1.name == p2.name, ".PRO name round-trips");
        check (p1.patch == p2.patch, ".PRO patch[112] byte-equal");
        check (p1.part == p2.part, ".PRO part[84] byte-equal");
        t.deleteFile();
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[3] PatchFile unit round-trip: .MUL (parse -> write -> parse)\n");
    {
        AmbikaMulti m1, m2;
        check (parseAmbikaMultiFile (refMUL(), m1), "parse reference .MUL");
        const juce::File t = tmpDir.getChildFile ("rt.MUL");
        check (writeAmbikaMultiFile (t, m1), "write .MUL");
        check (parseAmbikaMultiFile (t, m2), "re-parse written .MUL");
        check (m1.ok == m2.ok && m1.hasMultiData == m2.hasMultiData, ".MUL ok/hasMultiData equal");
        check (m1.name == m2.name, ".MUL name round-trips");
        check (m1.multiData == m2.multiData, ".MUL multiData[56] byte-equal");
        for (int i = 0; i < 6; ++i)
        {
            char m[96];
            std::snprintf (m, sizeof (m), ".MUL part %d patch[112] byte-equal", i);
            check (m1.parts[(size_t) i].hasPatch == m2.parts[(size_t) i].hasPatch && m1.parts[(size_t) i].patch == m2.parts[(size_t) i].patch, m);
            std::snprintf (m, sizeof (m), ".MUL part %d part[84] byte-equal", i);
            check (m1.parts[(size_t) i].hasPart == m2.parts[(size_t) i].hasPart && m1.parts[(size_t) i].part == m2.parts[(size_t) i].part, m);
        }
        t.deleteFile();
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[4] End-to-end .PRO (load -> save -> load into 2nd proc)\n");
    {
        ParvatiAudioProcessor a, b;
        a.prepareToPlay (48000.0, 512);
        b.prepareToPlay (48000.0, 512);

        check (a.loadProgramFile (refPRO()), "proc A loads reference .PRO");
        const juce::File t = tmpDir.getChildFile ("e2e.PRO");
        check (a.saveProgramFile (t), "proc A saves .PRO");
        check (b.loadProgramFile (t), "proc B loads saved .PRO");

        const int mism = countApvtsMismatches (a, b);
        std::printf ("     APVTS mismatches = %d\n", mism);
        check (mism == 0, "end-to-end .PRO: every APVTS value matches");
        t.deleteFile();
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[5] End-to-end .MUL (load -> save -> load into 2nd proc)\n");
    {
        ParvatiAudioProcessor a, b;
        a.prepareToPlay (48000.0, 512);
        b.prepareToPlay (48000.0, 512);

        check (a.loadMultiFile (refMUL()), "proc A loads reference .MUL");
        const juce::File t = tmpDir.getChildFile ("e2e.MUL");
        check (a.saveMultiFile (t), "proc A saves .MUL");
        check (b.loadMultiFile (t), "proc B loads saved .MUL");

        int routeMism = 0;
        for (int i = 0; i < SynthEngine::getNumParts(); ++i)
        {
            if (a.getEngine().getPartChannel (i) != b.getEngine().getPartChannel (i)) ++routeMism;
            if (a.getEngine().getPartKeyrangeLow (i) != b.getEngine().getPartKeyrangeLow (i)) ++routeMism;
            if (a.getEngine().getPartKeyrangeHigh (i) != b.getEngine().getPartKeyrangeHigh (i)) ++routeMism;
            if (a.getEngine().getPartVoiceAllocation (i) != b.getEngine().getPartVoiceAllocation (i)) ++routeMism;
        }
        std::printf ("     per-part routing mismatches = %d\n", routeMism);
        check (routeMism == 0, "end-to-end .MUL: all 6 parts' channel/keyrange/alloc match");

        const int apvtsMism = countApvtsMismatches (a, b);   // current part (0) APVTS
        std::printf ("     current-part APVTS mismatches = %d\n", apvtsMism);
        check (apvtsMism == 0, "end-to-end .MUL: current-part APVTS matches");
        t.deleteFile();
    }

    tmpDir.deleteRecursively();

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "ROUNDTRIP TEST: FAILURES" : "ROUNDTRIP TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}

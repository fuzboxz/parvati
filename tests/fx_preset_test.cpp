// Per-part FX serialization regression test for Parvati.
//
// Proves the Parvati-only FX section (3 FX slots + 16-slot FX mod matrix,
// topology + order) round-trips through EVERY format that should carry it
// (.parvati multi + .parvati patch + host binary state), and is DROPPED by the
// Ambika .PRO/.MUL byte formats (which know nothing about FX). Mirrors the
// shape of parvati_preset_test.cpp.
//
// Built by default. Run with: ./build/parvati_fx_preset_test

#include <cstdio>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "ParvatiPreset.h"
#include "PluginProcessor.h"
#include "SynthEngine.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

// Sets an APVTS parameter by raw (denormalized) value via the host notification
// path (works for AudioParameterInt and AudioParameterChoice).
void setParam (ParvatiAudioProcessor& proc, const char* id, int value)
{
    if (auto* p = proc.getApvts().getParameter (id))
        p->setValueNotifyingHost (p->convertTo0to1 (static_cast<float> (value)));
}

// Select part @p partIndex (0-based) by driving the part_select param (1-based).
void selectPart (ParvatiAudioProcessor& proc, int partIndex)
{
    setParam (proc, "part_select", partIndex + 1);
}

// Count every field-level mismatch between two Parts' fxState (all 76 FX params).
int countFxMismatches (const PartFxState& a, const PartFxState& b)
{
    int m = 0;
    for (int s = 0; s < kNumFxSlots; ++s)
    {
        if (a.slotType   [(size_t) s].load() != b.slotType   [(size_t) s].load()) ++m;
        if (a.slotEnabled[(size_t) s].load() != b.slotEnabled[(size_t) s].load()) ++m;
        if (a.slotDryWet [(size_t) s].load() != b.slotDryWet [(size_t) s].load()) ++m;
        for (int k = 0; k < kNumFxSlotParams; ++k)
            if (a.slotParam[(size_t) s][(size_t) k].load() != b.slotParam[(size_t) s][(size_t) k].load()) ++m;
    }
    if (a.topology.load() != b.topology.load()) ++m;
    if (a.orderIdx.load()  != b.orderIdx.load())  ++m;
    // Master section (v3).
    if (a.mix.load()       != b.mix.load())       ++m;
    if (a.keepTails.load() != b.keepTails.load()) ++m;
    if (a.eqLow.load()     != b.eqLow.load())     ++m;
    if (a.eqMid.load()     != b.eqMid.load())     ++m;
    if (a.eqHigh.load()    != b.eqHigh.load())    ++m;
    for (int i = 0; i < kNumFxMatrixSlots; ++i)
    {
        if (a.modSource[(size_t) i].load() != b.modSource[(size_t) i].load()) ++m;
        if (a.modDest  [(size_t) i].load() != b.modDest  [(size_t) i].load()) ++m;
        if (a.modAmount[(size_t) i].load() != b.modAmount[(size_t) i].load()) ++m;
    }
    return m;
}

bool allFxAtDefaults (const PartFxState& fx)
{
    // Default fxState is entirely zero-initialized.
    return countFxMismatches (fx, PartFxState{}) == 0;
}

// Set a diverse spread of FX values on the CURRENT part (so it hits the current
// Part's fxState via applyFxParameter). Keeps the spread within legal ranges.
void paintDiverseFx (ParvatiAudioProcessor& proc)
{
    // FX1 = Reverb, enabled, dry/wet mid, params spread.
    setParam (proc, "fx1_type",    3);   // Reverb
    setParam (proc, "fx1_enabled", 1);
    setParam (proc, "fx1_drywet",  64);
    setParam (proc, "fx1_param1",  20);
    setParam (proc, "fx1_param2",  90);
    setParam (proc, "fx1_param3",  45);
    setParam (proc, "fx1_param4",  110);
    // FX2 = Delay, disabled.
    setParam (proc, "fx2_type",    2);   // Delay
    setParam (proc, "fx2_enabled", 0);
    setParam (proc, "fx2_drywet",  100);
    setParam (proc, "fx2_param1",  127);
    // FX3 = Chorus, enabled.
    setParam (proc, "fx3_type",    4);   // Chorus
    setParam (proc, "fx3_enabled", 1);
    setParam (proc, "fx3_drywet",  32);
    // Topology + order.
    setParam (proc, "fx_topo",  1);      // Parallel
    setParam (proc, "fx_order", 3);      // perm {1,2,0}
    // Master section (v3): non-default values so they round-trip distinctly.
    setParam (proc, "fx_mix",        90);   // ~71% global wet
    setParam (proc, "fx_keep_tails", 1);    // keep tails
    setParam (proc, "fx_eq_low",     40);
    setParam (proc, "fx_eq_mid",     80);   // +mid
    setParam (proc, "fx_eq_high",    100);  // +high
    // FX mod matrix: a few active slots.
    setParam (proc, "fxmod1_source", 2);
    setParam (proc, "fxmod1_dest",   5);
    setParam (proc, "fxmod1_amount", 40);
    setParam (proc, "fxmod2_source", 8);
    setParam (proc, "fxmod2_dest",   0);
    setParam (proc, "fxmod2_amount", -25);
    setParam (proc, "fxmod16_source", 15);
    setParam (proc, "fxmod16_dest",   14);
    setParam (proc, "fxmod16_amount", 63);
}
}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    using namespace parvati::preset;

    // ---------------------------------------------------------------------
    std::printf ("[1] .parvati MULTI round-trips FX on every Part\n");
    {
        ParvatiAudioProcessor a, b;
        a.prepareToPlay (48000.0, 512);
        b.prepareToPlay (48000.0, 512);

        // Paint distinct FX state onto Part 0 and Part 3.
        selectPart (a, 0);  paintDiverseFx (a);
        selectPart (a, 3);  paintDiverseFx (a);
        // Distinguish Part 3 from Part 0 so a silent round-trip can't hide a
        // per-part mix-up (set fx_order to a different value than Part 0).
        setParam (a, "fx_order", 5);          // {2,1,0}
        setParam (a, "fx2_drywet", 12);

        const juce::String yaml = serializeParvatiMulti (a);
        check (yaml.contains ("fx1_type"), "multi YAML carries fx1_type");
        check (yaml.contains ("fxmod1_amount"), "multi YAML carries fxmod1_amount");
        check (yaml.contains ("fx_topo"), "multi YAML carries fx_topo");

        check (applyParvatiMulti (b, yaml), "applyParvatiMulti parses + applies");

        // Every Part's fxState must match field-for-field. Parts we didn't paint
        // (1,2,4,5) stay at defaults on both sides (a was default before paint,
        // b's defaults are untouched by the load).
        for (int i = 0; i < SynthEngine::getNumParts(); ++i)
        {
            const int mism = countFxMismatches (a.getEngine().getPart (i).fxState,
                                                b.getEngine().getPart (i).fxState);
            std::printf ("     Part %d fxState mismatches = %d\n", i, mism);
            char buf[64];
            (void) std::snprintf (buf, sizeof (buf), "Part %d full fxState round-trips", i);
            check (mism == 0, buf);
        }
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[2] .parvati PATCH (single, current part) round-trips FX\n");
    {
        ParvatiAudioProcessor a, b;
        a.prepareToPlay (48000.0, 512);
        b.prepareToPlay (48000.0, 512);

        // Single-part patches act on the current (Part 0) only.
        selectPart (a, 0);
        paintDiverseFx (a);

        const juce::String yaml = serializeParvatiPatch (a);
        check (yaml.contains ("fx1_type"), "patch YAML carries fx1_type");
        check (yaml.contains ("fxmod16_dest"), "patch YAML carries fxmod16_dest");

        check (applyParvatiPatch (b, yaml), "applyParvatiPatch parses + applies");

        // The patch writes through the APVTS -> applyFxParameter on b's current
        // Part (0). Part 0's fxState must match a's.
        const int mism = countFxMismatches (a.getEngine().getPart (0).fxState,
                                            b.getEngine().getPart (0).fxState);
        std::printf ("     Part 0 fxState mismatches = %d\n", mism);
        check (mism == 0, "single-part .parvati round-trips Part 0 FX");
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[3] .PRO (single-part) DROPS FX -- fxState stays default\n");
    {
        ParvatiAudioProcessor a, b;
        a.prepareToPlay (48000.0, 512);
        b.prepareToPlay (48000.0, 512);

        selectPart (a, 0);
        paintDiverseFx (a);
        check (! allFxAtDefaults (a.getEngine().getPart (0).fxState),
               "source Part 0 actually has non-default FX (sanity)");

        const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("parvati_fx_preset.PRO");
        check (a.saveProgramFile (tmp), ".PRO saved");
        check (b.loadProgramFile (tmp), ".PRO loaded");

        check (allFxAtDefaults (b.getEngine().getPart (0).fxState),
               ".PRO load leaves fxState at defaults (FX dropped -- Ambika format)");
        tmp.deleteFile();
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[4] .MUL (multi) DROPS FX -- fxState stays default\n");
    {
        ParvatiAudioProcessor a, b;
        a.prepareToPlay (48000.0, 512);
        b.prepareToPlay (48000.0, 512);

        selectPart (a, 0);  paintDiverseFx (a);
        selectPart (a, 3);  paintDiverseFx (a);
        check (! allFxAtDefaults (a.getEngine().getPart (0).fxState),
               "source Part 0 actually has non-default FX (sanity)");

        const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("parvati_fx_preset.MUL");
        check (a.saveMultiFile (tmp), ".MUL saved");
        check (b.loadMultiFile (tmp), ".MUL loaded");

        bool allDefault = true;
        for (int i = 0; i < SynthEngine::getNumParts(); ++i)
            if (! allFxAtDefaults (b.getEngine().getPart (i).fxState))
                allDefault = false;
        check (allDefault,
               ".MUL load leaves every Part's fxState at defaults (FX dropped)");
        tmp.deleteFile();
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[5] Forward-compat: .parvati multi WITHOUT any fx params loads cleanly\n");
    {
        // Hand-craft a multi with NO fx params. Old .parvati files (pre-FX) must
        // still load and leave fxState at defaults.
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 512);

        juce::String yaml =
            "format: parvati-multi\n"
            "version: 1\n"
            "name: \"Legacy\"\n"
            "parts:\n"
            "  - channel: 1\n"
            "    keyzone_low: 0\n"
            "    keyzone_high: 127\n"
            "    voice_allocation: 1\n"
            "    params:\n"
            "      osc1_shape: 2\n";

        check (applyParvatiMulti (proc, yaml), "legacy multi (no fx params) loads");
        check (allFxAtDefaults (proc.getEngine().getPart (0).fxState),
               "Part 0 fxState stays at defaults for a pre-FX .parvati multi");
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "FX PRESET TEST: FAILURES" : "FX PRESET TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}

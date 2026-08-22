// Parvati-native YAML preset format round-trip + forward-compat verification.
//
// Proves the .parvati format carries EVERYTHING Parvati can do — including the
// Parvati-only `vca_curve` / `filter_card` options and the arp settings that the
// Ambika .PRO byte format silently drops. A patch sculpted with the SVF +
// exponential VCA survives a .parvati save+load intact, whereas a .PRO of the
// same state reverts them.
//
// Built by default. Run with: ./build/parvati_preset_test

#include <cmath>
#include "unified_test_runner.h"
#include "test_utils.h"
#include <cstdio>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "ParameterLayout.h"
#include "ParvatiPreset.h"
#include "PluginProcessor.h"

namespace
{
int g_failures = 0;

void check (bool cond, const char* msg)
{
    std::printf ("  %s: %s\n", cond ? "ok  " : "FAIL", msg);
    if (! cond) ++g_failures;
}

bool rawEqual (float a, float b) { return std::fabs (a - b) <= 0.5f; }

int countApvtsMismatches (ParvatiAudioProcessor& a, ParvatiAudioProcessor& b)
{
    int mism = 0;
    for (const auto& d : getPatchParamDescriptors())
    {
        const float va = a.getApvts().getRawParameterValue (d.paramID)->load();
        const float vb = b.getApvts().getRawParameterValue (d.paramID)->load();
        if (! rawEqual (va, vb)) ++mism;
    }
    return mism;
}
}  // namespace

TEST(parvati_preset_test)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    using namespace parvati::preset;

    // ---------------------------------------------------------------------
    std::printf ("[1] YAML emit/parse round-trips a var tree\n");
    {
        auto root = std::make_unique<juce::DynamicObject>();
        root->setProperty ("format", "parvati-patch");
        root->setProperty ("version", 1);
        root->setProperty ("name", "Test");
        auto params = std::make_unique<juce::DynamicObject>();
        params->setProperty ("osc1_shape", 1);
        params->setProperty ("osc1_range", -12);
        params->setProperty ("vca_curve", 1);
        root->setProperty ("params", juce::var (params.release()));

        const juce::String text = emitParvatiYaml (juce::var (root.release()));
        const juce::var parsed = parseParvatiYaml (text);
        check (parsed.isObject(), "parsed tree is an object");
        check ((int) parsed["version"] == 1, "version round-trips as int");
        check ((int) parsed["params"]["osc1_shape"] == 1, "nested param round-trips");
        check ((int) parsed["params"]["osc1_range"] == -12, "negative int round-trips");
        check ((int) parsed["params"]["vca_curve"] == 1, "vca_curve round-trips");
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[2] .parvati patch round-trip: vca_curve/filter_card/arp survive\n");
    {
        ParvatiAudioProcessor a, b;
        a.prepareToPlay (48000.0, 512);
        b.prepareToPlay (48000.0, 512);

        // Set several params, INCLUDING the Parvati-only options + arp that .PRO
        // drops. Use non-default values so a drop is detectable.
        setParam (a, "osc1_shape", 1);         // Saw
        setParam (a, "osc1_range", -12);
        setParam (a, "env2_sustain", 80);
        setParam (a, "vca_curve", 1);          // Exponential  <-- .PRO drops this
        setParam (a, "filter_card", 2);        // 2-pole SVF    <-- .PRO drops this
        setParam (a, "arp_mode", 1);           // Arp           <-- .PRO drops this
        setParam (a, "arp_resolution", 10);
        a.syncAllParamsToEngine();

        const juce::String yaml = serializeParvatiPatch (a);
        check (yaml.contains ("vca_curve: 1"), "YAML contains vca_curve=1");
        check (yaml.contains ("filter_card: 2"), "YAML contains filter_card=2");
        check (yaml.contains ("arp_mode: 1"), "YAML contains arp_mode=1");
        check (yaml.contains ("parvati_version: 0.1.0"), "parvati_version is 0.1.0 (pre-release)");

        check (applyParvatiPatch (b, yaml), "applyParvatiPatch parses + applies");

        const int mism = countApvtsMismatches (a, b);
        std::printf ("     APVTS mismatches = %d\n", mism);
        check (mism == 0, "end-to-end .parvati: EVERY APVTS value matches (incl options/arp)");

        // Specifically prove the gap is closed:
        check (rawEqual (a.getApvts().getRawParameterValue ("vca_curve")->load(),
                         b.getApvts().getRawParameterValue ("vca_curve")->load()),
               "vca_curve survives .parvati round-trip");
        check (rawEqual (a.getApvts().getRawParameterValue ("filter_card")->load(),
                         b.getApvts().getRawParameterValue ("filter_card")->load()),
               "filter_card survives .parvati round-trip");
        check (rawEqual (a.getApvts().getRawParameterValue ("arp_mode")->load(),
                         b.getApvts().getRawParameterValue ("arp_mode")->load()),
               "arp_mode survives .parvati round-trip");
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[3] Contrast: .PRO of the same state DROPS the options\n");
    {
        ParvatiAudioProcessor a, b;
        a.prepareToPlay (48000.0, 512);
        b.prepareToPlay (48000.0, 512);

        setParam (a, "vca_curve", 1);
        setParam (a, "filter_card", 2);
        a.syncAllParamsToEngine();

        const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("parvati_preset_gap.PRO");
        check (a.saveProgramFile (tmp), ".PRO saved");
        check (b.loadProgramFile (tmp), ".PRO loaded");

        const bool vcaKept = rawEqual (a.getApvts().getRawParameterValue ("vca_curve")->load(),
                                       b.getApvts().getRawParameterValue ("vca_curve")->load());
        const bool cardKept = rawEqual (a.getApvts().getRawParameterValue ("filter_card")->load(),
                                        b.getApvts().getRawParameterValue ("filter_card")->load());
        std::printf ("     .PRO keeps vca_curve=%d, filter_card=%d (expect 0)\n", vcaKept, cardKept);
        check (! vcaKept, ".PRO DROPS vca_curve (documenting the gap this format fixes)");
        check (! cardKept, ".PRO DROPS filter_card (documenting the gap this format fixes)");
        tmp.deleteFile();
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[4] Forward-compat: unknown keys + typo'd paramIDs are ignored\n");
    {
        ParvatiAudioProcessor proc;
        proc.prepareToPlay (48000.0, 512);

        juce::String yaml =
            "format: parvati-patch\n"
            "version: 1\n"
            "name: \"Future\"\n"
            "params:\n"
            "  future_param: 99\n"        // unknown key -> ignored
            "  osc1_shape: 2\n"           // Square
            "  typo_paramID: 5\n"         // unknown -> ignored
            "  env2_sustain: 60\n";       // known -> applied

        check (applyParvatiPatch (proc, yaml), "load succeeds with unknown keys");
        check ((int) proc.getApvts().getRawParameterValue ("osc1_shape")->load() == 2,
               "known value (osc1_shape=2) applied despite unknown keys");
        check ((int) proc.getApvts().getRawParameterValue ("env2_sustain")->load() == 60,
               "known value (env2_sustain=60) applied");
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[5] .parvati MULTI round-trip (all 6 parts + routing)\n");
    {
        ParvatiAudioProcessor a, b;
        a.prepareToPlay (48000.0, 512);
        b.prepareToPlay (48000.0, 512);

        // Customize a couple of parts' routing + arp directly on the engine.
        // (Write pendingConfig_ + flag configDirty_ -- the authoritative arp/seq
        // config; the live objects lag it until the audio thread services
        // configDirty_, and serializeParvatiMulti reads pendingConfig_.)
        a.getEngine().getPart (1).pendingConfig_.arpOctave = 3;
        a.getEngine().getPart (1).configDirty_.store (true);
        a.getEngine().setPartChannel (1, 3);
        a.getEngine().setPartKeyrange (1, 36, 60);
        a.getEngine().getPart (2).pendingConfig_.arpResolution = 6;
        a.getEngine().getPart (2).configDirty_.store (true);
        a.getEngine().setPartChannel (2, 5);
        // And a global option.
        setParam (a, "filter_card", 1);   // SSM2164
        a.syncAllParamsToEngine();

        const juce::String yaml = serializeParvatiMulti (a);
        check (yaml.contains ("format: parvati-multi"), "multi YAML has format tag");
        check (applyParvatiMulti (b, yaml), "applyParvatiMulti parses + applies");

        int routeMism = 0;
        for (int i = 0; i < SynthEngine::getNumParts(); ++i)
        {
            if (a.getEngine().getPartChannel (i) != b.getEngine().getPartChannel (i)) ++routeMism;
            if (a.getEngine().getPartKeyrangeLow (i) != b.getEngine().getPartKeyrangeLow (i)) ++routeMism;
            if (a.getEngine().getPartKeyrangeHigh (i) != b.getEngine().getPartKeyrangeHigh (i)) ++routeMism;
            if (a.getEngine().getPartVoiceAllocation (i) != b.getEngine().getPartVoiceAllocation (i)) ++routeMism;
        }
        std::printf ("     per-part routing mismatches = %d\n", routeMism);
        check (routeMism == 0, "all 6 parts' channel/keyrange/alloc match");

        check (a.getEngine().getPart (1).pendingConfig_.arpOctave == b.getEngine().getPart (1).pendingConfig_.arpOctave,
               "Part 1 arp_octave preserved");
        check (a.getEngine().getPart (2).pendingConfig_.arpResolution == b.getEngine().getPart (2).pendingConfig_.arpResolution,
               "Part 2 arp_resolution preserved");
        check (rawEqual (a.getApvts().getRawParameterValue ("filter_card")->load(),
                         b.getApvts().getRawParameterValue ("filter_card")->load()),
               "global filter_card option preserved");

        // Current-part (0) APVTS should match too.
        const int apvtsMism = countApvtsMismatches (a, b);
        check (apvtsMism == 0, "current-part APVTS matches after multi round-trip");
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[6] .parvati PATCH load: part_raga is the whole tuning state\n");
    {
        // The custom-tuning subsystem was removed (2026-08-19): part_raga /
        // PartData byte 4 is the whole tuning state. A patch file whose params
        // say part_raga: 0 must land the part at 12-EDO, and one carrying a
        // preset must land at that preset.
        ParvatiAudioProcessor p;
        p.prepareToPlay (48000.0, 512);
        p.getApvts().getParameterAsValue ("part_raga") = 7.0f;
        check (p.getEngine().resolvedTuningMode (0) == 7,
               "precondition: preset 7 selected");

        juce::File f = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("parvati_t1_patch.parvati");
        check (f.replaceWithText (
                   "format: parvati-patch\nversion: 1\nname: \"T\"\nparams:\n"
                   "  part_raga: 0\n"),
               "T1 patch file written");
        check (p.loadParvatiPatchFile (f), "T1: .parvati patch loads");
        check (p.getEngine().resolvedTuningMode (0) == 0,
               "T1: part_raga 0 patch load lands at 12-EDO");
        f.deleteFile();
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[7] .parvati MULTI load: no tuning keys emitted; legacy keys accepted\n");
    {
        // The serializer emits NO tuning_mode/tuning_offsets keys (the raga
        // rides params: part_raga); a saved multi must round-trip the raga,
        // and every part must resolve its stored mode. Legacy files carrying
        // tuning_mode still load (custom 33 -> 12-EDO) — see parvati_tuning_test
        // [6] for the legacy-key acceptance coverage.
        ParvatiAudioProcessor a, b;
        a.prepareToPlay (48000.0, 512);
        b.prepareToPlay (48000.0, 512);

        a.getApvts().getParameterAsValue ("part_raga") = 3.0f;

        juce::File f = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("parvati_t2_multi.parvati");
        check (a.saveParvatiMultiFile (f), "T2: multi saved with raga 3 on part 0");
        {
            juce::String text;
            if (juce::FileInputStream in (f); in.openedOk())
                text = in.readEntireStreamAsString();
            check (text.isNotEmpty() && ! text.contains ("tuning_mode")
                       && ! text.contains ("tuning_offsets"),
                   "T2: premise — the serializer emits NO tuning keys");
        }

        check (b.loadParvatiMultiFile (f), "T2: .parvati multi loads");
        check (b.getEngine().resolvedTuningMode (0) == 3,
               "T2: part 0 resolves preset 3 (raga rides params)");
        bool restEdo = true;
        for (int i = 1; i < SynthEngine::getNumParts(); ++i)
            restEdo = restEdo && b.getEngine().resolvedTuningMode (i) == 0;
        check (restEdo, "T2: every other part resolves 12-EDO");
        f.deleteFile();
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[T3] Wave 2: degenerate multi docs rejected; missing routing keys default\n");
    {
        ParvatiAudioProcessor a, b;
        a.prepareToPlay (48000.0, 512);
        b.prepareToPlay (48000.0, 512);

        // (a) `parts: []` (and non-object entries) must be REJECTED before any
        // engine mutation: a degenerate list previously "loaded" over the
        // previous multi's leftover state and even reported success.
        juce::File empty = juce::File::getSpecialLocation (juce::File::tempDirectory)
                               .getChildFile ("parvati_t3_empty.parvati");
        empty.replaceWithText ("format: parvati-multi\nversion: 1\nname: \"Empty\"\nparts: []\n");
        b.getEngine().setPartVoiceSlots (0, 11);   // distinctive pre-state
        b.getEngine().setPartName (0, " sentinel");
        check (! b.loadParvatiMultiFile (empty), "T3: empty parts array rejected");
        check (b.getEngine().getPartVoiceSlots (0) == 11,
               "T3: failed load leaves slots untouched (no init reset)");
        check (b.getEngine().getPartName (0) == " sentinel",
               "T3: failed load leaves part names untouched");

        juce::File scalars = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("parvati_t3_scalars.parvati");
        scalars.replaceWithText ("format: parvati-multi\nparts:\n  - 7\n  - 9\n");
        check (! b.loadParvatiMultiFile (scalars), "T3: non-object parts entries rejected");

        // (b) A PRESENT part entry without routing keys must not inherit the
        // PREVIOUS multi's channel/zone: absent keys fall back to the engine
        // init defaults (channel = partIndex + 1, zone 0..127). Entry 1 below
        // is PRESENT (so the loader reaches it) but carries only a name.
        juce::File sparse = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("parvati_t3_sparse.parvati");
        sparse.replaceWithText (
            "format: parvati-multi\nversion: 1\nname: \"Sparse\"\nparts:\n"
            "  - name: \"A\"\n    voice_slots: 4\n"
            "  - name: \"X\"\n    voice_slots: 4\n");
        // Pollute part 1's routing so an inherit would be visible (part 1
        // would keep channel 5 / zone 36..60 instead of the init 2 / 0..127).
        b.getEngine().setPartChannel (1, 5);
        b.getEngine().setPartKeyrange (1, 36, 60);
        check (b.loadParvatiMultiFile (sparse), "T3: sparse multi loads");
        check (b.getEngine().getPartChannel (1) == 2,
               "T3: part 1 gets INIT channel (2) — not the previous multi's 5");
        check (b.getEngine().getPartKeyrangeLow (1) == 0
                   && b.getEngine().getPartKeyrangeHigh (1) == 127,
               "T3: part 1 gets INIT zone 0..127 — not the previous 36..60");
        empty.deleteFile();
        scalars.deleteFile();
        sparse.deleteFile();
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[T4] Wave 2: quoted top-level names round-trip\n");
    {
        ParvatiAudioProcessor a, b;
        a.prepareToPlay (48000.0, 512);
        b.prepareToPlay (48000.0, 512);
        // A name with a double quote AND a newline: the old emitter wrote both
        // raw between quotes — the quote truncated the name on reload and the
        // newline SPLIT the line-based document (params: never parsed -> the
        // load failed silently). Control chars are stripped on save; quotes
        // and backslashes escape exactly like per-part names.
        a.setLoadedProgramName ("My \"Cool\"\\Patch\nName");
        juce::File f = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("parvati_t4_name.parvati");
        check (a.saveParvatiMultiFile (f), "T4: multi with hostile name saved");
        check (b.loadParvatiMultiFile (f), "T4: multi with hostile name LOADS (was a silent parse failure)");
        // NOTE: the loaded program name comes from the FILENAME on this path
        // (loadParvatiMultiFile's tail), not the document — the escaping
        // observable is the PARSED DOCUMENT's name: the raw `\"` in the file
        // must survive emit->parse round-trip as the quote, and the newline
        // must never have entered the line-based document at all.
        {
            juce::String saved;
            if (juce::FileInputStream in (f); in.openedOk())
                saved = in.readEntireStreamAsString();
            const juce::var re = parseParvatiYaml (saved);
            check (re.isObject(), "T4: saved document still parses after the hostile name");
            if (re.isObject())
            {
                const juce::String got = re["name"].toString();
                check (got == "My \"Cool\"\\PatchName",
                       "T4: name round-trips (quote + backslash unescaped, newline stripped)");
            }
            else
                check (false, "T4: name round-trips (quote + backslash unescaped, newline stripped)");
        }
        f.deleteFile();
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[T5] Wave 2: part NAMES lifecycle across loads\n");
    {
        ParvatiAudioProcessor p;
        p.prepareToPlay (48000.0, 512);
        auto& eng = p.getEngine();

        // Drum Kit template: every part carries a name (Kick/Snare/...).
        const juce::File tplDir = juce::File::getCurrentWorkingDirectory()
                                      .getChildFile ("presets/TEMPLATES");
        const juce::File drum = tplDir.getChildFile ("Drum Kit (GM).parvati");
        if (drum.existsAsFile())
        {
            check (p.loadParvatiMultiFile (drum), "T5: Drum Kit loads");
            check (eng.getPartName (0) == "Kick", "T5: Drum Kit part 0 named 'Kick'");

            // A .MUL multi carries NO part names (the format has no such
            // field) — the whole-setup load must CLEAR the aliases instead of
            // labelling an unrelated multi's parts with drum names.
            const juce::File mul = juce::File::getCurrentWorkingDirectory()
                                       .getChildFile ("presets/FACTORY_MULTI/000.MUL");
            if (mul.existsAsFile())
            {
                check (p.loadMultiFile (mul), "T5: factory .MUL loads");
                bool allEmpty = true;
                for (int i = 0; i < SynthEngine::getNumParts(); ++i)
                    allEmpty = allEmpty && eng.getPartName (i).isEmpty();
                check (allEmpty, "T5: .MUL load clears every part name (no stale 'Kick')");
            }
            else
                std::printf ("     (factory .MUL not found — name-clear check skipped)\n");

            // Re-loading the kit restores the names (the multi serializer
            // always emits them).
            check (p.loadParvatiMultiFile (drum), "T5: Drum Kit re-loads");
            check (eng.getPartName (0) == "Kick", "T5: names restored from the .parvati multi");

            // A SINGLE-patch load (.PRO into the current part) KEEPS the part
            // alias: the name is user metadata about the track, not the patch.
            const juce::File pro = juce::File::getCurrentWorkingDirectory()
                                       .getChildFile ("presets/FACTORY/A/000.PRO");
            if (pro.existsAsFile())
            {
                check (p.loadProgramFile (pro), "T5: .PRO loads into part 0");
                check (eng.getPartName (0) == "Kick",
                       "T5: .PRO patch load KEEPS the part name ('Kick' survives)");
            }
            else
                std::printf ("     (factory .PRO not found — name-keep check skipped)\n");
        }
        else
            std::printf ("     (Drum Kit template not found — run parvati_gen_templates)\n");
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[T6] Wave 2: undo cannot cross a part switch\n");
    {
        ParvatiAudioProcessor p;
        p.prepareToPlay (48000.0, 512);
        auto& eng = p.getEngine();

        // Part 1 (0-based 0): tweak a param so the undo stack is non-empty.
        // (Pick a value DIFFERENT from the current one — ValueTree only
        // records an undo action on an actual change.)
        p.getApvts().getParameterAsValue ("part_select") = 1.0f;
        juce::Value cutoffParam = p.getApvts().getParameterAsValue ("filter1_cutoff");
        const float cur = (float) cutoffParam.getValue();
        cutoffParam.setValue (cur == 100.0f ? 55.0f : 100.0f);
        check (p.getUndoManager().canUndo(), "T6: precondition — an edit is undoable");
        const uint8_t part0Before = eng.getPart (0).patchBytes[16];   // filter cutoff byte

        // Switch to Part 2: the switch must invalidate the history (both the
        // dump pollution and the replay-misrouting hazards). The synchronous
        // clear can leave stragglers (JUCE appends the caller's part_select
        // action AFTER its listeners return), so the REAL user entry point
        // (undoSafe — what the header button drives) must end with an empty
        // stack.
        p.getApvts().getParameterAsValue ("part_select") = 2.0f;
        check (p.getEngine().getCurrentPart() == 1,
               "T6: precondition — switched to part 2");
        const uint8_t part1Before = eng.getPart (1).patchBytes[16];
        p.undoSafe();
        check (! p.getUndoManager().canUndo(),
               "T6: undoSafe leaves no undoable action after a part switch");
        check (eng.getPart (1).patchBytes[16] == part1Before,
               "T6: undo after a part switch is a no-op (part 2 bytes untouched)");
        check (eng.getPart (0).patchBytes[16] == part0Before,
               "T6: part 1's edited byte stays edited (no phantom revert)");
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[T7] Wave 2: corrupt .parvati PATCH leaves voices alone\n");
    {
        ParvatiAudioProcessor p;
        p.prepareToPlay (48000.0, 256);
        auto& eng = p.getEngine();

        // Hold a note so a resetAllVoices would be observable.
        int activeBefore = 0;
        {
            juce::AudioBuffer<float> buf (2, 256);
            buf.clear();
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 110), 0);
            p.processBlock (buf, midi);
            for (int i = 0; i < eng.getNumVoices(); ++i)
                if (auto* av = eng.getAmbikaVoice (i); av != nullptr && av->isDisplayedActive())
                    ++activeBefore;
        }
        check (activeBefore > 0, "T7: precondition — a voice is sounding");

        // A corrupt .parvati PATCH (the multi path got validate-first; the
        // patch path used to resetAllVoices first and only then fail).
        juce::File bad = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("parvati_t7_bad.parvati");
        bad.replaceWithText ("this is not: yaml: at all: [");
        check (! p.loadParvatiPatchFile (bad), "T7: corrupt patch rejected");
        bad.deleteFile();

        int activeAfter = 0;
        {
            juce::AudioBuffer<float> buf (2, 256);
            buf.clear();
            juce::MidiBuffer midi;
            p.processBlock (buf, midi);
            for (int i = 0; i < eng.getNumVoices(); ++i)
                if (auto* av = eng.getAmbikaVoice (i); av != nullptr && av->isDisplayedActive())
                    ++activeAfter;
        }
        check (activeAfter == activeBefore,
               "T7: failed patch load does NOT kill the sounding voice");
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[T8] Wave 3: hand-edited .parvati routing values are clamped\n");
    {
        // Out-of-range hand-edited values previously stored verbatim (uint8
        // wrap): channel 17 wrapped to a channel no MIDI stream matches (the
        // part went silent), an inverted zone matched no note, and a bitmask
        // with high bits set materialized a slot count the 6-card pool cannot
        // honor. The loader now clamps to the engine's accepted ranges and
        // normalizes an inverted zone by swap.
        ParvatiAudioProcessor p;
        p.prepareToPlay (48000.0, 512);

        juce::File f = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("parvati_t8_clamp.parvati");
        f.replaceWithText (
            "format: parvati-multi\nversion: 1\nname: \"Clamp\"\nparts:\n"
            "  - channel: 17\n"
            "    keyzone_low: 100\n"
            "    keyzone_high: 20\n"
            "    voice_slots: 4\n");
        check (p.loadParvatiMultiFile (f), "T8: clamped multi loads");
        check (p.getEngine().getPartChannel (0) == 16,
               "T8: channel 17 clamps to 16 (was: uint8 wrap to a dead channel)");
        check (p.getEngine().getPartKeyrangeLow (0) == 100
                   && p.getEngine().getPartKeyrangeHigh (0) == 20,
               "T8: wrap zone 100..20 PRESERVED (firmware low>high wrap; was: normalized 20..100)");
        check (p.getEngine().getPartVoiceSlots (0) == 4,
               "T8: voice_slots still applies alongside the clamped routing");
        f.deleteFile();
    }

    // ---------------------------------------------------------------------
    std::printf ("\n[T9] W11: options: accepts ONLY option descriptors (F-state-5)\n");
    {
        // The serializer emits only isOption params under `options:`, but the
        // loader used to apply ANY APVTS paramID found there — a hand-edited
        // per-part key (osc1_shape) wrote into the PRE-LOAD current part,
        // clobbering that part's just-loaded file bytes (part_select is reset
        // to Part 0 only AFTER the options loop). Restricting to isOption
        // descriptors (minus part_select) makes out-of-contract keys inert.
        ParvatiAudioProcessor a, b;
        a.prepareToPlay (48000.0, 512);
        b.prepareToPlay (48000.0, 512);

        // Give part 3 (0-based) a DISTINCTIVE osc1 shape byte via the engine
        // (the file's part 3 will carry the same value through its params).
        // Save a full multi, then hand-inject the hostile options key.
        juce::File f = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("parvati_t9_options.parvati");
        check (a.saveParvatiMultiFile (f), "T9: reference multi saved");
        juce::String txt;
        if (juce::FileInputStream in (f); in.openedOk())
            txt = in.readEntireStreamAsString();
        // Append a hostile per-part key under the EXISTING options: block
        // (the emitter always writes one; if not, add it after `name:`).
        if (txt.contains ("options:"))
        {
            const int opos = txt.indexOf ("options:");
            const int eol  = txt.indexOfChar (opos, '\n');
            txt = txt.substring (0, eol) + "\n  osc1_shape: 5\n  part_select: 5" + txt.substring (eol);
        }
        else
            txt += "\noptions:\n  osc1_shape: 5\n  part_select: 5\n";
        f.replaceWithText (txt);

        // Make the CURRENT part (pre-load) part 3 so a mis-applied options
        // key would be observable there.
        b.getApvts().getParameterAsValue ("part_select") = 3.0f;
        const uint8_t before = b.getEngine().getPart (3).patchBytes[0];   // osc1 shape byte
        check (b.loadParvatiMultiFile (f), "T9: multi with hostile options keys loads");
        const uint8_t after = b.getEngine().getPart (3).patchBytes[0];
        std::printf ("     part-3 osc1 shape byte: %d -> %d (hostile key wrote 5)\n",
                     (int) before, (int) after);
        check (after == before,
               "T9: hostile per-part key under options: is IGNORED (no pre-load part clobber)");
        // The load resets to Part 0 regardless of the injected part_select
        // (part_select is 1-based: denormalized 1 == Part 0).
        check (juce::roundToInt (b.getApvts().getRawParameterValue ("part_select")->load()) == 1,
               "T9: injected options part_select is inert (loader still shows Part 0)");
        f.deleteFile();
    }

    std::printf ("\n%s (%d failure%s)\n",
                 g_failures ? "PRESET TEST: FAILURES" : "PRESET TEST: ALL CHECKS PASSED",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0;
}
